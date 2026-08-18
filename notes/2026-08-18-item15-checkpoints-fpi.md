# Item 15: WAL Checkpoints & Full-Page Images (FPI)

**Date:** 2026-08-18  
**Topic:** PostgreSQL Checkpoint Mechanism, Truncated Crash Recovery Windows, Full-Page Images (`full_page_writes`), and Torn-Page Protection  
**Status:** Complete & Verified  

---

## 1. Directory Structure & Attached Code Files

- **WAL Header**: [`include/pg/wal.h`](../include/pg/wal.h)
- **WAL Implementation (Checkpoints & FPI)**: [`src/wal.cpp`](../src/wal.cpp)
- **Engine REPL Command (`CHECKPOINT`)**: [`include/pg/engine.h`](../include/pg/engine.h) and [`src/engine.cpp`](../src/engine.cpp)
- **Unit Test Suite**: [`tests/test_checkpoint.cpp`](../tests/test_checkpoint.cpp)

---

## 2. The Problems Solved

### A. The Endless WAL Replay Bottleneck
Without checkpoints, REDO crash recovery must scan the WAL log from byte 0 across all historical transactions that ever ran. As the database runs for months or years, recovery time grows to hours.

**Checkpoints** truncate the recovery horizon:
1. All dirty pages currently resident in `shared_buffers` (RAM) are written to disk (`flush_all()`).
2. A `CHECKPOINT` record is appended to the WAL and flushed.
3. On restart, REDO recovery starts scanning **strictly from the last checkpoint LSN**, skipping ancient history!

```text
 ───[ancient WAL...]───► [ CHECKPOINT (LSN 4700) ] ───► [ 5 new transactions ] ──► (CRASH)
                              ▲
                              └─── REDO Recovery starts here! (Skips all prior logs)
```

---

### B. The Torn-Page Hazard & Full-Page Images (FPI)
Standard OS/disk sector writes are 512 bytes or 4KB. A PostgreSQL page is **8KB**. If a power loss occurs mid-write, the disk may end up with 4KB of old data and 4KB of new data (a **torn page**).

Normal WAL delta records only say: *"change price from $10 to $20 at offset 8120"*. If the page header or slotted layout is torn, delta replay will crash or corrupt data.

**Full-Page Image (FPI)** solves this:
- On the **first modification** of an 8KB page after each checkpoint, PostgreSQL writes the **entire 8KB pristine page** into the WAL.
- During recovery, the FPI record provides a complete 8KB baseline, overwriting and healing any torn page on disk before applying subsequent delta records.

---

## 3. Verification Results (`tests/test_checkpoint.cpp`)

```text
--- REPRODUCING POSTGRESQL WAL CHECKPOINTS & FULL-PAGE IMAGES (FPI) ---
[Step 1] Inserting 50 items and executing CHECKPOINT...
[CHECKPOINT] All dirty buffer pool frames flushed to disk. Checkpoint record logged at LSN: 4700.
[Step 2] Inserting 5 post-checkpoint items (5100..5500)...
[Step 3] Restarting database and running WAL recovery from checkpoint...
[REDO REDO] Successfully scanned WAL and replayed 5 committed log records into heap table.
 -> Verified: Recovery skipped all 50 pre-checkpoint records and only replayed 5 records!
 -> All 55 items verified after fast checkpoint-assisted recovery.

[Step 4] Testing Full-Page Image (FPI) Torn-Page Restoration...
 -> FPI record logged to WAL at LSN: 0
 -> Simulated torn page on disk (corrupted 4096..8191 bytes with 0xDE).
 -> WAL recovery replayed 1 records (including FPI baseline).
 -> Torn page fully healed from FPI! All 5 tuples restored byte-for-byte.

>>> ITEM 15 (WAL CHECKPOINTS & FULL-PAGE IMAGES) TESTS PASSED SUCCESSFULLY! <<<
```

---

## 5. Quiz Diagnostics & Graded Mechanics

### Q1 · Checkpoint Recovery Horizon Truncation (`CHECKPOINT-RECOVERY-HORIZON`)
- **Question**: What is the primary purpose of writing periodic `CHECKPOINT` records?
- **Answered**: Option 2 (To flush dirty buffer pool pages to disk and establish a known durable baseline so crash recovery only needs to replay WAL records written after the checkpoint LSN rather than scanning from byte 0) ✅
- **Mechanism**:
  Checkpoints establish a physical synchronization point where on-disk table pages match in-memory modifications. Replay before the checkpoint LSN is redundant, capping recovery duration to the volume of WAL generated since the checkpoint.

---

### Q2 · Torn-Page Healing via Full-Page Images (`TORN-PAGE-FPI-HEALING`)
- **Question**: Why does PostgreSQL write an 8KB FPI on the first modification after a checkpoint, but compact deltas thereafter?
- **Answered**: Option 1 (Writing 8KB on every update causes massive I/O bloat, but the first write provides an uncorrupted 8KB baseline to heal torn pages resulting from partial sector writes during a crash) ✅
- **Mechanism**:
  If a crash occurs mid-write on an 8KB block, disk sectors are inconsistent. Replaying delta offsets on a torn page causes fatal memory corruption. The FPI overwrites the broken block with a pristine whole-page baseline before deltas are applied.

---

### Q3 · Checkpoint Frequency Trade-off (`CHECKPOINT-THROUGHPUT-TRADE-OFF`)
- **Question**: What is the trade-off of running checkpoints very frequently vs infrequently?
- **Answered**: Option 1 (Frequent checkpoints provide faster recovery times but cause high I/O write spikes by flushing dirty pages and triggering FPIs; infrequent checkpoints maximize normal write throughput but result in longer recovery times during crash startup) ✅
- **Mechanism**:
  Frequent checkpoints incur repeated dirty buffer writeback and force new FPI records on subsequent writes. Tuning checkpoint intervals balances steady-state transaction latency against Mean Time to Recovery (MTTR).

