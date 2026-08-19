# Item 20: Native Desktop GUI & Unified Server Daemon

**Confidence**: `verified`  
**Citations**: [src/gui/main_gui.cpp:1-480](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/src/gui/main_gui.cpp), [src/server_main.cpp:1-75](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/src/server_main.cpp), [CMakeLists.txt:100-150](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/CMakeLists.txt)

---

## 1. Unified Applications Overview

Item 20 delivers two standalone executable artifacts:
1. **`pg_gui.exe` (3.89 MB)**: A self-contained Windows desktop GUI application built with **Dear ImGui + DirectX 11 + Win32**, providing visual memory inspectors for 8KB slotted pages, clock-sweep buffer frames, on-disk B-Tree graphs, and an interactive SQL scratchpad.
2. **`pg_server.exe`**: A multi-protocol daemon running both **Approach A (HTTP REST :8080)** and **Approach C (pgwire TCP :5432)** concurrently on background threads, wired to a shared `pg::Engine`.

```mermaid
flowchart TD
    subgraph StandaloneApps["Executable Applications"]
        GUI["pg_gui.exe (Desktop Explorer)\n• DirectX 11 + Dear ImGui\n• Live Slotted Page Memory Bar\n• Real-Time Clock-Sweep Visualizer\n• B-Tree Index Key Mapper"]
        SRV["pg_server.exe (Unified Server Daemon)\n• Background Thread 1: HTTP :8080\n• Background Thread 2: pgwire :5432\n• Signal Handler & Console CLI"]
    end

    subgraph CoreEngine["Shared Core Library (libpg_core.a)"]
        ENG["pg::Engine (C++20)\nShared Buffers · Slotted Pages · ARIES WAL · CLOG · TOAST"]
    end

    GUI --> ENG
    SRV --> ENG
```

---

## 2. Desktop GUI Feature Breakdown

```text
┌────────────────────────────────────────────────────────────────────────────────────────┐
│  🐘 CPP-POSTGRES STORAGE ENGINE DESKTOP EXPLORER                         [ _ □ ✕ ]   │
├──────────────────────────┬─────────────────────────────────────────────────────────────┤
│ 📝 SQL QUERY WORKSPACE   │ 🔬 PHYSICAL STORAGE ENGINE VISUALIZERS                      │
│ ┌──────────────────────┐ │ [📄 8KB Slotted Page] [🧠 Shared Buffers] [🌲 Disk B-Tree]  │
│ │ SELECT * FROM items; │ │ ─────────────────────────────────────────────────────────── │
│ └──────────────────────┘ │ Page 0 Header (18B) | pd_lower: 30B | pd_upper: 8120B      │
│ [▶ Run F5] [BEGIN]      │ [■■■ Line Pointers] [        Free Space        ] [■■ Tuples] │
│ [COMMIT]   [ROLLBACK]   │ ┌────────┬────────┬────────┬───────────────────────────────┐ │
│ [STATUS]   [VACUUM]     │ │ Slot # │ Offset │ Length │ Live Tuple Details            │ │
│                         │ ├────────┼────────┼────────┼───────────────────────────────┤ │
│ 📋 LIVE TABLE VIEW      │ │ Slot 1 │ 8168   │ 24 B   │ [LIVE] id=100, price=$10      │ │
│ ┌─────┬───────┬────┬───┐│ │ Slot 2 │ 8144   │ 24 B   │ [HOT REDIRECT] -> (0, 3)      │ │
│ │ id  │ price │xmin│CTI││ │ Slot 3 │ 8120   │ 24 B   │ [HEAP-ONLY TUPLE] id=100, $20 │ │
│ ├─────┼───────┼────┼───┤│ └────────┴────────┴────────┴───────────────────────────────┘ │
│ │ 100 │ $20   │ 4  │0,3││                                                             │
│ │ 200 │ $5    │ 2  │0,2││ 🧠 Shared Buffers Tab: Live Clock-Sweep & Pin Tracking       │
│ └─────┴───────┴────┴───┘│ 🌲 Disk B-Tree Tab: Key -> Candidate CTID Mapping           │
│                         │ 🚦 CLOG Tab: 2-bit Transaction Status Bitmaps               │
│ 📊 OUTPUT LOG CONSOLE   │ 📜 WAL Tab: ARIES Crash Recovery & Checkpoint Stream         │
│                         │ 🍞 TOAST Tab: 2KB Chunk Auxiliary Table Inspector           │
└──────────────────────────┴─────────────────────────────────────────────────────────────┘
```

---

## 3. Sequence Diagram: Dual Server Daemon Operation

```mermaid
sequenceDiagram
    autonumber
    participant Main as Server Main (src/server_main.cpp)
    participant Engine as Shared pg::Engine
    participant HTTP as HttpServer (:8080)
    participant PGW as PgWireServer (:5432)
    participant React as React Browser
    participant PSQL as psql CLI

    Main->>Engine: Initialize Database ("pg_server_data")
    Main->>HTTP: start_async() (Spawns HTTP Worker Thread)
    Main->>PGW: start_async() (Spawns pgwire Worker Thread)
    
    par Concurrent Client Queries
        React->>HTTP: POST /api/sql { "sql": "INSERT INTO items..." }
        HTTP->>Engine: lock(mutex) -> execute()
        Engine-->>HTTP: JSON output
        HTTP-->>React: HTTP 200 OK
    and
        PSQL->>PGW: Simple Query ('Q', "SELECT * FROM items;")
        PGW->>Engine: lock(mutex) -> seq_scan()
        Engine-->>PGW: Tuples
        PGW-->>PSQL: RowDescription ('T') + DataRow ('D')
    end

    Note over Main: User types 'quit' or sends SIGINT
    Main->>HTTP: stop() (Closes socket)
    Main->>PGW: stop() (Closes socket)
    Main-->>Main: Database cleanly persisted to disk!
```
