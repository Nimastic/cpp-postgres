#include "pg/lock.h"
#include <iostream>
#include <sstream>
#include <algorithm>

namespace pg {

bool LockManager::acquire(const LockResource& res, tx_id_t tx_id, LockMode mode, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);

    auto& head = lock_table_[res];

    // Reentrant lock check: already held by this tx with sufficient permission
    if (head.is_held_by(tx_id, mode)) {
        return true;
    }

    // Immediate grant if compatible and no earlier waiters in the queue
    if (head.is_compatible(mode) && head.wait_queue.empty()) {
        head.granted_list.push_back({tx_id, mode, true});
        held_by_tx_[tx_id].insert(res);
        return true;
    }

    // If non-blocking try-lock was requested
    if (timeout_ms == 0) {
        return false;
    }

    // Enqueue into wait queue
    head.wait_queue.push_back({tx_id, mode, false});
    waiting_on_[tx_id] = res;

    // Check for deadlock cycle in the Wait-For Graph before blocking
    std::vector<tx_id_t> cycle;
    if (check_deadlock(tx_id, cycle)) {
        // Deadlock detected! Remove from wait queue and throw exception to abort requester
        auto& q = lock_table_[res].wait_queue;
        for (auto it = q.begin(); it != q.end(); ++it) {
            if (it->tx_id == tx_id) {
                q.erase(it);
                break;
            }
        }
        waiting_on_.erase(tx_id);
        if (head.granted_list.empty() && head.wait_queue.empty()) {
            lock_table_.erase(res);
        }

        std::ostringstream oss;
        oss << "deadlock detected: cycle in wait-for graph (";
        for (size_t i = 0; i < cycle.size(); ++i) {
            oss << "Tx " << cycle[i];
            if (i + 1 < cycle.size()) oss << " -> ";
        }
        oss << ")";
        throw DeadlockException(oss.str(), cycle);
    }

    // Wait condition: granted flag becomes true
    auto check_granted = [&]() {
        auto it = lock_table_.find(res);
        if (it == lock_table_.end()) return false;
        return it->second.is_held_by(tx_id, mode);
    };

    bool acquired = false;
    if (timeout_ms < 0) {
        cv_.wait(lock, check_granted);
        acquired = true;
    } else {
        acquired = cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), check_granted);
    }

    if (!acquired) {
        // Timed out: remove from wait_queue
        auto it = lock_table_.find(res);
        if (it != lock_table_.end()) {
            auto& q = it->second.wait_queue;
            for (auto q_it = q.begin(); q_it != q.end(); ++q_it) {
                if (q_it->tx_id == tx_id) {
                    q.erase(q_it);
                    break;
                }
            }
            if (it->second.granted_list.empty() && it->second.wait_queue.empty()) {
                lock_table_.erase(it);
            }
        }
        waiting_on_.erase(tx_id);
        return false;
    }

    return true;
}

void LockManager::wake_up_waiters() {
    for (auto& [res, head] : lock_table_) {
        while (!head.wait_queue.empty()) {
            const auto& front = head.wait_queue.front();
            if (head.is_compatible(front.mode)) {
                // Promote to granted list
                head.granted_list.push_back({front.tx_id, front.mode, true});
                held_by_tx_[front.tx_id].insert(res);
                waiting_on_.erase(front.tx_id);
                head.wait_queue.pop_front();
            } else {
                // Incompatible with current holders: preserve FIFO wait queue order
                break;
            }
        }
    }
}

