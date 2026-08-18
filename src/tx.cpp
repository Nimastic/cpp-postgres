#include "pg/tx.h"
#include "pg/clog.h"
#include <algorithm>

namespace pg {

TransactionManager::TransactionManager(CLogManager* clog) : clog_(clog) {
    // Transaction ID 0 (bootstrap / non-transactional inserts) is always COMMITTED
    status_map_[0] = TransactionStatus::COMMITTED;
    if (clog_) {
        clog_->set_status(0, TransactionStatus::COMMITTED);
    }
}

tx_id_t TransactionManager::begin_transaction() {
    tx_id_t tx_id = next_tx_id_++;
    status_map_[tx_id] = TransactionStatus::IN_PROGRESS;
    active_txs_.insert(tx_id);
    if (clog_) {
        clog_->set_status(tx_id, TransactionStatus::IN_PROGRESS);
    }
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
    if (clog_) {
        clog_->set_status(tx_id, TransactionStatus::COMMITTED);
    }
}

void TransactionManager::abort(tx_id_t tx_id) {
    status_map_[tx_id] = TransactionStatus::ABORTED;
    active_txs_.erase(tx_id);
    if (clog_) {
        clog_->set_status(tx_id, TransactionStatus::ABORTED);
    }
}

tx_id_t TransactionManager::oldest_active_xmin() const {
    if (active_txs_.empty()) {
        return next_tx_id_;
    }
    tx_id_t min_id = next_tx_id_;
    for (tx_id_t id : active_txs_) {
        min_id = std::min(min_id, id);
    }
    return min_id;
}

TransactionStatus TransactionManager::get_status(tx_id_t tx_id) const {
    if (tx_id == 0) {
        return TransactionStatus::COMMITTED;
    }

    // 1. Check RAM status map
    auto it = status_map_.find(tx_id);
    if (it != status_map_.end()) {
        return it->second;
    }

    // 2. Check persistent disk CLOG bitmap if available
    if (clog_) {
        return clog_->get_status(tx_id);
    }

    // 3. Fallback: if not recorded and smaller than next_tx_id_, assume committed
    if (tx_id < next_tx_id_) {
        return TransactionStatus::COMMITTED;
    }
    return TransactionStatus::IN_PROGRESS;
}

void TransactionManager::set_status(tx_id_t tx_id, TransactionStatus status) {
    status_map_[tx_id] = status;
    if (clog_) {
        clog_->set_status(tx_id, status);
    }
    if (status != TransactionStatus::IN_PROGRESS) {
        active_txs_.erase(tx_id);
    }
    if (tx_id >= next_tx_id_) {
        next_tx_id_ = tx_id + 1;
    }
}

} // namespace pg
