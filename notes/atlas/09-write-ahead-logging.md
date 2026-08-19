# Item 9: Write-Ahead Logging (WAL) & REDO Durability

**Confidence**: `verified`  
**Citations**: [include/pg/wal.h:1-95](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/include/pg/wal.h), [src/wal.cpp:1-180](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/src/wal.cpp), [tests/test_wal.cpp:1-135](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/tests/test_wal.cpp)

---

## 1. Write-Ahead Logging (WAL) Architecture

To guarantee **ACID Durability** and high write throughput, PostgreSQL converts random 8KB page writes into sequential, append-only log records in the Write-Ahead Log (`*.log`).

```mermaid
flowchart LR
    subgraph TransactionWorker["Transaction Commit Flow"]
        DML["1. DML Execution\n(Mutates RAM Buffer Frame)"]
        LOG["2. Append WAL Record\n[LSN, Type, XID, Data, CRC32]"]
        FLUSH["3. Flush WAL to Disk (fsync)"]
        COMMIT["4. Commit Complete to Client"]
        DIRTY["5. Dirty Page Flush\n(Delayed / Checkpoint)"]
    end

    DML --> LOG --> FLUSH --> COMMIT
    FLUSH -.->|Must precede| DIRTY
```

---

## 2. Invariants & Record Binary Layout

1. **The WAL Invariant**:
   $$\text{page.pd\_lsn} \le \text{wal.flushed\_lsn}$$
   A dirty page in RAM is never written to disk before all WAL records up to the page's `pd_lsn` have been flushed to disk (`[src/wal.cpp:110]`).
2. **Log Record Structure (Header + Payload + CRC32)**:
   - `lsn` (8 Bytes): Monotonically increasing Log Sequence Number.
   - `prev_lsn` (8 Bytes): Backward pointer for UNDO / transaction rollback chains.
   - `tx_id` (4 Bytes): Transaction ID.
   - `type` (1 Byte): `1`=INSERT, `2`=UPDATE, `3`=COMMIT, `4`=ABORT, `5`=CHECKPOINT, `6`=FPI, `7`=CLR.
   - `page_id` (4 Bytes), `slot_id` (2 Bytes).
   - `payload_len` (2 Bytes) + `payload` data.
   - `crc32` (4 Bytes): IEEE 802.3 polynomial checksum verifying data integrity against partial writes and disk corruptions.

---

## 3. Sequence Diagram: REDO Crash Recovery Pass

```mermaid
sequenceDiagram
    autonumber
    participant Engine as Database Engine (Startup)
    participant WAL as WALManager (src/wal.cpp)
    participant Heap as Heap Storage (RAM & Disk)

    Engine->>WAL: recover(heap)
    Note over WAL: Scan log file from byte 0 to EOF
    loop For each WAL record in log
        WAL->>WAL: verify_crc32(record)
        alt CRC32 Mismatch
            Note over WAL: Corrupted record / torn tail -> Stop scan!
        else CRC32 Valid
            alt Record is COMMIT / ABORT
                WAL->>WAL: Track committed_txs set
            else Record is INSERT / UPDATE
                WAL->>WAL: Add to redo_records list
            end
        end
    end
    loop For each committed record in redo_records
        WAL->>Heap: replay_record(record)
        Note over Heap: Reapplies physical insert/update.<br/>Updates page.pd_lsn = record.lsn.
    end
    WAL-->>Engine: Replay complete (Survives power failure!)
```
