# Item 12: Buffer Pool Integration (Wiring All I/O Through Shared Buffers)

**Date:** 2026-08-18  
**Topic:** Routing all HeapFile, Scan, Insert, Update, and VACUUM I/O through `BufferPoolManager` (`shared_buffers`), Single Gateway to Disk Invariant  
**Status:** Complete & Verified  

---

## 1. Directory Structure & Attached Code Files

- **Heap Header (BPM Routing)**: [`include/pg/heap.h`](../include/pg/heap.h)
- **Heap Implementation (BPM Pin/Unpin)**: [`src/heap.cpp`](../src/heap.cpp)
- **Engine Constructor (Wiring)**: [`src/engine.cpp`](../src/engine.cpp)
- **Integration Test Suite**: [`tests/test_buffer_integration.cpp`](../tests/test_buffer_integration.cpp)

---

## 2. The Problem Solved

In Items 1–11, `BufferPoolManager` existed as an isolated subsystem in Item 8 tests, while `HeapFile` and `Engine` called `Pager::read_page()` and `Pager::write_page()` directly.

In real PostgreSQL, **no subsystem ever reads or writes disk pages directly**. All page reads, writes, scans, inserts, updates, and vacuum operations pass through `shared_buffers` as the **single gateway to disk**:

```text
               CLIENT QUERY / DML / VACUUM
                          │
                          ▼
                    +------------+
                    |  HeapFile  |
                    +------------+
                          │
                          ▼
            +───────────────────────────+
            |    BufferPoolManager      |
            |     (shared_buffers)      |
            +───────────────────────────+
                 │                 ▲
      (Cache Hit)│      (Cache Miss / Writeback)
                 ▼                 │
            [RAM Frame]      +────────────+
                             |   Pager    |
                             +────────────+
                                   │
                                   ▼
                              [disk.db]
```

---

## 3. Implementation Details

1. **`HeapFile` Constructor & Dependency Injection**:
   - `HeapFile` accepts an optional `BufferPoolManager* bpm` (default `nullptr` for backwards compatibility).
   - In `Engine`, `heap_->set_bpm(bpm_.get())` is called upon startup.

2. **Internal Read/Write Helpers**:
   - `read_page_internal(pid, buffer)`:
     - Fetches page via `bpm_->fetch_page(pid)` (pins frame in RAM).
     - Copies bytes and immediately unpins (`is_dirty = false`).
   - `write_page_internal(pid, page)`:
     - Fetches page via `bpm_->fetch_page(pid)`.
     - Copies modified bytes into frame memory and unpins with `is_dirty = true`.

3. **Automatic Delayed Writeback**:
   - Dirty pages remain resident in RAM.
   - Flushed to disk only upon:
     - Clock-Sweep frame eviction (when pool is full).
     - Explicit `flush_all()` (e.g. during engine shutdown or `DUMP PAGE`).

---

## 4. Verification Results (`tests/test_buffer_integration.cpp`)

```text
--- BUFFER POOL INTEGRATION (ALL HEAP I/O THROUGH SHARED_BUFFERS) ---
[Step 1] Inserting 50 rows through buffer-pool-integrated heap...
 -> Buffer pool has 1 resident pages after 50 inserts.
[Step 2] Reading item 100 via B-Tree index (should hit buffer pool)...
(1 row returned via B-Tree Index Scan (Key: 100))
[Step 3] Sequential scan of all 50 rows through buffer pool...
 -> Sequential scan returned all 50 rows via buffer pool.
[Step 4] Page dump through buffer pool...
 -> Page dump successful through buffer pool integration.
 -> STATUS report includes buffer pool metrics.

[Step 5] Restarting engine from disk files...
 -> Data persisted to disk via buffer pool writeback and survived restart!
 -> All 50 rows verified after engine restart.

[Step 6] Testing HOT update through buffer pool...
[Tx 53] UPDATE: HOT-update successful! Placed at (0, 51) on SAME page (WAL LSN: 4770). ZERO index writes!
 -> HOT update verified through buffer pool integration.

[Step 7] Running VACUUM through buffer pool integration...
[VACUUM] Garbage collection complete (Cutoff oldest_active_xmin=53).
 -> VACUUM executed through buffer pool integration.

>>> ITEM 12 (BUFFER POOL INTEGRATION) TESTS PASSED SUCCESSFULLY! <<<
```

---

## 5. Self-Check & Calibration Questions

- **Definition (`BUFFER-POOL-GATEWAY`)**: Why must all storage engine subsystems route page access through `shared_buffers` rather than reading directly from disk files?
- **Mechanism (`PIN-UNPIN-DIRTY-LIFECYCLE`)**: Trace the exact pin count and dirty flag transitions when `HeapFile::insert()` places a new tuple on Page 0 through the buffer pool.
- **System Design (`DELAYED-WRITEBACK-THROUGHPUT`)**: How does buffering dirty pages in RAM reduce total disk I/O when 1,000 consecutive updates occur on the same 8KB page?
