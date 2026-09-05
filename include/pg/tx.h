#pragma once

#include "pg/constants.h"
#include "pg/tuple.h"
#include <cstdint>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <memory>
#include <stdexcept>

namespace pg {

class CLogManager; // Forward declaration

enum class TransactionStatus : uint8_t {
    IN_PROGRESS = 0, // 0b00 in CLOG
    COMMITTED = 1,   // 0b01 in CLOG
    ABORTED = 2,     // 0b10 in CLOG
    SUB_COMMITTED = 3// 0b11 in CLOG
};

struct Snapshot {
    tx_id_t current_tx_id{0};                  // The transaction that owns this snapshot
    tx_id_t xmin{0};                           // All tx < xmin are FINISHED (committed or aborted)
    tx_id_t xmax{0};                           // All tx >= xmax are in the future (invisible)
    std::unordered_set<tx_id_t> active_txs{};  // Active transactions when snapshot was taken (invisible)

    bool is_active(tx_id_t tx_id) const {
        return active_txs.find(tx_id) != active_txs.end();
    }
};

class TransactionManager {
public:
    explicit TransactionManager(CLogManager* clog = nullptr);

    // Start a new transaction and obtain its ID
    tx_id_t begin_transaction();

    // Create a snapshot for a transaction
    Snapshot take_snapshot(tx_id_t tx_id);

    // Commit a transaction (writes to RAM and CLOG on disk)
    void commit(tx_id_t tx_id);

    // Abort/Rollback a transaction (writes to RAM and CLOG on disk)
    void abort(tx_id_t tx_id);

    // Query status of a transaction (checks RAM map, then falls back to persistent CLOG)
    TransactionStatus get_status(tx_id_t tx_id) const;

    // Set status of a transaction (used during WAL crash recovery / CLOG sync)
    void set_status(tx_id_t tx_id, TransactionStatus status);

    // Lowest transaction id still running. Not a safe VACUUM cutoff on its own.
    tx_id_t oldest_active_xmin() const;

    // Lowest xmin across every snapshot still held by a running transaction.
    // This is the VACUUM horizon (PostgreSQL's GetOldestNonRemovableTransactionId):
    // a transaction holding an old snapshot can still need row versions deleted
    // by transactions numbered below its own id, so the cutoff must come from
    // the snapshots, not from the transaction ids.
    tx_id_t oldest_snapshot_xmin() const;

    // Register/forget the snapshot a running transaction holds, so the horizon
    // above can be computed. PostgreSQL publishes this as PGPROC.xmin.
    void register_snapshot(tx_id_t tx_id, tx_id_t snapshot_xmin);
    void forget_snapshot(tx_id_t tx_id);

    size_t active_count() const { return active_txs_.size(); }

    // Get current global transaction ID counter
    tx_id_t next_tx_id() const { return next_tx_id_; }
    void set_next_tx_id(tx_id_t id) { next_tx_id_ = id; }

    // Wire persistent CLOG manager
    void set_clog(CLogManager* clog) { clog_ = clog; }
    CLogManager* clog() const { return clog_; }

private:
    tx_id_t next_tx_id_{1};
    CLogManager* clog_{nullptr};
    std::unordered_map<tx_id_t, TransactionStatus> status_map_;
    std::unordered_set<tx_id_t> active_txs_;
    std::unordered_map<tx_id_t, tx_id_t> snapshot_xmins_;  // tx_id -> xmin of the snapshot it holds
};

} // namespace pg
