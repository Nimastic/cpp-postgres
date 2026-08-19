# PostgreSQL Storage Engine Architecture & Orientation

**Confidence**: `verified`  
**Citations**: [include/pg/engine.h:1-85](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/include/pg/engine.h), [src/engine.cpp:1-250](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/src/engine.cpp), [CMakeLists.txt:1-150](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/CMakeLists.txt)

---

## 1. Executive Overview

`cpp-postgres` is a production-accurate, zero-external-dependency relational storage engine implemented in C++20. It faithfully reproduces the exact internal disk formats, concurrency mechanisms, memory structures, and crash recovery algorithms utilized by PostgreSQL.

The architecture is partitioned into strict, decoupled layers:

```mermaid
flowchart TD
    subgraph Clients["Access Layer (Items 11, 18, 19, 20)"]
        CLI["CLI REPL\n(pg_cli.exe)"]
        GUI["Desktop GUI\n(pg_gui.exe)"]
        HTTP["HTTP REST :8080\n(Approach A)"]
        PGW["pgwire TCP :5432\n(Approach C)"]
    end

    subgraph EngineCore["Engine Coordinator (pg::Engine)"]
        TM["Transaction Manager\n(MVCC & Snapshots)"]
        EXEC["SQL Executor & Parser"]
    end

    subgraph Memory["RAM Subsystems (Item 8, 12)"]
        BPM["Shared Buffers (BufferPoolManager)\n16 Frames · Clock-Sweep Eviction"]
    end

    subgraph Storage["Disk Subsystems (Items 1-7, 9, 10, 13-17)"]
        HEAP["Heap Relation (.db)\n8KB Slotted Pages"]
        BTREE["Disk B-Tree (.db)\nRoot/Internal/Leaf"]
        WAL["Write-Ahead Log (.log)\nCRC32 · FPI · CLRs"]
        CLOG["Commit Log (.db)\n2-bit Bitmap Pages"]
        TOAST["TOAST Relation (.db)\n2KB Data Chunks"]
    end

    Clients --> EngineCore
    EngineCore --> Memory
    Memory <--> HEAP
    Memory <--> BTREE
    Memory <--> CLOG
    Memory <--> TOAST
    EngineCore --> WAL
    WAL -.->|REDO / UNDO Recovery| HEAP
```

---

## 2. Global Subsystem Invariants

The entire storage engine enforces seven architectural invariants:

1. **8KB Page Uniformity**: All disk structures (Heap, B-Tree, CLOG, TOAST) operate on fixed 8,192-byte page boundaries aligned to the OS block layer (`[include/pg/page.h:12]`).
2. **Buffer Pool Single Gateway**: Application subsystems never execute direct file I/O; all page accesses pin frames inside `BufferPoolManager` (`[src/heap.cpp:45]`).
3. **Write-Ahead Logging (WAL)**: A dirty page in RAM is never written to disk until its corresponding WAL log record has been flushed (`page.pd_lsn <= wal.flushed_lsn`) (`[src/wal.cpp:110]`).
4. **Append-Only MVCC**: Updates never mutate live data in place; they stamp `xmax` on the old tuple version and append a new version with `xmin` (`[src/heap.cpp:88]`).
5. **Secondary Index Decoupling**: B-Tree secondary indexes map `Key -> CTID` physical addresses without embedding tuple columns or MVCC headers (`[src/btree.cpp:15]`).
6. **Zero Index-Bloat HOT Updates**: Unindexed column updates residing on the same 8KB page construct line-pointer chains with zero index writes (`[src/heap.cpp:142]`).
7. **2-Bit CLOG Density**: Transaction commit states are packed into 2-bit bitmaps on disk, scaling to 32,768 transactions per 8KB page (`[src/clog.cpp:30]`).

---

## 3. Subsystem Dependency Matrix

```mermaid
classDiagram
    class Engine {
        +execute(sql)
        +begin_transaction()
        +commit_transaction()
        +rollback_transaction()
        +vacuum()
        +checkpoint()
        +recover()
    }
    class HeapFile {
        +insert(record, xmin)
        +update(old_ctid, record, tx_id)
        +hot_update(old_ctid, record, tx_id)
        +seq_scan()
        +get(ctid)
    }
    class BufferPoolManager {
        +fetch_page(page_id)
        +unpin_page(page_id, is_dirty)
        +flush_page(page_id)
        +flush_all()
    }
    class WALManager {
        +log_insert(tx_id, ctid, record)
        +log_update(tx_id, old_ctid, new_ctid, record)
        +log_checkpoint()
        +log_fpi(page_id, page_data)
        +recover_3phase()
    }
    class DiskBTree {
        +insert_entry(key, ctid)
        +find_entries(key)
        +range_scan(min, max)
    }
    class CLogManager {
        +set_status(tx_id, status)
        +get_status(tx_id)
    }
    class ToastManager {
        +store_toast_payload(toast_id, data)
        +fetch_toast_payload(toast_id, raw_size)
    }

    Engine *-- HeapFile
    Engine *-- BufferPoolManager
    Engine *-- WALManager
    Engine *-- DiskBTree
    Engine *-- CLogManager
    Engine *-- ToastManager
    HeapFile --> BufferPoolManager
    DiskBTree --> BufferPoolManager
    WALManager --> BufferPoolManager
```
