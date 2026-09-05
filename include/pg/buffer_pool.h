#pragma once

#include "pg/constants.h"
#include "pg/pager.h"
#include "pg/page.h"
#include <cstdint>
#include <cstring>
#include <vector>
#include <deque>
#include <unordered_map>
#include <memory>
#include <stdexcept>
#include <string>

namespace pg {

using frame_id_t = uint32_t;
constexpr frame_id_t INVALID_FRAME_ID = std::numeric_limits<frame_id_t>::max();

// Ceiling on a frame's usage counter, matching PostgreSQL's BUF_USAGECOUNT_MAX.
// The cap is what makes the clock sweep terminate: with an unbounded counter a
// hot page needs as many hand passes as it had hits, and the sweep gives up
// while frames sit unpinned and evictable.
constexpr uint8_t BUF_USAGECOUNT_MAX = 5;

class WALManager;

// An 8KB memory frame in the shared buffer pool.
// Pinned, non-copyable, and addressed in place: `view` is a Page over this
// frame's own bytes, so callers mutate shared memory rather than a copy.
struct BufferFrame {
    alignas(8) uint8_t data[PAGE_SIZE]{};              // Raw 8KB in-memory page buffer
    Page      view{data};                              // Non-owning view over `data`
    page_id_t page_id{INVALID_PAGE_ID};                // Disk page ID currently resident in this frame
    uint32_t  pin_count{0};                            // Number of concurrent readers/writers
    bool      is_dirty{false};                         // Has this page been modified in RAM?
    uint8_t   usage_count{0};                          // Clock-sweep usage counter, capped at BUF_USAGECOUNT_MAX

    BufferFrame() = default;
    // `view` points into this object, so a frame must never be copied or moved.
    BufferFrame(const BufferFrame&) = delete;
    BufferFrame& operator=(const BufferFrame&) = delete;

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
    // Returns a view into the frame. The page stays pinned until unpin_page,
    // so the caller may read and write it in place.
    Page* fetch_page(page_id_t page_id);

    // Extend the relation by one page and return it pinned, so that relation
    // extension goes through the pool like every other page access.
    Page* new_page(page_id_t* out_page_id);

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

    // The WAL rule: a dirty page may not reach disk until the log records that
    // describe it are durable. Wiring the log in lets the pool enforce that at
    // the moment of eviction, which is the only place it can be enforced.
    void set_wal(WALManager* wal) { wal_ = wal; }

    // Cache statistics, for STATUS output and tests.
    size_t hits() const { return hits_; }
    size_t misses() const { return misses_; }
    size_t evictions() const { return evictions_; }

private:
    Pager& pager_;
    size_t pool_size_;
    std::deque<BufferFrame> frames_;   // deque: BufferFrame is pinned in place, so no reallocation
    std::unordered_map<page_id_t, frame_id_t> page_table_;
    std::vector<frame_id_t> free_list_;
    size_t clock_hand_{0};
    WALManager* wal_{nullptr};
    size_t hits_{0};
    size_t misses_{0};
    size_t evictions_{0};

    // Write a frame out, honouring the WAL rule first.
    void write_frame(BufferFrame& frame);

    // Clock Sweep eviction: selects an unpinned frame (pin_count == 0) and flushes if dirty
    frame_id_t victim_frame();
};

// RAII pin. Holding the pin for the whole read-modify-write is the entire point
// of a buffer pool: it is what stops the frame being evicted, or another writer
// interleaving, between the read and the write-back.
class PinnedPage {
public:
    PinnedPage() = default;
    PinnedPage(BufferPoolManager& bpm, page_id_t page_id)
        : bpm_(&bpm), page_id_(page_id), page_(bpm.fetch_page(page_id)) {}

    ~PinnedPage() { release(); }

    PinnedPage(const PinnedPage&) = delete;
    PinnedPage& operator=(const PinnedPage&) = delete;
    PinnedPage(PinnedPage&& o) noexcept
        : bpm_(o.bpm_), page_id_(o.page_id_), page_(o.page_), dirty_(o.dirty_) { o.page_ = nullptr; }
    PinnedPage& operator=(PinnedPage&& o) noexcept {
        if (this != &o) {
            release();
            bpm_ = o.bpm_; page_id_ = o.page_id_; page_ = o.page_; dirty_ = o.dirty_;
            o.page_ = nullptr;
        }
        return *this;
    }

    bool valid() const { return page_ != nullptr; }
    explicit operator bool() const { return valid(); }

    Page& operator*() const { return *page_; }
    Page* operator->() const { return page_; }
    Page* get() const { return page_; }
    page_id_t page_id() const { return page_id_; }

    // Declare the mutation. PostgreSQL calls this MarkBufferDirty, at the point
    // of the change rather than at unpin time.
    void mark_dirty() { dirty_ = true; }

    void release() {
        if (page_ != nullptr && bpm_ != nullptr) {
            bpm_->unpin_page(page_id_, dirty_);
            page_ = nullptr;
        }
    }

private:
    BufferPoolManager* bpm_{nullptr};
    page_id_t page_id_{INVALID_PAGE_ID};
    Page* page_{nullptr};
    bool dirty_{false};
};

} // namespace pg
