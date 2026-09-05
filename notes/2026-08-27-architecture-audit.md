# Architecture Audit: cpp-postgres vs. a real RDBMS

**Date**: 2026-08-27 (audit) / 2026-08-27 (remediation)
**Scope**: the implementation in `src/` and `include/pg/`, not the atlas prose.
**Method**: read every core subsystem, then wrote throwaway probes against `libpg_core.a` to confirm the severe findings empirically rather than by inspection alone.

The atlas (`notes/atlas/`) documents *format* fidelity accurately. This audit covers *behavioural* fidelity, and the picture was worse: several mechanisms were present as code but not wired into the running system, and the two guarantees the project most loudly claimed — write-ahead durability and the buffer pool as a single gateway — were not upheld.

Four findings below are marked **[PROVEN]**: a probe was compiled against the real library and its output is quoted.

---

## Remediation status

All of Tier 1 and all of Tier 3 are fixed, along with the two headline Tier 2
items. Regression coverage lives in `tests/test_recovery.cpp` and
`tests/test_concurrency.cpp`; `bash run_tests.sh` runs the whole suite (19 tests).

| # | Finding | Status | Where |
|---|---|---|---|
| 1.1 | WAL never fsynced | **Fixed** | `File::sync()` (`_commit` / `fdatasync`); the storage layer no longer uses `std::fstream` |
| 1.2 | Log written after the page | **Fixed** | `HeapFile` emits the record before mutating, while holding the pin |
| 1.3 | `pd_lsn` never set | **Fixed** | `Page::set_lsn` on every change; `BufferPoolManager::write_frame` calls `WALManager::flush_up_to` before any write |
| 1.4 | Recovery never ran at startup | **Fixed** | `ControlFile` + `Engine::Engine()` replays when the last shutdown was unclean |
| 1.5 | VACUUM bypassed the pool | **Fixed** | `Vacuum::run` works through `PinnedPage`; a relation has exactly one pool |
| 2.1 | One transaction engine-wide | **Fixed** | `Session`; `Engine::execute(sql, Session&)`; one session per connection |
| 2.2 | No lock manager | **Fixed (basic)** | `LockManager` serialises row writers and reports conflicts |
| 2.3 | Index is a `std::multimap` | **Fixed** | `DiskBTree` wired into `Engine`; `Index` interface; `remove_entry` in `Vacuum`; buffer pool caching; $O(1)$ startup |
| 2.4 | Copy-in/copy-out buffer pool | **Fixed** | `Page` is a view over the frame; `PinnedPage` holds the pin across read-modify-write |
| 2.5 | No executor | **Partly** | scans filter as they go instead of materialising the table; still no plan tree |
| 2.6 | No free space map | **Fixed** | Free Space Map (`<db_prefix>_fsm.db`) companion fork; 8KB `FsmPage` binary max-heap tree layout; $O(\log M)$ allocation; VACUUM Phase 3 recycling |
| 2.7 | TOAST writes no WAL | **Fixed** | `ToastChunkHeader` on-disk layout; `ToastManager::scan_existing_pages()` startup recovery; `WALRecordType::TOAST_INSERT`; ARIES REDO/UNDO replay |
| 2.8 | CLOG I/O per transaction | **Partly** | `begin_transaction` no longer writes CLOG; commit/abort still go straight to the pager |
| 3.1 | Clock sweep threw spuriously | **Fixed** | `usage_count` capped at `BUF_USAGECOUNT_MAX = 5`; sweep bounded by the cap |
| 3.2 | CLR/ABORT silently dropped | **Fixed** | positioned writes; no sticky stream state |
| 3.3 | Wrong VACUUM cutoff | **Fixed** | `TransactionManager::oldest_snapshot_xmin()` |
| 3.4 | Recycled line pointers | **Fixed** | three-phase VACUUM; `LP_DEAD` is not allocatable; `LP_REDIRECT` for HOT roots |
| 3.5 | REDO ignored slot ids | **Fixed** | `Page::insert_tuple_at` |
| 3.6 | XIDs reused after restart | **Fixed** | counter persisted in the control file |
| 3.7 | HOT precondition unchecked | **Fixed** | `Engine::update_item` verifies no indexed column changed |
| 3.8 | Minor (UB cast, unsafe default, non-atomic update) | **Fixed / Partly** | frame owns its `Page` view; unknown XID now reads as aborted; update still two page writes under one record |

