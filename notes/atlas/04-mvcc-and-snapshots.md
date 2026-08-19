# Item 4: MVCC & Transaction Snapshot Visibility

**Confidence**: `verified`  
**Citations**: [include/pg/tx.h:1-75](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/include/pg/tx.h), [include/pg/mvcc.h:1-45](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/include/pg/mvcc.h), [src/tx.cpp:1-95](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/src/tx.cpp), [src/mvcc.cpp:1-75](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/src/mvcc.cpp), [tests/test_mvcc.cpp:1-135](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/tests/test_mvcc.cpp)

---

## 1. Multi-Version Concurrency Control (MVCC) Overview

PostgreSQL implements **Snapshot Isolation** using row-version header stamps (`xmin` and `xmax`). Readers never block writers, and writers never block readers.

```mermaid
flowchart TD
    subgraph TransactionTimeline["Transaction Horizon"]
        XMIN["xmin (Lowest Active Tx ID)\nAll Tx < xmin are COMMITTED"]
        ACTIVE["Active In-Flight Transactions\n(active_txs set)"]
        XMAX["xmax (Next Tx ID Counter)\nAll Tx >= xmax are in the FUTURE"]
    end

    V1["Tuple A\n[xmin=5, xmax=0]"] -->|xmin < Snapshot.xmax & committed| VISIBLE["VISIBLE to Reader"]
    V2["Tuple B\n[xmin=15, xmax=0]"] -->|xmin in active_txs or >= xmax| INVISIBLE["INVISIBLE to Reader"]
```

---

## 2. Invariants & Visibility State Machine

A tuple version is visible to a `Snapshot` if and only if **both** of the following conditions hold (`[src/mvcc.cpp:25]`):

1. **Creation Visibility (`xmin`)**:
   - `xmin == snapshot.current_tx_id` (created by current tx), OR
   - (`xmin < snapshot.xmax` AND `xmin` is NOT in `snapshot.active_txs` AND `status(xmin) == COMMITTED`).
2. **Deletion Invisibility (`xmax`)**:
   - `xmax == 0` (never deleted/updated), OR
   - `xmax == snapshot.current_tx_id` AND NOT yet committed, OR
   - `xmax >= snapshot.xmax` (deleted in future), OR
   - `xmax` IS in `snapshot.active_txs` (deleted by active concurrent tx), OR
   - `status(xmax) == ABORTED` (deletion was rolled back).

---

## 3. Sequence Diagram: Concurrent MVCC Reads and Writes

```mermaid
sequenceDiagram
    autonumber
    participant Tx1 as Tx 1 (Writer)
    participant Tx2 as Tx 2 (Reader)
    participant Heap as Heap Storage
    participant TM as Transaction Manager

    Tx1->>TM: begin_transaction() -> TxID: 10
    Tx2->>TM: begin_transaction() -> TxID: 11
    Tx2->>TM: take_snapshot(11) -> Snapshot (xmin 10, xmax 12, active: 10)

    Tx1->>Heap: insert(Item 100, xmin=10) -> Stored at (0, 1)
    Tx2->>Heap: seq_scan(snapshot_11)
    Note over Tx2,Heap: Reads (0, 1) [xmin=10].<br/>10 is in active_txs -> INVISIBLE!
    Heap-->>Tx2: 0 rows returned

    Tx1->>TM: commit(10)
    Note over Tx1,TM: Tx 1 marked COMMITTED in CLOG.

    participant Tx3 as Tx 3 (New Reader)
    Tx3->>TM: take_snapshot(12) -> Snapshot (xmin 12, xmax 13, active: none)
    Tx3->>Heap: seq_scan(snapshot_12)
    Note over Tx3,Heap: Reads (0, 1) [xmin=10].<br/>10 < xmin (12) and COMMITTED -> VISIBLE!
    Heap-->>Tx3: 1 row returned (Item 100)
```
