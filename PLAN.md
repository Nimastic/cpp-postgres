# Plan

I am building a small Postgres-like storage engine in C++ after watching Hussein Nasser’s video:

https://www.youtube.com/watch?v=q9jixKv4h2I

Not the whole database. Just the parts from that video, plus a few things you bump into once you actually try to make it survive a crash.

One item at a time. When an item works, stop. Do not start the B-tree, the WAL, or a SQL parser early — they do not mean anything until the heap can hand you a CTID and an update can leave two versions of the same row sitting on a page.

Where I am: Item 6 complete (B-Tree Index key->CTID mapping, multi-version index entries per key, index_lookup combining B-Tree candidates with MVCC snapshot evaluation, range scan, and test suite). Next is Item 7 (Heap-Only Tuples / HOT).

A few constants I am sticking to:

- Pages are 8192 bytes.
- One table for a long time: `items`, with `item_id` and `price`. Price is just an int.
- Line pointers start at 1, like real Postgres. First row on page 0 is CTID `(0,1)`.
- C++20, CMake, tests that you can actually run. No SQL until the end.

Rough folder layout, filled in as I go:

```
include/pg/
src/
tests/
CMakeLists.txt
```

---

## What the video is saying

A table is a file. That file is page 0, then page 1, then page 2, each 8KB. Postgres does not read a row off disk. It pulls the whole page.

Inside a page, line pointers grow from the front and the actual row data grows from the back. The gap in the middle is free space.

A tuple is not “the row.” It is one version of the row. You find it with a CTID: which page, which line pointer.

An index does not store the row. It stores the CTID. Look up item 100, get `(0,1)`, maybe also `(0,3)` after an update, then go to the heap and figure out which one you are allowed to see.

Updates never happen in place. Old tuple keeps `$10` and gets an xmax. New tuple has `$20` and a new xmin. Transaction 10 sees twenty bucks. Some old transaction that started earlier still sees ten. That is MVCC.

Those old versions stick around until nobody needs them. That is vacuum. A transaction that sits open for ages pins the old snapshot, and that is how you get bloat.

---

## Item 1 — the pager

The whole game starts here. Open a file, treat it as an array of 8KB pages, seek to `page_id * 8192`, read or write 8KB.

Need: create/open the file, `read`, `write`, and `allocate` (append a new empty page).

It works when I can write junk into page 0 and page 1, close everything, open it again, and get the same bytes back. File should be exactly 16KB.

Do not put headers or rows on the page yet. Just bytes.

---

## Item 2 — what a page actually looks like

Now the page has a shape. Header with `lower` / `upper` / how many items (the Postgres `pd_lower` / `pd_upper` idea). Line pointers at the start: offset + length. Tuple bytes packed in from the end.

Insert a blob, get back slot 1. Insert another, slot 2. Print the page so I can see pointers at the low end, data at the high end, free space shrinking in the middle.

Still no real columns. One page in memory is enough.

---

## Item 3 — tuples and CTIDs

This is the `items` table from the video. Insert item 100 at $10, that should land at `(0,1)`. Insert 200 at $5, that is `(0,2)`. Scan the heap, get both back.

Leave room on the tuple for xmin and xmax, but leave them zero for now. If a page fills up, allocate the next one.

No updates yet. No index. Just insert and scan.

---

## Item 4 — MVCC

This is the part the video is actually about.

Never overwrite a row. Update writes a new tuple, stamps xmax on the old one, xmin on the new one.

I need a transaction id counter, begin/commit, and a snapshot taken at begin (like the long-running transaction in the video). Visibility is: can this snapshot see this xmin, and has xmax ended it yet or not.

The test is the video. Transaction 1 inserts `(100, 10)`. Later someone updates it to $20. A transaction that started in between and never finished should still see $10. A new one should see $20. Both physical tuples are still on the page.

Seq scan is fine. No index, no WAL. One connection.

---

## Item 5 — vacuum

The $10 tuple is not garbage just because we updated it. It is garbage when no running transaction still has a snapshot that needs it.

Vacuum walks pages, throws away tuples that are actually dead, and leaves the free space on the page. It does not shrink the file.

Test: vacuum while that old transaction is still open — the $10 row stays. Commit it, vacuum again — a new insert should reuse the hole instead of growing the file.

No VACUUM FULL. No autovacuum. No index cleanup (there is no index yet).

---

## Item 6 — the index

In the video the index is “a B-tree, but just think of it as key → CTID.”

Key is `item_id`. Value is the CTID. After the $10 → $20 update, item 100 has two entries. You still go to the heap to read the price and to decide which version you see.

First make that path real with an in-memory map. If that is solid and it is not a huge jump, put the leaves on disk and do splits. If it is getting messy, stop and do the on-disk tree as its own sitting.

You know it works when SELECT for item 100 through the index matches the snapshot tests from item 4, and a big heap does not need a full scan to find the row.

Every update still adds an index entry. That is supposed to be ugly. HOT is next.

---

## Item 7 — HOT

If I only change price, not item_id, and there is still room on the same page, Postgres can leave a forwarding pointer on the old tuple and skip touching the index. Heap-only tuples.

Update $10 to $20, index still has one entry for 100, fetch follows the chain, snapshots still work.

Do not bother pruning chains yet. If someone changes item_id, that is a normal update.

---

## Item 8 — shared buffers

The video mentions this in passing: you do not seek for every row. You pull the 8KB page into memory (`shared_buffers`) and look at it there.

A small pool, say 32 pages. Pin, unpin, dirty bit, kick out the least recently used (or clock). Heap and index reads go through the pool.

Fill more pages than the pool has. Some reads should hit, some miss. Dirty pages get written when they are evicted.

Without a WAL, a crash can lose work that was only in memory. That is fine for now — just do not pretend otherwise.

---

## Item 9 — WAL

Not really in the video. It shows up the moment you have a buffer pool.

Write the change to a log first, fsync that, then you can call the transaction committed. On open, replay the log.

Test: insert, commit, die before the heap pages are flushed, open again, the row is there. An insert that never committed should not come back.

No replication. No point-in-time recovery. Keep the log records boring.

---

## Item 10 — TOAST

A tuple cannot sit across two pages. If I ever store something huge, it has to get compressed or chopped into a side table, and the main page just keeps a pointer.

I have been using tiny rows on purpose. Add an optional payload, spill it if it would blow past ~2KB, glue it back together on read.

Insert 20KB of text, main page still looks like a normal page, read returns the whole string. Short rows stay inline.

---

## Item 11 — a tiny SQL prompt

Last on purpose. Until now C++ tests drive everything.

Enough SQL to replay the video by typing: begin, commit, insert, update, select, vacuum, dump page. `WHERE item_id = 100` uses the index. Everything else can seq scan. Still just the `items` table.

If I can type the lecture scenario and get $10 vs $20 for the two snapshots, that is the project.

No joins. No query planner. No network protocol. No threads fighting over the same page. Maybe later, not this.

---

## I am not building

A real Postgres. Joins. A planner. The wire protocol. Replication. Freeze / xid wraparound (worth knowing about when I do vacuum; I do not need to implement it unless a test actually wraps). VACUUM FULL. Autovacuum.
