# Item 22: CLOG SLRU (Simple Least Recently Used) Buffer Cache

**Date:** 2026-09-05  
**Topic:** PostgreSQL Commit Log SLRU Buffer Architecture (`src/backend/access/transam/slru.c`), Shared Memory Frame Replacement, $O(1)$ In-Memory Commit Updates, and Checkpoint Flushing  
**Status:** Pre-Build Study & Specification  

---

## 1. Directory Structure & Attached Code Files

- **CLOG Header**: [`include/pg/clog.h`](../include/pg/clog.h)
- **CLOG Implementation**: [`src/clog.cpp`](../src/clog.cpp)
- **Engine Checkpoint Integration**: [`src/engine.cpp`](../src/engine.cpp)
- **Unit Test Suite**: [`tests/test_clog.cpp`](../tests/test_clog.cpp)

---

## 2. The Problem Solved

In naive implementations, every transaction commit or abort does:
1. `pager_->read_page(page_id)` (8KB disk read)
2. In-memory bit flip
3. `pager_->write_page(page_id)` (8KB disk write)

This amounts to **16KB of synchronous disk I/O on every single transaction!**

In real PostgreSQL (`src/backend/access/transam/slru.c`), transaction status is managed through the **SLRU (Simple Least Recently Used)** buffer cache:
- `pg_xact` pages are held in a pool of fixed 8KB shared memory frames (e.g. `NUM_CLOG_BUFFERS = 32`).
- Setting a transaction status is a pure **in-memory bitwise operation** ($O(1)$ time, $< 10$ nanoseconds).
- Durability is guaranteed by the Write-Ahead Log (`WALRecordType::COMMIT`). The write to CLOG on disk is deferred until dirty frames are written at **checkpoint** or evicted when the cache fills up.

```text
                                [ CLIENT TRANSACTION COMMIT ]
                                              │
                      ┌───────────────────────┴───────────────────────┐
                      ▼                                               ▼
              [ 1. Append WAL ]                               [ 2. Update CLOG ]
          WALRecordType::COMMIT                            In-Memory SLRU Cache
                   │                                                  │
          file_.sync() (fsync)                           Modify 2 bits in 8KB frame
                   │                                        Mark frame DIRTY
          DURABILITY BARRIER                                (Zero Disk I/O!)
                                                                      │
                                                        ┌─────────────┴─────────────┐
                                                        ▼                           ▼
                                                  [ Eviction ]               [ CHECKPOINT ]
                                             Flush dirty frame if needed     Flush all dirty frames
                                                                             and sync pager_
```

---

## 3. Mathematical Foundations & Binary Layout

### A. 2-Bit Status Layout
Each transaction status is encoded in exactly 2 bits:
- `0b00`: `IN_PROGRESS` (zero-initialized state)
- `0b01`: `COMMITTED`
- `0b10`: `ABORTED`
- `0b11`: `SUB_COMMITTED`

Mathematical addressing:
$$\text{page\_id} = \left\lfloor \frac{\text{tx\_id}}{32,768} \right\rfloor$$
$$\text{tx\_within\_page} = \text{tx\_id} \pmod{32,768}$$
$$\text{byte\_offset} = \left\lfloor \frac{\text{tx\_within\_page}}{4} \right\rfloor$$
$$\text{bit\_shift} = (\text{tx\_within\_page} \pmod 4) \times 2$$

### B. SLRU Cache Dimensions
- Frame size: 8,192 bytes (`PAGE_SIZE`).
- Number of frames: 32 buffers (`CLOG_SLRU_BUFFERS`).
- Total memory footprint: $32 \times 8 \text{ KB} = 256 \text{ KB}$.
- Resident capacity: $32 \times 32,768 = 1,048,576$ active transactions cached in RAM simultaneously!

### C. Frame State Invariant
```cpp
struct SlruFrame {
    page_id_t page_id{INVALID_PAGE_ID};
    bool valid{false};
    bool dirty{false};
    uint64_t lru_counter{0};
    uint8_t data[PAGE_SIZE]{};
};
```

---

## 4. Algorithms

### A. Cache Lookup & Load
1. For each frame $f \in [0, \text{num\_buffers})$:
   - If $f.\text{valid} \land f.\text{page\_id} == \text{target\_pid}$:
     - Update $f.\text{lru\_counter} = ++\text{global\_counter}$.
     - Return frame index $f$ (Cache Hit!).
2. Cache Miss: Select victim frame $v$ with minimum $v.\text{lru\_counter}$.
3. If $v.\text{dirty}$:
   - `pager_->write_page(v.page_id, v.data)`
4. `pager_->read_page(target_pid, v.data)`
5. $v.\text{page\_id} = \text{target\_pid}$; $v.\text{valid} = \text{true}$; $v.\text{dirty} = \text{false}$.
6. $v.\text{lru\_counter} = ++\text{global\_counter}$.
7. Return frame index $v$.

### B. Setting Status
1. Obtain frame index $f$ via `find_or_load_frame(page_id)`.
2. Clear and set the 2 bits at `byte_offset` with `bit_shift`.
3. Mark $f.\text{dirty} = \text{true}$.
4. Return immediately with zero disk I/O.

---

## 5. Attacking Common Industry Misconceptions

- **Misconception: "Every commit requires an fsync to both the WAL and the table/CLOG files."**
  - *Reality*: Double fsync kills database performance. WAL is the *sole* synchronous durability barrier. Modifying table and commit-log pages happens in shared memory buffers; asynchronous background writers and checkpoints flush dirty pages lazily.
- **Misconception: "If dirty CLOG frames in RAM are lost in a crash, committed transactions are lost."**
  - *Reality*: ARIES crash recovery replays the WAL from the last checkpoint. During the REDO pass, `WALRecordType::COMMIT` records are replayed and reinject the committed status into the CLOG!

---

## 6. The 3-Depth Diagnostic Ladder

1. **Level 1 (Mechanics)**: A bitmask operation `~(0b11 << shift)` followed by `|= (status << shift)` modifies the cached frame.
2. **Level 2 (Invariants)**: At any point, every dirty frame in the SLRU is protected by a prior durable WAL record (`WAL flushed_lsn >= page pd_lsn`).
3. **Level 3 (Failure Modes)**: On sudden power outage, unwritten dirty SLRU frames in RAM evaporate; startup recovery reconstructs them by scanning WAL from the checkpoint record.
