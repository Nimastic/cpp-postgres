# Item 15: WAL Checkpoints & Full-Page Images (FPI)

**Confidence**: `verified`  
**Citations**: [include/pg/wal.h:20-60](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/include/pg/wal.h), [src/wal.cpp:115-180](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/src/wal.cpp), [tests/test_checkpoint.cpp:1-130](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/tests/test_checkpoint.cpp)

---

## 1. Checkpoint & Torn-Page Protection

**Checkpoints** truncate the recovery horizon so that crash recovery does not need to scan WAL logs from the beginning of time.

**Full-Page Images (FPI)** prevent catastrophic corruption caused by **torn pages** (when the OS crashes in the middle of writing an 8KB page, leaving a half-written 4KB sector on disk).

```mermaid
flowchart TD
    subgraph CheckpointTimeline["Checkpoint Recovery Horizon"]
        CP["CHECKPOINT (LSN: 5000)\n1. All dirty frames flushed to disk\n2. Checkpoint record logged\n3. Truncate WAL scan horizon"]
        FIRST_WRITE["First Write to Page 0 After Checkpoint\nLogs entire 8192-byte FPI to WAL!"]
        CRASH["OS Crash / Power Loss\n(Torn Page on Disk)"]
        HEAL["Crash Recovery Replay:\nFPI overwrites torn page with perfect 8KB baseline!"]
    end

    CP --> FIRST_WRITE --> CRASH --> HEAL
```

---

## 2. Invariants & Mechanics

1. **Dirty Buffer Coalescing**: `log_checkpoint()` invokes `BufferPoolManager::flush_all()`, writing all modified RAM frames to disk before appending the `CHECKPOINT` record (`[src/wal.cpp:135]`).
2. **First-Write FPI Invariant**: The first transaction to modify any page following a checkpoint must write a full 8,192-byte snapshot of the page into WAL (`RecordType::FPI = 6`). Subsequent writes to that page only log compact delta records until the next checkpoint (`[src/wal.cpp:150]`).
3. **Torn-Page Healing**: During recovery, replaying an FPI record unconditionally restores the physical 8KB page byte-for-byte, healing torn sectors before delta REDO logs are applied.

---

## 3. Sequence Diagram: Checkpoint & Torn Page Healing

```mermaid
sequenceDiagram
    autonumber
    participant Admin as Checkpoint Daemon
    participant WAL as WALManager
    participant BPM as BufferPoolManager
    participant Disk as Physical Disk (*.db)

    Note over Admin,Disk: Checkpoint Execution
    Admin->>BPM: flush_all() (Write all dirty RAM frames to disk)
    Admin->>WAL: log_checkpoint() -> Records Checkpoint LSN (e.g. 5000)
    
    Note over Admin,Disk: First Post-Checkpoint Mutation
    Admin->>WAL: log_fpi(page_id=0, raw_8192_bytes)
    Admin->>WAL: log_insert(tx=10, ctid=(0, 1), data)
    
    Note over Admin,Disk: CRASH! Disk sector torn during write!
    
    Note over Admin,Disk: ARIES Crash Recovery
    Admin->>WAL: recover()
    WAL->>WAL: Replay starts at Checkpoint LSN 5000 (Skips old history!)
    WAL->>Disk: Replay FPI -> Writes exact 8192 bytes (Torn page healed!)
    WAL->>Disk: Replay Insert Delta -> Slot 1 restored!
```
