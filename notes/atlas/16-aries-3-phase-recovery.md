# Item 16: ARIES 3-Phase Crash Recovery & UNDO Pass

**Confidence**: `verified`  
**Citations**: [include/pg/wal.h:70-95](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/include/pg/wal.h), [src/wal.cpp:160-240](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/src/wal.cpp), [tests/test_undo.cpp:1-135](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/tests/test_undo.cpp)

---

## 1. The 3-Phase ARIES Recovery Algorithm

PostgreSQL employs the **ARIES (Algorithms for Recovery and Isolation Exploiting Semantics)** model to restore consistency after an unexpected system crash or power outage:

```mermaid
flowchart TD
    subgraph ARIES["ARIES 3-Phase Recovery Pipeline"]
        P1["Phase 1: Analysis Pass (Forward Scan)\n• Scans from Checkpoint to EOF\n• Identifies Active Transaction Table (ATT)\n• Finds Loser (Uncommitted) Transactions"]
        P2["Phase 2: REDO Pass (Repeating History)\n• Replays all logged operations (both winners & losers)\n• Restores exact state of database at moment of crash"]
        P3["Phase 3: UNDO Pass (Backward Scan)\n• Scans backward rolling back active loser transactions\n• Restores modified tuples & stamps xmax=0\n• Writes Compensation Log Records (CLRs)\n• Marks loser transactions ABORTED in CLOG"]
    end

    P1 --> P2 --> P3
```

---

## 2. Invariants & Compensation Log Records (CLRs)

1. **Repeating History**: REDO is executed unconditionally for all transactions (committed and uncommitted) before UNDO runs. This guarantees that uncommitted mutations, page splits, and allocations are brought into physical memory before being rolled back (`[src/wal.cpp:190]`).
2. **Compensation Log Record (CLR)**: When an operation is rolled back during the UNDO pass, a `CLR` (`type = 7`) is written to WAL with an `undo_next_lsn` pointer. CLRs are **never undone**, preventing infinite recovery loops if the system crashes during recovery itself (`[src/wal.cpp:215]`).
3. **CLOG Status Finalization**: Once an uncommitted transaction is rolled back, its status is finalized to `ABORTED` (`0x02`) in CLOG.

---

## 3. Sequence Diagram: ARIES Crash Recovery Walkthrough

```mermaid
sequenceDiagram
    autonumber
    participant System as Crash Recovery Engine
    participant WAL as WAL Log Stream
    participant ATT as Active Transaction Table (RAM)
    participant Heap as Heap Storage
    participant CLOG as Commit Log (CLOG)

    Note over System,CLOG: Phase 1: Analysis Pass
    System->>WAL: Forward scan from Checkpoint to EOF
    WAL-->>ATT: Tx 1 = COMMITTED (Winner)<br/>Tx 2 = IN_PROGRESS (Loser)

    Note over System,CLOG: Phase 2: REDO Pass (Repeating History)
    System->>WAL: Replay Tx 1 Insert -> Applied to Page 0
    System->>WAL: Replay Tx 2 Update -> Applied to Page 0 (Price $999)

    Note over System,CLOG: Phase 3: UNDO Pass (Rolling Back Losers)
    System->>WAL: Scan backward for Tx 2 actions
    System->>Heap: Rollback Update -> Restore Slot 1 price ($10) & xmax=0
    System->>WAL: log_clr(tx_id=2, undone_lsn)
    System->>CLOG: set_status(tx_id=2, ABORTED)
    Note over System,CLOG: Database fully consistent!
```
