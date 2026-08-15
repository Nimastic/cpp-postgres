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

enum class TransactionStatus : uint8_t {
    IN_PROGRESS = 0,
    COMMITTED = 1,
    ABORTED = 2
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
    TransactionManager();

    // Start a new transaction and obtain its ID
    tx_id_t begin_transaction();

    // Create a snapshot for a transaction
    Snapshot take_snapshot(tx_id_t tx_id);

    // Commit a transaction
    void commit(tx_id_t tx_id);

    // Abort/Rollback a transaction
    void abort(tx_id_t tx_id);

    // Query status of a transaction
    TransactionStatus get_status(tx_id_t tx_id) const;

    // Get current global transaction ID counter
    tx_id_t next_tx_id() const { return next_tx_id_; }

private:
    tx_id_t next_tx_id_{1};
    std::unordered_map<tx_id_t, TransactionStatus> status_map_;
    std::unordered_set<tx_id_t> active_txs_;
};

} // namespace pg
