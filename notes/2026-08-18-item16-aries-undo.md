# Item 16: ARIES UNDO Crash Recovery Pass & Compensation Log Records (CLR)

**Date:** 2026-08-18  
**Topic:** 3-Phase ARIES Recovery Model (Analysis -> REDO -> UNDO), Active Transaction Table (ATT), Rolling Back Loser Transactions, and Compensation Log Records (CLRs)  
**Status:** Complete & Verified  

---

## 1. Directory Structure & Attached Code Files

- **WAL Header**: [`include/pg/wal.h`](../include/pg/wal.h)
- **ARIES 3-Phase Engine**: [`src/wal.cpp`](../src/wal.cpp)
- **Unit Test Suite**: [`tests/test_undo.cpp`](../tests/test_undo.cpp)

---

## 2. The Problems Solved

### A. The Incomplete Transaction Hazard (STEAL Buffer Pool Policy)
Under high-performance database architectures with a **STEAL** buffer policy, dirty frames belonging to active, *uncommitted* transactions can be flushed to disk if the buffer pool needs to evict pages. If the system crashes:
- On disk, pages contain partial, dirty writes from transactions that never committed!
- If the system only performs a REDO pass, those uncommitted tuples remain physically on the slotted page.

---

### B. The 3-Phase ARIES Recovery Solution

```text
 1. ANALYSIS PASS (Scan WAL forward):
    ├── Identifies last CHECKPOINT LSN
    └── Builds Active Transaction Table (ATT) = {Tx with mutations} - {COMMITTED} - {ABORTED}
                                            (The "Loser Transactions")
 2. REDO PASS (Repeating History forward):
    └── Replays all FPIs, INSERTs, UPDATEs forward to restore exact pre-crash physical state.

 3. UNDO PASS (Rolling Back Loser Transactions backwards):
    ├── Scans WAL backwards from log end
    ├── For every action by a Tx in ATT:
    │   ├── INSERT -> Zeroes line pointer (marks slot UNUSED)
    │   ├── UPDATE -> Reverts old tuple's xmax = 0, marks new slot UNUSED
    │   └── Appends Compensation Log Record (CLR) to WAL
    └── Marks all loser transactions ABORTED in CLOG and appends ABORT record to WAL.
```

---

## 3. Verification Results (`tests/test_undo.cpp`)

```text
--- REPRODUCING POSTGRESQL ARIES UNDO CRASH RECOVERY ---
[Step 1] Tx 1 commits (100, $10); Tx 2 inserts (200, $20) and crashes UNCOMMITTED...
 -> Pre-crash disk state: Page 0 has Slot 1 (Tx 1) and Slot 2 (Tx 2).
[Step 2] Restarting database engine and running 3-Phase ARIES Recovery...
 -> ARIES Recovery complete. Replayed 2 records during REDO pass.
 -> Physical verification: Slot 2 (uncommitted Tx 2) was wiped to UNUSED by UNDO pass!
 -> Transaction status verification: Tx 2 marked ABORTED in persistent CLOG.

[Step 3] Testing uncommitted UPDATE rollback during UNDO pass...
 -> Tx 3 performed UPDATE (stamped xmax=3 on Slot 1, created Slot 3 with price $999) without committing.
[Step 4] Running ARIES Recovery to rollback Tx 3...
 -> UPDATE Rollback verified: Slot 1 xmax restored to 0, Slot 2 erased to UNUSED!

>>> ITEM 16 (ARIES UNDO RECOVERY PASS) TESTS PASSED SUCCESSFULLY! <<<
```

---

## 4. Self-Check & Calibration Questions

- **Definition (`ARIES-LOSER-TRANSACTIONS`)**: What constitutes a "loser transaction" during the ARIES Analysis pass?
- **Mechanism (`COMPENSATION-LOG-RECORDS-CLR`)**: What is the purpose of writing a Compensation Log Record (CLR) into the WAL while undoing a transaction's modifications?
- **System Design (`REDO-REPEATING-HISTORY-BEFORE-UNDO`)**: Why does ARIES insist on replaying uncommitted operations during the REDO pass before subsequently rolling them back in the UNDO pass, rather than simply skipping uncommitted operations during REDO?
