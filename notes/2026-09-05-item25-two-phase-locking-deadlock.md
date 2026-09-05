# Pre-Build Study: Milestone 25 - Two-Phase Locking (2PL) & Wait-For Graph Deadlock Detection

**Date**: 2026-09-05  
**Author**: Antigravity  
**Target Milestone**: Milestone 25 (LockManager 2PL, Lock Queues, Wait-For Graph DFS Cycle Detection, Automated Victim Abort)  
**Status**: Pre-Build Exploration & Architectural Blueprint

---

## 1. Context & Motivation

In classical MVCC (Multi-Version Concurrency Control), readers do not block writers, and writers do not block readers. However, concurrent writers targeting the same physical row versions (write-write conflicts) must be strictly serialized to prevent lost updates and anomalies.

In the current codebase (`include/pg/session.h:44-84`), `LockManager` uses a simple non-blocking `try_lock()` map. If a row is already locked by another transaction, it immediately aborts the second writer with `"could not serialize access"`.

While safe, this non-blocking design has critical limitations:
1. **No Concurrency Queueing**: In PostgreSQL, a concurrent writer does not immediately fail; it waits on the row lock held by the earlier transaction (`src/backend/storage/lmgr/lock.c`). Once the first transaction commits or aborts, waiting transactions are awakened in FIFO or priority order.
2. **No Lock Granularity or Modes**: Current locks are untyped write locks. Strict 2PL requires explicit Shared (S) and Exclusive (X) lock modes with a formal Lock Compatibility Matrix.
3. **No Deadlock Detection**: When transactions wait on each other (e.g., $T_1$ holds row $A$ and requests row $B$, while $T_2$ holds row $B$ and requests row $A$), the system enters a mutual deadlock. Without a **Wait-For Graph (WFG)** and cycle detection, the system hangs indefinitely.

PostgreSQL handles this in `src/backend/storage/lmgr/deadlock.c` by maintaining a lock wait queue and periodically running a topological cycle detection check over the Wait-For Graph. If a directed cycle is detected, one transaction is selected as the victim and aborted with error code `40P01 (deadlock_detected)`.

---

## 2. PostgreSQL Architectural Reference

In PostgreSQL (`src/backend/storage/lmgr/`):
- `PROCLOCK`: Association between a transaction (`PGPROC`) and a lockable resource (`LOCK`).
- `LOCKMETHOD`: Defines lock modes and compatibility matrix:
  ```
               Existing Lock Mode
  Request:       Shared (S)     Exclusive (X)
  Shared (S)       GRANT            WAIT
  Exclusive (X)    WAIT             WAIT
  ```
- `DeadLockCheck()` (`deadlock.c`): Traverses the wait-for edges $T_{waiter} \to T_{holder}$ using Depth-First Search (DFS). If a node currently in the DFS recursion stack (colored `GRAY`) is visited again, a cycle is proven.
- **Victim Selection**: Aborts the transaction that closed the cycle or the youngest transaction ($XID_{victim} = \max(XID \in \text{Cycle})$).

---

## 3. Detailed Design for Milestone 25

### 3.1 Lock Modes & Resource ID
```cpp
enum class LockMode : uint8_t {
    SHARED,     // S-lock (e.g., SELECT FOR SHARE, read locks)
    EXCLUSIVE   // X-lock (e.g., UPDATE, DELETE, SELECT FOR UPDATE)
};

struct LockResource {
    enum class Type : uint8_t { ROW, RELATION } type{Type::ROW};
    uint32_t rel_id{0};
    CTID ctid{};

    bool operator==(const LockResource& o) const = default;
};
```

### 3.2 Lock Request & Queue Entry
```cpp
struct LockRequest {
    tx_id_t tx_id;
    LockMode mode;
    bool granted{false};
};

struct LockHead {
    std::vector<LockRequest> granted_list;
    std::deque<LockRequest> wait_queue;
};
```

### 3.3 Wait-For Graph (WFG) & DFS Cycle Detection
```cpp
// Directed adjacency list: waiter_tx -> set of holder_tx
std::unordered_map<tx_id_t, std::unordered_set<tx_id_t>> build_wait_for_graph() const;

// 3-Color DFS Cycle Detection:
// WHITE = unvisited, GRAY = on current recursion stack, BLACK = fully explored
bool detect_deadlock(tx_id_t start_tx, std::vector<tx_id_t>& cycle);
```

### 3.4 Strict Two-Phase Locking (2PL) Protocol
- **Growing Phase**: Transactions acquire S or X locks on resources during statement execution.
- **Shrinking Phase**: All locks acquired by a transaction are held until commit or abort (`release_all(tx_id)`). This guarantees Strict 2PL (rigorous schedule avoiding cascading aborts).

### 3.5 Blocking vs. Non-blocking & Timeout
- `acquire(resource, tx_id, mode, timeout_ms)`:
  - If compatible with current holders and no prior waiters, grant immediately.
  - If incompatible, register in `wait_queue`, construct WFG edge `tx_id -> current_holders`.
  - Check for deadlock via `detect_deadlock()`.
  - If a cycle is formed, abort the requester with `DeadlockException ("deadlock detected: cycle in wait-for graph")`.
  - Otherwise, wait on `std::condition_variable` until awakened or timeout expires.

---

## 4. Verification & Testing Strategy

We will build a comprehensive test suite `tests/test_locks.cpp`:
1. **Lock Compatibility Matrix Verification**:
   - S/S concurrency: Multiple transactions concurrently hold Shared locks on the same resource.
   - S/X conflict: X-lock is blocked when S-lock is held.
   - X/X conflict: X-lock is blocked when X-lock is held.
2. **Lock Queue FIFO Ordering**:
   - Waiters are granted access in strict FIFO order when the existing holder commits.
3. **Wait-For Graph Deadlock Detection**:
   - Classic 2-Tx deadlock: $T_1$ holds $R_A$, waits for $R_B$. $T_2$ holds $R_B$, waits for $R_A$.
   - Multi-Tx circular deadlock: $T_1 \to T_2 \to T_3 \to T_1$.
   - Verify that DFS cycle detector isolates the exact cycle and aborts the victim transaction cleanly.
4. **Strict 2PL Lifecycle**:
   - Verify that committing or aborting a transaction releases all held locks, instantly granting locks to waiting transactions.
5. **Integration with Engine & Concurrent Sessions**:
   - Test through `Engine` and `Session` instances across concurrent worker threads (`std::thread`).
