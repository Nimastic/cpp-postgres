#pragma once

#include "pg/constants.h"
#include "pg/pager.h"
#include "pg/page.h"
#include <cstdint>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <memory>
#include <stdexcept>
#include <string>

namespace pg {

using frame_id_t = uint32_t;
constexpr frame_id_t INVALID_FRAME_ID = std::numeric_limits<frame_id_t>::max();

// An 8KB memory frame in the shared buffer pool
struct BufferFrame {
    uint8_t   data[PAGE_SIZE];                         // Raw 8KB in-memory page buffer
    page_id_t page_id{INVALID_PAGE_ID};                // Disk page ID currently resident in this frame
    uint32_t  pin_count{0};                            // Number of concurrent readers/writers
    bool      is_dirty{false};                         // Has this page been modified in RAM?
    uint8_t   usage_count{0};                          // Clock-sweep usage counter (0 or 1+)

    void reset() {
        std::memset(data, 0, PAGE_SIZE);
        page_id = INVALID_PAGE_ID;
        pin_count = 0;
        is_dirty = false;
        usage_count = 0;
    }
};

// PostgreSQL Shared Buffers Manager (Buffer Pool)
// Enforces:
// 1. Fixed-size RAM pool (N frames of 8KB each)
// 2. Clock-Sweep (Second Chance) Eviction
// 3. Pinning / Unpinning (prevents evicting active pages)
// 4. Dirty Page writeback on eviction
class BufferPoolManager {
public:
    BufferPoolManager(Pager& pager, size_t pool_size);
    ~BufferPoolManager();

    // Disable copy
    BufferPoolManager(const BufferPoolManager&) = delete;
    BufferPoolManager& operator=(const BufferPoolManager&) = delete;

    // Fetch a page by page_id into RAM and pin it.
    // Cache Hit: returns resident memory pointer directly.
    // Cache Miss: evicts a victim page via Clock Sweep (if pool full), reads page from disk, and pins it.
    Page* fetch_page(page_id_t page_id);

    // Unpin a page after reading or writing.
    // is_dirty: mark true if the caller modified any bytes in the page buffer.
    bool unpin_page(page_id_t page_id, bool is_dirty);

    // Explicitly flush a specific page to disk if dirty
    bool flush_page(page_id_t page_id);

    // Flush all dirty pages resident in the buffer pool to disk
    void flush_all();

    // Pool metrics
    size_t pool_size() const { return pool_size_; }
    size_t resident_pages() const { return page_table_.size(); }
    bool is_resident(page_id_t page_id) const { return page_table_.find(page_id) != page_table_.end(); }
    uint32_t get_pin_count(page_id_t page_id) const;
    bool is_dirty(page_id_t page_id) const;

    // Direct access to underlying Pager
    Pager& pager() { return pager_; }

private:
    Pager& pager_;
    size_t pool_size_;
    std::vector<BufferFrame> frames_;
    std::unordered_map<page_id_t, frame_id_t> page_table_;
    std::vector<frame_id_t> free_list_;
    size_t clock_hand_{0};

    // Clock Sweep eviction: selects an unpinned frame (pin_count == 0) and flushes if dirty
    frame_id_t victim_frame();
};

} // namespace pg
