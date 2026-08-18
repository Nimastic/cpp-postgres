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

## 5. Quiz Diagnostics & Graded Mechanics

### Q1 · Loser Transactions Identification (`ARIES-LOSER-TRANSACTIONS`)
- **Question**: What constitutes a "loser transaction" during the ARIES Analysis pass?
- **Answered**: Option 2 (Any transaction that wrote log records to the WAL but never logged a corresponding COMMIT or ABORT record before the end of the log) ✅
- **Mechanism**:
  The Analysis pass reconstructs the Active Transaction Table (ATT). Transactions without a terminal COMMIT or ABORT are incomplete losers that must have their physical mutations rolled back.

---

### Q2 · Compensation Log Records Idempotency (`COMPENSATION-LOG-RECORDS-CLR`)
- **Question**: What is the purpose of writing a CLR into the WAL while undoing modifications?
- **Answered**: Option 2 (To record that an undo operation was performed so that if another crash occurs mid-recovery, subsequent recovery passes will not redundantly attempt to re-undo already reverted modifications) ✅
- **Mechanism**:
  CLRs make the UNDO pass bounded and idempotent. If a secondary crash occurs during recovery, re-running recovery treats CLRs as completed compensations, avoiding infinite rollback cascading.

---

### Q3 · Repeating History Before UNDO (`REDO-REPEATING-HISTORY-BEFORE-UNDO`)
- **Question**: Why does ARIES replay uncommitted operations during REDO before rolling them back in UNDO?
- **Answered**: Option 1 (Replaying history exactly as it occurred restores the exact physical state of all pages at crash time, ensuring that the backward UNDO pass operates on deterministic, structurally consistent pages regardless of when buffer pool frames were flushed) ✅
- **Mechanism**:
  With a STEAL buffer pool policy, uncommitted writes may have already reached disk while committed writes may not have. Repeating history brings the entire disk state to the exact physical point in time of the crash before logical undo executes.

