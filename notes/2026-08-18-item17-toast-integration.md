# Item 17: TOAST Heap + Index Full Integration

**Date:** 2026-08-18  
**Topic:** PostgreSQL TOAST Oversized-Attribute Storage Integration (`HEAP_HASEXTERNAL`), Automatic 2KB Chunking, Auxiliary Relation Storage, and B-Tree Index Routing  
**Status:** Complete & Verified  

---

## 1. Directory Structure & Attached Code Files

- **Tuple Header (`HEAP_HASEXTERNAL`)**: [`include/pg/tuple.h`](../include/pg/tuple.h)
- **TOAST Subsystem**: [`include/pg/toast.h`](../include/pg/toast.h) and [`src/toast.cpp`](../src/toast.cpp)
- **Engine TOAST Integration**: [`include/pg/engine.h`](../include/pg/engine.h) and [`src/engine.cpp`](../src/engine.cpp)
- **Unit Test Suite**: [`tests/test_toast_integration.cpp`](../tests/test_toast_integration.cpp)

---

## 2. The Architecture Solved

### A. The 8KB Page Boundary vs Large Attributes
PostgreSQL slotted pages are strictly 8KB. A single database row cannot exceed 8KB directly on a page. When tables store text documents, JSON, or media payloads:
1. **Inline Storage ($ \le 2048$ bytes)**: The payload is kept inline inside the main heap tuple.
2. **Out-of-Line TOAST Storage ($ > 2048$ bytes)**:
   - The large payload is sliced into **2KB chunks** with `(toast_id, chunk_seq)`.
   - The chunks are stored across dedicated 8KB pages in the auxiliary TOAST table.
   - The main heap tuple stores an 18-byte `ToastPointer` (`toast_id`, `raw_size`, `chunk_count`) and flags `HEAP_HASEXTERNAL` in its header `infomask`.
   - The B-Tree secondary index indexes only the search key (e.g. `item_id`), keeping index pages small and fast.

```text
 Main Heap Table Page (8KB)
 ┌──────────────────────────────────────────────────────────┐
 │ Slot 1: [xmin=2, xmax=0, HEAP_HASEXTERNAL]               │
 │         item_id = 200, price = $20                       │
 │         ToastPointer: {toast_id=1, raw_size=10000, cnt=5}│
 └─────────────────────────┬────────────────────────────────┘
                           │
                           ▼
 Auxiliary TOAST Relation Pages (8KB)
 ┌──────────────────────────────────────────────┐
 │ Chunk 0: (toast_id=1, seq=0) -> 2048 bytes   │
 │ Chunk 1: (toast_id=1, seq=1) -> 2048 bytes   │
 │ Chunk 2: (toast_id=1, seq=2) -> 2048 bytes   │
 │ Chunk 3: (toast_id=1, seq=3) -> 2048 bytes   │
 │ Chunk 4: (toast_id=1, seq=4) -> 1808 bytes   │
 └──────────────────────────────────────────────┘
```

---

## 3. Verification Results (`tests/test_toast_integration.cpp`)

```text
--- REPRODUCING POSTGRESQL TOAST + HEAP + INDEX INTEGRATION ---
[Step 1] Inserting small inline document (<2KB)...
[Tx 1] INSERT (WITH TOAST): Landed at CTID (0, 1) (xmin=1, price=$10). Document size: 55 bytes (INLINE in tuple).
[Tx 1] COMMIT: Logged to WAL (LSN: 59). Transaction committed.
[SELECT TOAST] item_id=100, price=$10, CTID=(0, 1) [INLINE ATTRIBUTE]
 -> Verified: Small payload stored inline without auxiliary TOAST table write.

[Step 2] Inserting oversized document (10,000 bytes -> 5 chunks of 2KB)...
[Tx 2] INSERT (WITH TOAST): Landed at CTID (0, 2) (xmin=2, price=$20). Document size: 10000 bytes (OUT-OF-LINE in TOAST table, 5 chunks of 2KB, ToastID: 1).
[Tx 2] COMMIT: Logged to WAL (LSN: 153). Transaction committed.
 -> TOAST manager confirmed 5 auxiliary 2KB chunks stored in toast relation.
[SELECT TOAST] item_id=200, price=$20, CTID=(0, 2) [TOASTED ATTRIBUTE PRESENT]
 -> Verified: Heap tuple header has HEAP_HASEXTERNAL infomask bit flag set.

[Step 3] Testing SQL REPL INSERT with document...
[Tx 3] INSERT (WITH TOAST): Landed at CTID (0, 3) (xmin=3, price=$30). Document size: 30 bytes (INLINE in tuple).
[Tx 3] COMMIT: Logged to WAL (LSN: 247). Transaction committed.

+---------+-------+-------+-------+--------+
| item_id | price | xmin  | xmax  | CTID   |
+---------+-------+-------+-------+--------+
|     300 | $   30 |     3 |     0 | (0, 3) |
+---------+-------+-------+-------+--------+
(1 row returned via B-Tree Index Scan (Key: 300))
[Step 4] Restarting engine and verifying TOAST persistence from disk...

+---------+-------+-------+-------+--------+
| item_id | price | xmin  | xmax  | CTID   |
+---------+-------+-------+-------+--------+
|     100 | $   10 |     1 |     0 | (0, 1) |
|     200 | $   20 |     2 |     0 | (0, 2) |
|     300 | $   30 |     3 |     0 | (0, 3) |
+---------+-------+-------+-------+--------+
(3 rows returned via Sequential Table Scan)
 -> All rows and TOASTed tuples verified surviving restart across disk files!

>>> ITEM 17 (TOAST HEAP + INDEX INTEGRATION) TESTS PASSED SUCCESSFULLY! <<<
```

---

## 5. Quiz Diagnostics & Graded Mechanics

### Q1 · TOAST External Storage Flag (`TOAST-HEAP-HASEXTERNAL`)
- **Question**: What does the `HEAP_HASEXTERNAL` infomask flag signify on a heap tuple header?
- **Answered**: Option 2 (The tuple contains one or more oversized attributes stored out-of-line in the auxiliary TOAST table and replaced in the main heap tuple by a ToastPointer) ✅
- **Mechanism**:
  `HEAP_HASEXTERNAL` instructs the tuple deconstruction engine to inspect attribute offsets for 18-byte `ToastPointer` values rather than parsing inline raw bytes.

---

### Q2 · Index Compactness via TOAST Chunking (`TOAST-CHUNKED-REASSEMBLY`)
- **Question**: How does PostgreSQL prevent oversized attributes from causing index bloat or reducing tuples per page?
- **Answered**: Option 1 (By storing only an 18-byte ToastPointer inside the 8KB heap tuple and indexing only search keys in the B-Tree index, while slicing the payload into 2KB chunks in the auxiliary TOAST table) ✅
- **Mechanism**:
  Separating oversized attributes prevents physical page overflow and allows secondary B-Tree indices to store only compact scalar keys, keeping tree height shallow and lookups fast.

---

### Q3 · Sequential Scan I/O Efficiency (`TOAST-READ-AMPLIFICATION-BENEFIT`)
- **Question**: Why is isolating oversized data into an auxiliary TOAST table beneficial when performing sequential scans?
- **Answered**: Option 1 (Queries that don't project the large column only scan dense, compact 8KB heap pages holding hundreds of rows per page without loading or reading large TOAST pages into shared buffers) ✅
- **Mechanism**:
  Heap pages remain dense and narrow. Cold oversized data is never fetched into `shared_buffers` unless explicitly requested by the SQL projection list.

