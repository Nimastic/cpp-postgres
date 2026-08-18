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
    tx_id_t xmin{0};                           // All tx < xmin are committed and visible
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

    // Returns the lowest xmin among all currently active snapshots (or next_tx_id if none active)
    tx_id_t oldest_active_xmin() const;

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
};

} // namespace pg
