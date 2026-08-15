#include "pg/tx.h"
#include <algorithm>

namespace pg {

TransactionManager::TransactionManager() {
    // Transaction ID 0 (bootstrap / non-transactional inserts) is always COMMITTED
    status_map_[0] = TransactionStatus::COMMITTED;
}

tx_id_t TransactionManager::begin_transaction() {
    tx_id_t tx_id = next_tx_id_++;
    status_map_[tx_id] = TransactionStatus::IN_PROGRESS;
    active_txs_.insert(tx_id);
    return tx_id;
}

Snapshot TransactionManager::take_snapshot(tx_id_t tx_id) {
    Snapshot snap;
    snap.current_tx_id = tx_id;
    snap.xmax = next_tx_id_;

    if (active_txs_.empty()) {
        snap.xmin = next_tx_id_;
    } else {
        tx_id_t min_active = next_tx_id_;
        for (tx_id_t active_id : active_txs_) {
            if (active_id != tx_id) {
                min_active = std::min(min_active, active_id);
                snap.active_txs.insert(active_id);
            }
        }
        snap.xmin = min_active;
    }

    return snap;
}

void TransactionManager::commit(tx_id_t tx_id) {
    status_map_[tx_id] = TransactionStatus::COMMITTED;
    active_txs_.erase(tx_id);
}

void TransactionManager::abort(tx_id_t tx_id) {
    status_map_[tx_id] = TransactionStatus::ABORTED;
    active_txs_.erase(tx_id);
}

TransactionStatus TransactionManager::get_status(tx_id_t tx_id) const {
    if (tx_id == 0) {
        return TransactionStatus::COMMITTED;
    }
    auto it = status_map_.find(tx_id);
    if (it != status_map_.end()) {
        return it->second;
    }
    // If not recorded and smaller than next_tx_id_, assume committed
    if (tx_id < next_tx_id_) {
        return TransactionStatus::COMMITTED;
    }
    return TransactionStatus::IN_PROGRESS;
}

} // namespace pg
