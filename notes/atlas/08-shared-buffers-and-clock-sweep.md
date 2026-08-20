# Item 8: Shared Buffers & Clock-Sweep Replacement

**Confidence**: `verified`  
**Citations**: [include/pg/buffer_pool.h:1-75](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/include/pg/buffer_pool.h), [src/buffer_pool.cpp:1-125](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/src/buffer_pool.cpp), [tests/test_buffer_pool.cpp:1-120](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/tests/test_buffer_pool.cpp)

---

## 1. Buffer Pool Manager (Shared Buffers) Architecture

`BufferPoolManager` maintains a configurable pool of in-memory frames (defaulting to **16 frames**, each exactly 8,192 bytes) acting as an LRU-approximation cache between the execution engine and physical disk files.

```mermaid
flowchart TD
    subgraph RAM["RAM: Buffer Pool Manager (16 Frames)"]
        F0["Frame 0\n[Page 0, pin=1, dirty=1, use=3]"]
        F1["Frame 1\n[Page 5, pin=0, dirty=0, use=0]"]
        F2["Frame 2\n[Page 2, pin=0, dirty=1, use=2]"]
        FN["Frame 15\n[Page 9, pin=2, dirty=0, use=1]"]
    end

    subgraph ClockSweep["PostgreSQL Clock-Sweep Hand"]
        HAND["Clock Hand (frame_id)"] -->|Checks pin & usage| F1
    end

    subgraph Disk["Physical Disk File"]
        DISK_P["Heap / B-Tree Pages (.db)"]
    end

    F1 -->|If dirty, flush before reuse| DISK_P
```

---

## 2. Invariants & Clock-Sweep State Machine

1. **Pin Count Protection (`pin_count > 0`)**: Any frame currently pinned by an active query worker is immediately skipped by the clock-sweep hand; it can **never** be evicted while in use (`[src/buffer_pool.cpp:85]`).
2. **Second-Chance Algorithm (`usage_count`)**:
   - If `pin_count == 0` and `usage_count > 0`, the clock hand decrements `usage_count--` and advances to the next frame.
   - If `pin_count == 0` and `usage_count == 0`, the frame is selected as the eviction victim.
3. **Dirty Writeback Before Eviction**: If the victim frame has `is_dirty == true`, it is written to disk via `Pager::write_page()` before being repurposed for the new page.

---

## 3. Sequence Diagram: Page Fetch & Eviction Lifecycle

```mermaid
sequenceDiagram
    autonumber
    participant Subsystem as Engine / Heap / Index
    participant BPM as BufferPoolManager (src/buffer_pool.cpp)
    participant Disk as Physical Disk (Pager)

    Subsystem->>BPM: fetch_page(page_id = 42)
    alt Page 42 already resident in RAM (Cache Hit)
        BPM->>BPM: frame.pin_count++<br/>frame.usage_count++
        BPM-->>Subsystem: uint8_t* (Memory pointer to 8KB frame)
    else Cache Miss (All 16 frames occupied)
        loop Clock-Sweep Eviction Search
            BPM->>BPM: Check frame at clock_hand_
            alt pin_count > 0
                BPM->>BPM: Skip frame (in use)
            else pin_count == 0 and usage_count > 0
                BPM->>BPM: usage_count-- (Second chance granted)
            else pin_count == 0 and usage_count == 0
                BPM->>BPM: VICTIM IDENTIFIED!
            end
            BPM->>BPM: clock_hand_ = (clock_hand_ + 1) % 16
        end
        alt Victim frame is dirty
            BPM->>Disk: write_page(old_page_id, frame.data)
        end
        BPM->>Disk: read_page(42, frame.data)
        BPM->>BPM: Update page_table_[42] = victim_frame<br/>frame.pin_count = 1<br/>frame.usage_count = 1
        BPM-->>Subsystem: uint8_t* (Pointer to frame)
    end
```