void LockManager::release_all(tx_id_t tx_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 1. Remove from all granted lists
    auto it = held_by_tx_.find(tx_id);
    if (it != held_by_tx_.end()) {
        for (const auto& res : it->second) {
            auto tbl_it = lock_table_.find(res);
            if (tbl_it != lock_table_.end()) {
                auto& glist = tbl_it->second.granted_list;
                glist.erase(
                    std::remove_if(glist.begin(), glist.end(),
                                   [tx_id](const LockRequest& r) { return r.tx_id == tx_id; }),
                    glist.end()
                );
            }
        }
        held_by_tx_.erase(it);
    }

    // 2. Remove from wait_queue if waiting
    auto wait_it = waiting_on_.find(tx_id);
    if (wait_it != waiting_on_.end()) {
        const auto& res = wait_it->second;
        auto tbl_it = lock_table_.find(res);
        if (tbl_it != lock_table_.end()) {
            auto& q = tbl_it->second.wait_queue;
            for (auto q_it = q.begin(); q_it != q.end(); ++q_it) {
                if (q_it->tx_id == tx_id) {
                    q.erase(q_it);
                    break;
                }
            }
        }
        waiting_on_.erase(wait_it);
    }

    // 3. Clean up empty lock heads
    for (auto tbl_it = lock_table_.begin(); tbl_it != lock_table_.end();) {
        if (tbl_it->second.granted_list.empty() && tbl_it->second.wait_queue.empty()) {
            tbl_it = lock_table_.erase(tbl_it);
        } else {
            ++tbl_it;
        }
    }

    // 4. Promote eligible waiters and notify
    wake_up_waiters();
    cv_.notify_all();
}

tx_id_t LockManager::holder(const CTID& ctid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto res = LockResource::for_row(ctid);
    auto it = lock_table_.find(res);
    if (it != lock_table_.end() && !it->second.granted_list.empty()) {
        return it->second.granted_list.front().tx_id;
    }
    return INVALID_TX_ID;
}

size_t LockManager::locks_held() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const auto& [res, head] : lock_table_) {
        count += head.granted_list.size();
    }
    return count;
}

size_t LockManager::waiters_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return waiting_on_.size();
}

std::unordered_map<tx_id_t, std::unordered_set<tx_id_t>> LockManager::build_wait_for_graph() const {
    std::unordered_map<tx_id_t, std::unordered_set<tx_id_t>> graph;
    for (const auto& [res, head] : lock_table_) {
        if (head.wait_queue.empty() || head.granted_list.empty()) continue;

        for (const auto& waiter : head.wait_queue) {
            for (const auto& holder : head.granted_list) {
                if (waiter.tx_id != holder.tx_id) {
                    graph[waiter.tx_id].insert(holder.tx_id);
                }
            }
        }
    }
    return graph;
}

bool LockManager::dfs_cycle_detection(
    tx_id_t curr,
    const std::unordered_map<tx_id_t, std::unordered_set<tx_id_t>>& graph,
    std::unordered_map<tx_id_t, int>& color,
    std::vector<tx_id_t>& path,
    std::vector<tx_id_t>& cycle) const {

    color[curr] = 1; // Mark GRAY (on recursion stack)
    path.push_back(curr);

    auto it = graph.find(curr);
    if (it != graph.end()) {
        for (tx_id_t neighbor : it->second) {
            int neighbor_color = color[neighbor];
            if (neighbor_color == 1) {
                // Back-edge detected! Directed cycle proven
                auto cycle_start = std::find(path.begin(), path.end(), neighbor);
                cycle.assign(cycle_start, path.end());
                cycle.push_back(neighbor); // Close cycle loop
                return true;
            }
            if (neighbor_color == 0) {
                if (dfs_cycle_detection(neighbor, graph, color, path, cycle)) {
                    return true;
                }
            }
        }
    }

    color[curr] = 2; // Mark BLACK (finished exploration)
    path.pop_back();
    return false;
}

bool LockManager::check_deadlock(tx_id_t waiter_tx, std::vector<tx_id_t>& cycle_path) const {
    auto graph = build_wait_for_graph();
    std::unordered_map<tx_id_t, int> color;
    std::vector<tx_id_t> path;

    return dfs_cycle_detection(waiter_tx, graph, color, path, cycle_path);
}

} // namespace pg
