# Item 5: Garbage Collection & Vacuuming

**Date:** 2026-08-16  
**Topic:** PostgreSQL Dead Tuple Detection, Global Horizon (`oldest_active_xmin`), Page Defragmentation, and Slot Reuse  
**Status:** Complete & Verified  

---

## 1. Directory Structure & Attached Code Files

- **Vacuum Engine Header**: [`include/pg/vacuum.h`](../include/pg/vacuum.h)
- **Vacuum Engine Implementation**: [`src/vacuum.cpp`](../src/vacuum.cpp)
- **Page Compaction & Defragmentation**: [`include/pg/page.h`](../include/pg/page.h) & [`src/page.cpp`](../src/page.cpp)
- **Transaction Manager Global Horizon**: [`include/pg/tx.h`](../include/pg/tx.h) & [`src/tx.cpp`](../src/tx.cpp)
- **Unit Test Suite**: [`tests/test_vacuum.cpp`](../tests/test_vacuum.cpp)

---

## 2. What Makes a Tuple "Dead" in PostgreSQL?

A row version is **not** dead simply because an update occurred. It is dead **if and only if no active transaction snapshot in the database can ever see it again**:

$$\text{Tuple is DEAD} \iff \begin{cases} 
\text{status}(\text{xmin}) = \text{ABORTED} & \text{(Aborted insert)} \\
\text{OR} \\
\text{xmax} \ne 0 \ \land \ \text{status}(\text{xmax}) = \text{COMMITTED} \ \land \ \text{xmax} < \text{oldest\_active\_xmin} & \text{(Superseded/deleted)}
\end{cases}$$

Where:
$$\text{oldest\_active\_xmin} = \min_{t \in \text{active\_snapshots}} (t.\text{xmin})$$

If an ancient transaction started hours ago is still open, $\text{oldest\_active\_xmin}$ remains pinned to that ancient transaction's ID. All intervening dead tuples are **pinned** and cannot be cleaned up, causing **Table Bloat**.

---

## 3. The 3 Phases of Vacuum & Compaction

```text
BEFORE VACUUM (Slot 1 is dead, Slot 2 is live):
+-------------------------------------------------------------------------+
| Line Pointers:                                                          |
|   Slot 1: offset=8168 (DEAD) | Slot 2: offset=8144 (NORMAL)             |
+-------------------------------------------------------------------------+
|                          FREE SPACE GAP (8118B)                         |
+-------------------------------------------------------------------------+
| Slot 2 Data: [Offset 8144 .. 8167] (24B live)                           |
| Slot 1 Data: [Offset 8168 .. 8191] (24B dead waste)                     |
+-------------------------------------------------------------------------+

AFTER VACUUM (Page compacted & defragmented):
+-------------------------------------------------------------------------+
| Line Pointers:                                                          |
|   Slot 1: DEAD (offset=0, len=0) | Slot 2: offset=8168 (NORMAL)         |
+-------------------------------------------------------------------------+
|                          FREE SPACE GAP (8142B) -> RECLAIMED +24 BYTES! |
+-------------------------------------------------------------------------+
| Slot 2 Data: [Offset 8168 .. 8191] (Shifted to top of page)             |
+-------------------------------------------------------------------------+
```

### Phase 1: Dead Slot Identification
- Vacuum iterates through line pointers $s = 1 \dots \text{num\_slots}$.
- If tuple satisfies dead condition:
  - Line pointer is marked `ItemFlags::DEAD` with `offset = 0, length = 0`.

### Phase 2: Page Defragmentation (`Page::defragment()`)
- All surviving `NORMAL` tuples are shifted towards `PAGE_SIZE` (8192).
- Surviving line pointers' `lp_offset` values are updated to point to the new byte locations.
- `pd_upper` expands downward towards `pd_lower`, creating a larger contiguous free space gap.
- **Crucial Rule**: Slot IDs ($1, 2, \dots$) are **never renumbered**, maintaining CTID stability for indexes.