One regression was introduced and caught during remediation: once VACUUM began
creating `LP_REDIRECT` roots, index scans had to learn to follow them.
`HeapFile::hot_search` (PostgreSQL's `heap_hot_search_buffer`) now does.

The original findings follow unchanged, as the record of what was wrong.

---

## Tier 1 — the claimed guarantees do not hold

### 1.1 Nothing is ever fsynced. WAL is not durable.

`WALManager::flush()` (`src/wal.cpp:148`) and `Pager::write_page()` (`src/pager.cpp:131`) both call `std::fstream::flush()`. That drains the C++ userspace buffer into the OS page cache and returns. It does **not** force data to durable media.

```
$ grep -rn "fsync\|FlushFileBuffers\|_commit(\|fdatasync" src include
(no matches)
```

`log_commit()` carries the comment `// Force WAL to disk on transaction commit (Write-Ahead Durability!)`. It does not. A power cut or OS crash after a reported COMMIT loses the transaction. PostgreSQL calls `pg_fsync`/`fdatasync` on the WAL segment before acknowledging commit; that single syscall is what the entire D in ACID rests on.

**Fix**: on Windows, `FlushFileBuffers(HANDLE)`; on POSIX, `fdatasync(fd)`. `std::fstream` does not expose the handle, so the WAL needs a raw `HANDLE`/`fd` rather than a stream.

### 1.2 The log is written *after* the page is modified — this is write-behind, not write-ahead

`Engine::insert_item` (`src/engine.cpp:116-121`):

```cpp
CTID ctid = heap_->insert({item_id, price}, tx_id);   // 1. mutate the page (may reach disk)
auto tuple_opt = heap_->get(ctid);
lsn = wal_->log_insert(tx_id, ctid.page, ctid.slot, *tuple_opt);   // 2. only now, log it
```

`update_item` has the same order (`heap_->hot_update(...)` then `wal_->log_update(...)`). Because `HeapFile::write_page_internal` falls through to `pager_->write_page()` when no buffer pool is attached — and that call flushes — the data page can reach the OS before any WAL record describing it exists. That is the exact inversion of the rule stated as global invariant #3 in the atlas.

PostgreSQL emits the WAL record inside `heap_insert()` while still holding the buffer's exclusive content lock, then calls `PageSetLSN()` before releasing it. Logging and mutation are one critical section.

### 1.3 `pd_lsn` is never set during normal operation, so the WAL rule is unenforceable

`page.header().pd_lsn` is assigned in exactly three places, all inside `WALManager::recover()` (`src/wal.cpp:263, 300, 324`). No DML path sets it. Pages therefore sit on disk with `pd_lsn = 0` forever.

Consequently `BufferPoolManager::victim_frame()` (`src/buffer_pool.cpp:135`) evicts a dirty page with a bare `pager_.write_page(...)` and no `XLogFlush(page.pd_lsn)` beforehand. `flushed_lsn_` is tracked but never read by anything. The invariant `page.pd_lsn <= wal.flushed_lsn` is documented, and there is no code that could enforce it.

### 1.4 Crash recovery never runs at startup

`Engine::Engine()` opens the files, sequentially scans the heap to rebuild the index, and returns. It does not call `recover()`. The only call site is the `RECOVER` REPL verb (`src/engine.cpp:430`).

A real RDBMS decides at startup whether the last shutdown was clean — PostgreSQL reads `pg_control` and runs redo unconditionally if the state is not `DB_SHUTDOWNED`. Here, a crashed database silently reopens in an unrecovered state and serves queries from it.

### 1.5 VACUUM bypasses the buffer pool, so it reads stale pages and its work is silently discarded **[PROVEN]**

`Vacuum::run()` (`src/vacuum.cpp:66, 73`) calls `heap.pager().read_page()` and `heap.pager().write_page()` directly. Every other heap access goes through `BufferPoolManager`. Two writers, two paths, no coordination.

Probe (`probe_divergence.cpp`) — insert, HOT-update, checkpoint, vacuum:

```
=== A. VACUUM reclaims the dead HOT root at (0,1)
[VACUUM] ... Reclaimed 1 dead tuples (24 bytes) across 1 pages.

=== B. Page 0 ON DISK, read with a private Pager (what VACUUM wrote)
  slot 1 = DEAD
  slot 2 = NORMAL

=== C. Same page THROUGH THE ENGINE, i.e. via the buffer pool
|     100 | $   20 |     2 |     0 | (0, 2) |
  ^ resolved by walking the HOT chain from (0,1), a slot that
    VACUUM already marked DEAD on disk. The cached frame is stale.

=== D. Now UPDATE again, then CHECKPOINT -- the stale frame is written back
[Tx 4] UPDATE: HOT-update successful! Placed at (0, 3) ...
[CHECKPOINT] All dirty buffer pool frames flushed to disk.

=== E. Page 0 ON DISK again -- did VACUUM's work survive?
  slot 1 = NORMAL
  slot 2 = NORMAL
  slot 3 = NORMAL
```

Slot 1 went `DEAD` → `NORMAL`. VACUUM's reclamation was **silently undone** by a buffer frame that predated it. RAM and disk had diverged, and the loser was whichever writer flushed last.

An earlier run showed the same root cause from the other direction: `VACUUM` immediately after a HOT update reported `Reclaimed 0 dead tuples`, because the xmax stamp was still sitting in a dirty frame. A `CHECKPOINT` followed by the identical `VACUUM` then reclaimed 1.

`CLogManager` (`src/clog.cpp`) and `ToastManager` also use their own `Pager` and never touch the buffer pool. The pool is constructed on `heap_->pager()` alone (`src/engine.cpp:31`), so the "single gateway" covers one of four relations.

---

## Tier 2 — the shape is not an RDBMS

### 2.1 The engine can hold exactly one transaction, so MVCC is never exercised

`Engine` has a single `std::optional<tx_id_t> current_tx_` and a single `current_snapshot_` (`include/pg/engine.h:77-78`). `PgWireServer` holds one `Engine&` behind one `std::mutex engine_mutex_` (`include/pg/pgwire.h:50,55`).

Two `psql` sessions therefore share one transaction. Client A's `BEGIN` puts client B inside A's transaction; B's `COMMIT` commits A's writes. There is no session state.

This hollows out the centrepiece. MVCC exists so that concurrent transactions can hold *different* snapshots; `active_txs_` can only ever contain one entry from the engine's own path, so no snapshot ever excludes a genuinely concurrent writer. `src/mvcc.cpp` is a faithful transcription of `HeapTupleSatisfiesMVCC`'s shape, and it is correct — it simply never encounters the situation it was written for.

### 2.2 No lock manager, no row locks, no deadlock detection

```
$ grep -rn "LockManager\|row_lock\|FOR UPDATE\|deadlock" src include
(no matches)
```

MVCC removes the need for *read* locks. It does not remove the need for write-write conflict handling: two transactions updating the same row must serialise, and PostgreSQL does that with a tuple lock plus `XactLockTableWait`. Here, the second writer would simply overwrite. There is also no `LWLock`/content-lock layer on buffers.

### 2.3 The index the engine actually uses is a `std::multimap` in RAM

`include/pg/btree.h:52` — `std::multimap<index_key_t, CTID> tree_;`

The entire `DiskBTree` subsystem (Item 14 in the atlas — ~12 KB of source, disk pages, node splits, sibling pointers) was originally not referenced by `Engine` at all. `Engine`'s member was `BTreeIndex index_`, the in-memory multimap, which rebuilt itself on startup by sequential scan of the heap.

**Resolution (Fixed)**: Wired `DiskBTree` directly into `Engine` via an abstract `Index` interface (`include/pg/index.h`). `DiskBTree` now implements:
1. `remove_entry()` for VACUUM Phase 2 index pruning before `LP_DEAD` line pointers are released.
2. `num_entries()` and `num_unique_keys()` by leaf-level traversal.
3. `dump()` diagnostic formatting.
4. Dedicated buffer pool caching (`bpm_owned_`) with dirty page writeback and `flush()` on checkpoint and shutdown.
5. $O(1)$ instant engine startup from `<db_prefix>_index.db` without scanning the heap.

### 2.4 The buffer pool is a copy-in/copy-out cache, not a buffer pool

`HeapFile::read_page_internal` (`src/heap.cpp:24-37`) pins a frame, `memcpy`s all 8192 bytes into a local vector, **unpins immediately**, and returns a `Page` constructed over the copy — which copies the 8 KB a second time, since `Page` owns `uint8_t data_[PAGE_SIZE]`. `write_page_internal` re-fetches and `memcpy`s back.

Two consequences:

- **No page is pinned across a read-modify-write.** Between the read and the write-back the frame can be evicted, refetched, or modified by another path; the write-back then clobbers whatever happened in between. This is the same lost-update class as 1.5, and it is structural.
- Three 8 KB copies per update. A buffer pool exists so callers can work *in place* on shared memory. PostgreSQL's `BufferGetPage()` returns a pointer into shared buffers and the backend mutates it directly under `LW_EXCLUSIVE`.

### 2.5 There is no executor; `seq_scan()` materialises the whole table

`HeapFile::seq_scan()` (`src/heap.cpp:212`) builds a `std::vector` of every tuple on every page, and the snapshot-filtering overload (`:236`) then calls it and filters the result. Peak memory is the size of the table regardless of how many rows match.

Real engines stream: a plan node pulls one tuple at a time, so a `SELECT ... LIMIT 1` on a billion-row table touches one page. There is no plan tree, no cost model, no `ANALYZE` statistics, no join algorithms — reasonable for a storage-engine project, but it means "SQL engine" overstates the layer.

### 2.6 No free space map — VACUUM reclaims space that INSERT can never reuse

`HeapFile::insert` (`src/heap.cpp:60`) always targets `pager_->num_pages() - 1`, the last page, and appends a new page when that one is full. Earlier pages are never revisited.

So when VACUUM frees space in page 0 of a 500-page relation, that space is unreachable forever. PostgreSQL consults the `_fsm` fork to find a page with room. Without it, VACUUM's central purpose — making space reusable — is defeated for all but the final page.

**Resolution (Fixed)**:
1. **On-Disk Binary Max-Heap Tree (`include/pg/fsm.h`)**: Defined `FsmPage` (8,192 bytes) containing an 8,191-byte complete binary tree ($2^{13} - 1$ nodes) of depth 12 with 4,096 leaves (one byte per heap page, tracking categorical free space in 32-byte quantization buckets from 0 to 255) and 4,095 internal nodes maintaining child maximums.
2. **Companion Fork (`<db_prefix>_fsm.db`)**: `HeapFile` automatically manages its FSM companion fork.
3. **$O(\log M)$ Search and Descent (`src/fsm.cpp`)**: `FreeSpaceMap::search_page()` tests the root category in $O(1)$ and descends to the leftmost qualifying leaf in $< 12$ binary comparisons, eliminating $O(N)$ linear page scans.
4. **Instant Reclamation in VACUUM (`src/vacuum.cpp`)**: Phase 3 compaction registers the reclaimed free space of every compacted page into `heap.fsm()`, allowing subsequent inserts to reuse holes on earlier pages immediately.
5. **Regression Coverage (`tests/test_fsm.cpp`)**: Unit tests verify category discretization, max-heap property, multi-page scaling across 4,096 pages, and VACUUM hole reuse.

### 2.7 TOAST writes no WAL at all

```
$ grep -c "wal" src/toast.cpp
0
```

`insert_item_with_doc` writes TOAST chunks, inserts the heap tuple, then stamps `HEAP_HASEXTERNAL` — three separate page writes — and logs a single INSERT record covering only the heap tuple. After recovery, a restored tuple can carry a TOAST pointer to chunks that were never logged and may not exist. In PostgreSQL a TOAST table is an ordinary relation and its chunk inserts are WAL-logged like any other heap insert.

**Resolution (Fixed)**:
1. **On-Disk Slotted Page Chunk Format**: Defined `ToastChunkHeader` (16 bytes: `toast_id`, `chunk_seq`, `data_len`) preceding every chunk payload on auxiliary 8KB TOAST slotted pages.
2. **Startup Index Reconstruction**: Added `ToastManager::scan_existing_pages()`, reading all allocated TOAST pages and slotted line pointers at startup, reconstructing `chunk_index_` and advancing `next_toast_id_ = max(id) + 1` so toasted values survive restarts.
3. **WAL Logging**: Added `WALRecordType::TOAST_INSERT` (type 8). In `ToastManager::flush_chunk_to_page`, each chunk write emits a `TOAST_INSERT` record before updating disk, setting `pd_lsn` on the TOAST page.
4. **ARIES Crash Recovery**: `WALManager::recover` replays committed `TOAST_INSERT` records during the REDO pass via `ToastManager::replay_insert()`, and compensates uncommitted loser chunks in the UNDO pass.
5. **Regression Testing**: Added `Step 5` in `tests/test_toast_integration.cpp` verifying 20KB document crash recovery across simulated crashes.

### 2.8 CLOG does 16 KB of I/O per status change and bypasses the pool

`CLogManager::set_status` (`src/clog.cpp:38-45`) reads a full 8 KB page and writes a full 8 KB page to flip two bits, straight through `Pager`. It is called on every `begin_transaction`, `commit`, and `abort` — three round trips per transaction.

PostgreSQL keeps `pg_xact` in an SLRU cache in shared memory and writes pages at checkpoint. Also: PostgreSQL never writes `IN_PROGRESS`; `00` is the zero state of a freshly allocated page, so `begin_transaction` should not touch CLOG at all.

Note that `TransactionManager` keeps a `status_map_` of every XID in RAM (`include/pg/tx.h:70`) and consults it *before* CLOG, so at runtime CLOG is nearly vestigial — and that map grows without bound.

---

## Tier 3 — concrete bugs

### 3.1 Clock sweep throws "all frames are pinned" when nothing is pinned **[PROVEN]**

`usage_count` is incremented on every hit (`src/buffer_pool.cpp:26`) and never capped. `victim_frame()` gives up after `scan_limit = pool_size * 2` iterations (`:119`). Once every frame's `usage_count` reaches 2, `2 * pool_size` decrements cannot drive any frame to zero, and the sweep throws.

Probe (`probe_bpm.cpp`), pool of 4, each page touched three times, nothing pinned:

```
pool_size            = 4
scan_limit           = 8 (pool_size * 2)
pinned frames        = 0 (every fetch was unpinned)
usage_count / frame  = 3
decrements needed    = 12 > scan_limit

Now fetching a non-resident page, which forces eviction...
RESULT: THREW -> "BufferPoolManager: All buffer frames are pinned! Cannot evict any page."
        ...but no frame was actually pinned.
```

Two fetches of the same page are enough to arm this. This is exactly why PostgreSQL clamps at `BUF_USAGECOUNT_MAX = 5`: a bounded counter guarantees a bounded sweep.

**Fix**: clamp on increment (`if (uc < 5) uc++`), and let the sweep run until it finds a victim rather than capping iterations.

### 3.2 Recovery's CLR and ABORT records are silently dropped **[PROVEN]**

`recover()` reads the WAL to EOF, which sets `eofbit`. The UNDO pass then calls `append_record()` (`src/wal.cpp:354, 413, 422`) without `stream_.clear()`. `basic_ostream::seekp` and `write` construct a sentry that checks `good()`, and `good()` is false while `eofbit` is set — so both the seek and the write become no-ops, with no error surfaced.

Probe (`probe_clr.cpp`) — one committed transaction, one abandoned in-progress transaction, then `RECOVER`:

```
WAL size before recovery : 153 bytes
[REDO RECOVERY] Successfully scanned WAL and replayed 2 committed log records into heap table.
WAL size after recovery  : 153 bytes
Delta                    : 0 bytes
```

Zero bytes appended. No CLR, no ABORT record. The atlas documents this precise hazard as Item 1, invariant 3 ("subsequent seek operations silently fail unless `stream.clear()` is invoked") — and then the WAL code walks into it.

**Fix**: `stream_.clear()` at the top of `append_record`, and check `stream_.good()` after every write instead of assuming success.

### 3.3 VACUUM's cutoff uses the oldest active XID, not the oldest snapshot xmin

`Vacuum::run` uses `tm.oldest_active_xmin()` (`src/vacuum.cpp:59`), which returns `min(active_txs_)` — the lowest running *transaction id* (`src/tx.cpp:62-71`). That is not the same as the lowest *snapshot horizon*.

Counterexample: Tx 1 begins. Tx 2 begins and snapshots with `active = {1}`. Tx 1 deletes row R and commits. Now `oldest_active_xmin()` returns 2, so VACUUM sees `xmax=1 < 2` and reclaims R — but R is still visible to Tx 2's snapshot, which lists 1 as active. A row vanishes underneath a running transaction.

PostgreSQL computes `OldestXmin` from every backend's published snapshot `xmin` (`PGPROC.xmin`), not from backend XIDs. Currently unreachable only because of 2.1 (one transaction at a time).

### 3.4 Recycled line pointers plus an unvacuumed index can mis-resolve a key

`Page::insert_tuple` (`src/page.cpp:73-87`) reuses any line pointer flagged `DEAD` or `UNUSED` for a brand-new tuple. VACUUM marks slots `DEAD` (`src/vacuum.cpp:39`) and **never touches the index** — `BTreeIndex::remove_entry` exists but has no caller.

So a stale index entry pointing at a vacuumed slot can later resolve to an unrelated row that recycled it. This is exactly the corruption PostgreSQL's three-phase VACUUM exists to prevent: `LP_DEAD` is *not* allocatable, and the `LP_DEAD → LP_UNUSED` transition happens only after every index has been cleaned of that TID.

VACUUM also never converts a HOT chain root to `LP_REDIRECT`, so reclaiming a root severs the chain to its heap-only successors.

### 3.5 REDO does not restore tuples to their logged slots

The INSERT redo path calls `page.insert_tuple(&tuple, ...)` (`src/wal.cpp:261`), which picks its own slot — the first reusable one, else append. It ignores `rec.header.slot_id`. The UPDATE path does the same (`:297, 322`).

Physical redo must reproduce the exact physical state. If replay places a tuple at a different offset than the original, every index entry referencing the original CTID now points to the wrong row. PostgreSQL's `heap_xlog_insert` uses `PageAddItem` with the exact `OffsetNumber` from the record and errors out if it cannot honour it.

### 3.6 XIDs are reused after restart

`Engine::Engine()` sets `next_tx_id` to one past the highest `xmin`/`xmax` found in the heap (`src/engine.cpp:40-46`). If the heap is empty — or every row was vacuumed — `max_xid` is 0 and the counter restarts at 1, reissuing XIDs that already have committed/aborted entries in CLOG. Visibility then resolves against another transaction's status.

The counter should be persisted (PostgreSQL keeps `nextXid` in `pg_control`, updated at checkpoint), not re-derived from surviving data.

### 3.7 The HOT precondition is never checked

`Engine::update_item` calls `heap_->hot_update(...)` unconditionally and accepts it whenever the page has room (`src/engine.cpp:221`). Nothing tests whether an indexed column changed — the first of the two HOT preconditions.

This is currently harmless only because the schema is `items(item_id, price)`, the sole index is on `item_id`, and the sole UPDATE changes `price`. Add a second index, or allow `item_id` to be updated, and HOT will silently skip the required index insert.

### 3.8 Minor

- `reinterpret_cast<Page*>(frames_[fid].data)` (`src/buffer_pool.cpp:27, 57`) — no `Page` object was ever constructed at that address. It works because `Page` is standard-layout with one array member, but it is formally UB; `std::launder` or an explicit placement-new would make it well-defined.
- `TransactionManager::get_status` falls back to "assume COMMITTED if `tx_id < next_tx_id_`" when no CLOG is attached (`src/tx.cpp:90-92`). Silently treating unknown transactions as committed is the unsafe default; unknown should mean aborted.
- `HeapFile::update` inserts the new version and stamps the old one as two independent page writes (`src/heap.cpp:102-111`), non-atomic and covered by a single WAL record written afterwards. A crash between them leaves two live versions of the row.

---

## What is genuinely right

Worth stating plainly, because a lot of it is:

- **`src/mvcc.cpp`** is a correct transcription of `HeapTupleSatisfiesMVCC`'s structure — xmin phase then xmax phase, own-transaction special cases, aborted/in-progress/committed handling, snapshot-horizon checks in the right order.
- **`index_lookup`** (`src/btree.cpp:69-100`) correctly walks the HOT chain via `t_ctid`, guards against cross-page hops, and bounds the traversal. This is the subtle part of HOT and it is right.
- **Slot stability under defragmentation** (`src/page.cpp:113-149`) — `defragment()` slides payloads but never renumbers line pointers, which is the invariant that makes CTIDs stable for indexes. Correct, and load-bearing.
- **The slotted page layout itself**, the 2-bit CLOG packing and its bit arithmetic, the CTID encoding, and the pgwire v3.0 framing are all faithful.
- **Commit ordering** — `log_commit()` before `tm_.commit()` (`src/engine.cpp:81-82`) puts the WAL record ahead of the CLOG update, which is the right order even though neither is fsynced.
- **`recover()` repeats history for winners and losers alike** before undoing, which is the correct ARIES sequencing.

---

## Suggested order of work

If the goal is "behaves like an RDBMS" rather than "illustrates RDBMS concepts":

| # | Change | Unlocks |
|---|---|---|
| 1 | Route `Vacuum`, `CLogManager`, `ToastManager` through `BufferPoolManager`; delete every `pager()` call outside it | Fixes 1.5 — the active data-loss bug |
| 2 | Clamp `usage_count` at 5; loop the sweep until a victim is found | Fixes 3.1 — a live crash |
| 3 | `stream_.clear()` in `append_record`; check `good()` after writes | Fixes 3.2 |
| 4 | Real `fsync`/`FlushFileBuffers` on the WAL at commit | Fixes 1.1 — makes durability real |
| 5 | Move WAL logging *into* `HeapFile`, before the mutation; set `pd_lsn`; check `flushed_lsn` before eviction | Fixes 1.2, 1.3 |
| 6 | Call `recover()` from `Engine::Engine()` behind a clean-shutdown flag | Fixes 1.4 |
| 7 | Give `Page` a non-owning view over a pinned frame; keep the pin across read-modify-write | Fixes 2.4, and is the prerequisite for concurrency |
| 8 | Per-session transaction state (`Session` object), then a lock manager | Fixes 2.1, 2.2 — makes MVCC mean something |
| 9 | Three-phase VACUUM with an index pass; `LP_DEAD` not allocatable; `LP_REDIRECT` for HOT roots | Fixes 3.4 |
| 10 | Wire `DiskBTree` into `Engine` in place of the multimap | Fixes 2.3 |

Items 1–3 are small and fix reproducible failures. Item 7 is the one that changes the shape of the codebase, and items 8–10 depend on it.
