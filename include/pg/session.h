#pragma once

#include "pg/tuple.h"
#include "pg/tx.h"
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <string>

namespace pg {

// Per-connection state.
//
// Transaction state belongs to a session, not to the engine. When the engine
// holds a single current transaction, two clients share it: one client's BEGIN
// puts the other inside its transaction and one client's COMMIT commits the
// other's writes. It also means no two snapshots ever coexist, so the MVCC
// visibility rules never meet the concurrency they exist for.
struct Session {
    uint64_t                id{0};
    std::optional<tx_id_t>  current_tx;
    std::optional<Snapshot> snapshot;

    // A transaction block that hit an error rejects everything until ROLLBACK.
    // This is the 'E' state the wire protocol reports and psql shows as '!'.
    bool failed{false};

    bool in_transaction() const { return current_tx.has_value(); }

    void clear_transaction() {
        current_tx.reset();
        snapshot.reset();
        failed = false;
    }
};

// Row-level write-write conflict detection.
//
// MVCC removes the need for read locks: a reader sees its snapshot and never
// blocks. It does not remove the need to serialise two writers on the same row.
// PostgreSQL makes the second writer wait on the first transaction's lock and
// then re-evaluates; with a single-threaded executor there is nobody to wait
// for, so the conflict is reported instead of being silently lost.
class LockManager {
public:
    // Try to take the write lock on a row version. Re-locking a row this
    // transaction already holds succeeds.
    bool try_lock(const CTID& ctid, tx_id_t tx_id) {
        auto key = encode(ctid);
        auto it = holders_.find(key);
        if (it == holders_.end()) {
            holders_[key] = tx_id;
            held_by_[tx_id].insert(key);
            return true;
        }
        return it->second == tx_id;
    }

    tx_id_t holder(const CTID& ctid) const {
        auto it = holders_.find(encode(ctid));
        return it == holders_.end() ? INVALID_TX_ID : it->second;
    }

    // Locks live exactly as long as the transaction; both COMMIT and ROLLBACK
    // release them.
    void release_all(tx_id_t tx_id) {
        auto it = held_by_.find(tx_id);
        if (it == held_by_.end()) return;
        for (uint64_t key : it->second) {
            holders_.erase(key);
        }
        held_by_.erase(it);
    }

    size_t locks_held() const { return holders_.size(); }

private:
    static uint64_t encode(const CTID& ctid) {
        return (static_cast<uint64_t>(ctid.page) << 16) | static_cast<uint64_t>(ctid.slot);
    }

    std::unordered_map<uint64_t, tx_id_t> holders_;
    std::unordered_map<tx_id_t, std::unordered_set<uint64_t>> held_by_;
};

} // namespace pg