### Phase 3: In-Place Slot Reuse (No Disk Growth)
- When a new tuple is inserted, `insert_tuple()` checks for existing `DEAD` line pointers.
- Slot 1 is reused: the new tuple is written into the reclaimed space, and Line Pointer 1 is updated to `NORMAL`.
- `pd_lower` does not expand, and no new disk pages need to be allocated!

---

## 4. Verification Results (`tests/test_vacuum.cpp`)

```text
--- REPRODUCING POSTGRESQL VACUUM & BLOAT PREVENTION ---
[Step 1] Tx 1: Inserting (100, $10) and committing...
[Step 2] Tx 2: Starting long-running transaction (Snapshot Pinning)...
 -> Tx 2 active. tm.oldest_active_xmin() = 2
[Step 3] Tx 3: Updating item 100 to $20 and committing...
 -> Page 0 currently has 2 physical tuples.

[Step 4] Running VACUUM while Tx 2 is still open...
 -> Vacuum scanned 1 pages, reclaimed 0 dead tuples.
 -> Verified: Slot 1 ($10) was safely preserved for Tx 2!

[Step 5] Committing Tx 2 and running VACUUM again...
 -> Tx 2 committed. tm.oldest_active_xmin() advanced to 4
 -> Vacuum scanned 1 pages, reclaimed 1 dead tuples (24 bytes).
 -> Verified: Slot 1 dead tuple was reclaimed and Slot 2 remains intact!

[Step 6] Inserting a new row (300, $99) and verifying hole reuse...
 -> New row (300, $99) landed at CTID: (0, 1)
 -> Final table contains 2 live rows:
    CTID (0, 1) -> item_id=300, price=$99
    CTID (0, 2) -> item_id=100, price=$20

[Test 2] Inserting in aborted transaction and running VACUUM...
 -> Tx 6 inserted at (0, 3) and ABORTED.
 -> VACUUM successfully identified and purged aborted transaction tuple!

>>> ITEM 5 (VACUUM & GARBAGE COLLECTION) TESTS PASSED SUCCESSFULLY! <<<
```

---

## 6. Quiz Diagnostics & Graded Mechanics

### Q1 · Dead Tuple Global Horizon Cutoff (`DEAD-TUPLE-CUTOFF`)
- **Question**: Can a tuple with `xmax = 20` be vacuumed if `oldest_active_xmin = 15`?
- **Answered**: Option 2 (No, $\text{xmax} = 20 \ge \text{oldest\_active\_xmin} (15)$) ✅
- **Mechanism**:
  Transaction 15 began before Transaction 20 committed the deletion. If Transaction 15 executes a repeatable-read or snapshot query, it still requires the old tuple version. A tuple is only dead when $\text{xmax} < \text{oldest\_active\_xmin}$.

---

### Q2 · Page Compaction & Slot Stability (`VACUUM-DEFRAGMENT`)
- **Question**: Why does `pd_upper` move during defragmentation while `pd_lower` stays fixed?
- **Answered**: Option 1 (`pd_upper` expands as live tuples are compacted towards `PAGE_SIZE`, while `pd_lower` stays fixed to keep Slot IDs stable for external CTIDs) ✅
- **Mechanism**:
  Surviving live tuples are shifted into contiguous bytes at the high end of the 8KB page, updating their `lp_offset` in the line pointer array. Keeping `pd_lower` and the slot indices fixed guarantees that external references (like B-Tree index entries pointing to `(page_id, slot_id)`) remain valid without needing to rebuild indexes!

---

### Q3 · Standard VACUUM vs VACUUM FULL (`VACUUM-VS-VACUUM-FULL`)
- **Question**: Why does standard VACUUM not shrink the file size on disk?
- **Answered**: Option 1 (Standard VACUUM frees page space for future inserts without exclusive table locks; shrinking the file requires `VACUUM FULL` which rewrites pages and locks the table) ✅
- **Mechanism**:
  Standard `VACUUM` is an online, concurrent maintenance process. It marks dead space as reusable inside existing 8KB pages so new inserts don't grow the file. Shrinking the physical OS file would require moving tuples between pages, invalidating CTIDs, rebuilding all B-Tree indexes, and acquiring an exclusive `AccessExclusiveLock`.

