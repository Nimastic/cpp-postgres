# Item 8: Shared Buffers (Buffer Pool Manager)

**Date:** 2026-08-18  
**Topic:** PostgreSQL `shared_buffers`, 8KB RAM Memory Frames, Clock-Sweep (Second Chance) Eviction, Pin/Unpin Concurrency Protocol, and Dirty Page Writeback  
**Status:** Complete & Verified  

---

## 1. Directory Structure & Attached Code Files

- **Buffer Pool Header**: [`include/pg/buffer_pool.h`](../include/pg/buffer_pool.h)
- **Buffer Pool Implementation**: [`src/buffer_pool.cpp`](../src/buffer_pool.cpp)
- **Unit Test Suite**: [`tests/test_buffer_pool.cpp`](../tests/test_buffer_pool.cpp)

---

## 2. Why Databases Need a Buffer Pool (`shared_buffers`)

In real database engines, queries and transactions **never** execute direct OS disk reads and writes on every operation:
1. NVMe / SSD / HDD disk random I/O is thousands of times slower than accessing DDR RAM.
2. PostgreSQL allocates a fixed pool of memory frames called **`shared_buffers`** (e.g. 128MB, 16GB).
3. All queries, index scans, and tuple updates operate **directly in RAM**.
4. The Buffer Pool Manager arbitrates cache hits, cache misses, page pinning, and disk writebacks.

```text
                  POSTGRESQL ENGINE
                          |
                          v
               +----------------------+
               |     SHARED BUFFERS   |  <-- Fixed array of 8KB frames in RAM
               | (Buffer Pool Manager)|
               +----------------------+
                     /          \
            (Cache Hit)      (Cache Miss / Eviction)
                 v                  v
             [ In RAM ]       [ Disk Pager (File I/O) ]
```

---

## 3. Buffer Frame Anatomy & Pinning Lifecycle

Each frame in the buffer pool contains:

```cpp
struct BufferFrame {
    uint8_t   data[PAGE_SIZE];          // Raw 8KB in-memory page buffer
    page_id_t page_id{INVALID_PAGE_ID}; // Disk page ID currently resident
    uint32_t  pin_count{0};             // Number of active concurrent readers/writers
    bool      is_dirty{false};          // Modified in RAM since read from disk?
    uint8_t   usage_count{0};           // Clock-sweep usage counter (0 or 1+)
};
```

### The Pin / Unpin Contract:
- **`fetch_page(page_id)`**:
  - Increments `pin_count`.
  - While `pin_count > 0`, the eviction engine **cannot touch or evict** this frame.
- **`unpin_page(page_id, is_dirty)`**:
  - Decrements `pin_count`.
  - Sets `is_dirty = true` if the caller wrote or updated any bytes in the 8KB page.

---

## 4. The Clock-Sweep Eviction Algorithm

When a cache miss occurs and all buffer frames are occupied, PostgreSQL uses the **Clock-Sweep (Second Chance)** algorithm:

```text
                          [ Clock Hand ]
                                |
                                v
                       +-----------------+
                       | Frame 0         |
                       | pin_count: 1    |  <-- Pinned! Cannot evict.
                       +-----------------+
                                | (Advance Hand)
                                v
                       +-----------------+
                       | Frame 1         |
                       | pin_count: 0    |
                       | usage_count: 1  |  <-- Second chance: decrement to 0, advance.
                       +-----------------+
                                | (Advance Hand)
                                v
                       +-----------------+
                       | Frame 2         |
                       | pin_count: 0    |
                       | usage_count: 0  |  <-- VICTIM SELECTED!
                       +-----------------+
                                |
               +----------------+----------------+
               | If is_dirty == true             | If is_dirty == false
               v                                 v
   [ Write 8KB to Disk Pager ]         [ Evict Frame Silently ]
               \                                 /
                +--------------+----------------+
                               v
                  [ Read New Page from Disk ]
```

---

## 5. Verification Results (`tests/test_buffer_pool.cpp`)

```text
--- REPRODUCING POSTGRESQL SHARED BUFFERS (BUFFER POOL) ---
[Step 1] Loading Pages 0, 1, 2 into 3-frame buffer pool...
 -> Pages 0, 1, 2 loaded into RAM frames. Pool full.
[Step 2] Testing Cache Hit on resident Page 0...
 -> Cache Hit verified! Pin count on Page 0 incremented to 2.
 -> All frames unpinned (pin_count = 0).

[Step 3] Fetching Page 3 to trigger Clock-Sweep eviction...
 -> Page 3 loaded into RAM. Clean victim frame evicted silently without disk write.

[Step 4] Modifying resident page and verifying dirty writeback upon eviction...
 -> Dirty Page 1 was evicted by Clock Sweep and automatically written to disk.
 -> Disk file verified: Modified bytes persisted onto disk Page 1!

[Step 5] Testing Pinning Protection (Active pages cannot be evicted)...
 -> Clock Sweep respected pins: Pinned Pages 0 & 1 preserved, unpinned Page 2 evicted!

>>> ITEM 8 (SHARED BUFFERS / BUFFER POOL) TESTS PASSED SUCCESSFULLY! <<<
```

---

## 6. Self-Check & Calibration Questions

- **Definition (`BUFFER-PIN-COUNT`)**: What happens if the buffer pool is completely full and a query attempts to evict a frame whose `pin_count` is 2?
- **Mechanism (`CLOCK-SWEEP-SECOND-CHANCE`)**: In the Clock-Sweep algorithm, what does the eviction hand do when it encounters an unpinned frame (`pin_count == 0`) with `usage_count = 1`?
- **System Design (`DIRTY-PAGE-WRITEBACK`)**: Why does PostgreSQL delay writing dirty buffer frames to disk until eviction or checkpointing instead of writing every `INSERT`/`UPDATE` directly to disk synchronously?
