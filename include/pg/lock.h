#pragma once

#include "pg/tuple.h"
#include "pg/tx.h"
#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <stdexcept>
#include <optional>
#include <algorithm>

namespace pg {

// Lock Modes matching PostgreSQL src/include/storage/lock.h
enum class LockMode : uint8_t {
    SHARED,     // Shared lock (S) - multiple readers compatible
    EXCLUSIVE   // Exclusive lock (X) - single writer incompatible with all
};

inline const char* lock_mode_to_string(LockMode mode) {
    switch (mode) {
        case LockMode::SHARED: return "SHARED";
        case LockMode::EXCLUSIVE: return "EXCLUSIVE";
    }
    return "UNKNOWN";
}

// Exception raised when a deadlock cycle is detected in the Wait-For Graph
class DeadlockException : public std::runtime_error {
public:
    explicit DeadlockException(const std::string& msg, const std::vector<tx_id_t>& cycle = {})
        : std::runtime_error(msg), cycle_(cycle) {}

    const std::vector<tx_id_t>& cycle() const { return cycle_; }

private:
    std::vector<tx_id_t> cycle_;
};

// Lockable Resource Identification (Row or Relation level)
struct LockResource {
    enum class Type : uint8_t { ROW, RELATION } type{Type::ROW};
    uint32_t rel_id{0};
    CTID ctid{};

    static LockResource for_row(const CTID& ctid, uint32_t rel_id = 0) {
        LockResource res;
        res.type = Type::ROW;
        res.rel_id = rel_id;
        res.ctid = ctid;
        return res;
    }

    static LockResource for_relation(uint32_t rel_id) {
        LockResource res;
        res.type = Type::RELATION;
        res.rel_id = rel_id;
        res.ctid = CTID{};
        return res;
    }

    bool operator==(const LockResource& o) const {
        return type == o.type && rel_id == o.rel_id && ctid == o.ctid;
    }

    uint64_t hash_key() const {
        // Encode into 64-bit key
        uint64_t h = (static_cast<uint64_t>(type) << 56) ^ (static_cast<uint64_t>(rel_id) << 32);
        h ^= (static_cast<uint64_t>(ctid.page) << 16) | static_cast<uint64_t>(ctid.slot);
        return h;
    }
};

struct LockResourceHash {
    size_t operator()(const LockResource& r) const {
        return static_cast<size_t>(r.hash_key());
    }
};

// Individual Lock Request in a lock queue
struct LockRequest {
    tx_id_t tx_id{INVALID_TX_ID};
    LockMode mode{LockMode::EXCLUSIVE};
    bool granted{false};
};

// Lock Head per resource: holds list of current holders and FIFO wait queue
struct LockHead {
    std::vector<LockRequest> granted_list;
    std::deque<LockRequest> wait_queue;

    bool is_compatible(LockMode mode) const {
        if (granted_list.empty()) return true;
        if (mode == LockMode::EXCLUSIVE) return false;
        // Requested mode is SHARED: all existing granted holders must be SHARED
        for (const auto& req : granted_list) {
            if (req.mode == LockMode::EXCLUSIVE) return false;
        }
        return true;
    }

    bool is_held_by(tx_id_t tx_id, LockMode mode) const {
        for (const auto& req : granted_list) {
            if (req.tx_id == tx_id) {
                if (mode == LockMode::SHARED || req.mode == LockMode::EXCLUSIVE) {
                    return true;
                }
            }
        }
        return false;
    }

    bool has_any_holder(tx_id_t tx_id) const {
        for (const auto& req : granted_list) {
            if (req.tx_id == tx_id) return true;
        }
        return false;
    }
};

// Central Lock Manager implementing Strict Two-Phase Locking (2PL) and
// Wait-For Graph (WFG) Deadlock Detection.
class LockManager {
public:
    LockManager() = default;
    ~LockManager() = default;

    // Non-copyable, non-movable due to synchronization primitives
    LockManager(const LockManager&) = delete;
    LockManager& operator=(const LockManager&) = delete;

    // Acquire lock on a resource for tx_id.
    // If timeout_ms == 0, returns immediately without waiting (try_lock).
    // If timeout_ms > 0 or -1 (infinite), blocks until granted or timeout / deadlock detected.
    bool acquire(const LockResource& res, tx_id_t tx_id, LockMode mode, int timeout_ms = -1);

    // Convenience overload for row locks (CTID)
    bool acquire(const CTID& ctid, tx_id_t tx_id, LockMode mode = LockMode::EXCLUSIVE, int timeout_ms = -1) {
        return acquire(LockResource::for_row(ctid), tx_id, mode, timeout_ms);
    }

    // Try-lock for backwards compatibility
    bool try_lock(const CTID& ctid, tx_id_t tx_id, LockMode mode = LockMode::EXCLUSIVE) {
        return acquire(LockResource::for_row(ctid), tx_id, mode, 0);
    }

    // Release all locks held by a transaction (invoked upon COMMIT or ROLLBACK)
    void release_all(tx_id_t tx_id);

    // Query primary holder of a row lock
    tx_id_t holder(const CTID& ctid) const;

    // Total active locks held across all resources
    size_t locks_held() const;

    // Total waiting requests across all resources
    size_t waiters_count() const;

    // Build the dynamic Wait-For Graph (waiter -> set of holders)
    std::unordered_map<tx_id_t, std::unordered_set<tx_id_t>> build_wait_for_graph() const;

    // Check if adding an edge from waiter to holder would form a cycle in the WFG.
    // If a cycle is detected, returns true and populates cycle_path with the loop vertices.
    bool check_deadlock(tx_id_t waiter_tx, std::vector<tx_id_t>& cycle_path) const;

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;

    // Resource hash table -> LockHead
    std::unordered_map<LockResource, LockHead, LockResourceHash> lock_table_;

    // Reverse index: tx_id -> set of resources held
    std::unordered_map<tx_id_t, std::unordered_set<LockResource, LockResourceHash>> held_by_tx_;

    // Transactions currently waiting: tx_id -> resource waiting for
    std::unordered_map<tx_id_t, LockResource> waiting_on_;

    // Internal deadlock detection using 3-color DFS
    bool dfs_cycle_detection(tx_id_t curr,
                             const std::unordered_map<tx_id_t, std::unordered_set<tx_id_t>>& graph,
                             std::unordered_map<tx_id_t, int>& color,
                             std::vector<tx_id_t>& path,
                             std::vector<tx_id_t>& cycle) const;

    // Wake up eligible waiters after release
    void wake_up_waiters();
};

} // namespace pg
