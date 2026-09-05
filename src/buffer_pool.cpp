#include "pg/buffer_pool.h"
#include "pg/wal.h"
#include <iostream>

namespace pg {

BufferPoolManager::BufferPoolManager(Pager& pager, size_t pool_size)
    : pager_(pager), pool_size_(pool_size), frames_(pool_size) {
    if (pool_size_ == 0) {
        throw std::invalid_argument("BufferPoolManager: Pool size must be greater than 0");
    }
    for (size_t i = 0; i < pool_size_; ++i) {
        free_list_.push_back(static_cast<frame_id_t>(i));
    }
}

BufferPoolManager::~BufferPoolManager() {
    flush_all();
}

// The WAL rule, enforced at the only place it can be: a page carrying changes
// described by log records that are not yet durable must not reach disk, or a
// crash leaves the heap ahead of the log with no way to reconcile the two.
void BufferPoolManager::write_frame(BufferFrame& frame) {
    if (frame.page_id == INVALID_PAGE_ID) {
        return;
    }
    if (wal_ != nullptr) {
        Page page(frame.data);
        wal_->flush_up_to(page.lsn());
    }
    pager_.write_page(frame.page_id, frame.data);
    frame.is_dirty = false;
}

Page* BufferPoolManager::fetch_page(page_id_t page_id) {
    // 1. Cache hit
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        frame_id_t fid = it->second;
        frames_[fid].pin_count++;
        if (frames_[fid].usage_count < BUF_USAGECOUNT_MAX) {
            frames_[fid].usage_count++;
        }
        hits_++;
        return &frames_[fid].view;
    }

    // 2. Cache miss: the page must exist on disk
    if (page_id >= pager_.num_pages()) {
        throw std::out_of_range("BufferPoolManager: Requested page_id " + std::to_string(page_id) +
                                " exceeds total on-disk pages " + std::to_string(pager_.num_pages()));
    }
    misses_++;

    // 3. Acquire a frame: free list first, then clock sweep
    frame_id_t fid = INVALID_FRAME_ID;
    if (!free_list_.empty()) {
        fid = free_list_.back();
        free_list_.pop_back();
    } else {
        fid = victim_frame();
    }

    pager_.read_page(page_id, frames_[fid].data);

    frames_[fid].page_id = page_id;
    frames_[fid].pin_count = 1;
    frames_[fid].is_dirty = false;
    frames_[fid].usage_count = 1;
    page_table_[page_id] = fid;

    return &frames_[fid].view;
}

// Relation extension through the pool. Doing this through the Pager directly
// would put a page on disk that the pool does not know about, which is how the
// two copies drift apart.
Page* BufferPoolManager::new_page(page_id_t* out_page_id) {
    page_id_t pid = pager_.allocate_page();
    if (out_page_id != nullptr) {
        *out_page_id = pid;
    }
    Page* page = fetch_page(pid);
    page->init();
    auto it = page_table_.find(pid);
    if (it != page_table_.end()) {
        frames_[it->second].is_dirty = true;
    }
    return page;
}

bool BufferPoolManager::unpin_page(page_id_t page_id, bool is_dirty) {
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }

    frame_id_t fid = it->second;
    if (is_dirty) {
        frames_[fid].is_dirty = true;
    }
    if (frames_[fid].pin_count > 0) {
        frames_[fid].pin_count--;
    }
    return true;
}

bool BufferPoolManager::flush_page(page_id_t page_id) {
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }

    frame_id_t fid = it->second;
    if (frames_[fid].is_dirty) {
        write_frame(frames_[fid]);
    }
    return true;
}

void BufferPoolManager::flush_all() {
    for (const auto& [pid, fid] : page_table_) {
        if (frames_[fid].is_dirty) {
            write_frame(frames_[fid]);
        }
    }
}

uint32_t BufferPoolManager::get_pin_count(page_id_t page_id) const {
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return 0;
    }
    return frames_[it->second].pin_count;
}

bool BufferPoolManager::is_dirty(page_id_t page_id) const {
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }
    return frames_[it->second].is_dirty;
}

// Clock sweep. Because usage_count is capped at BUF_USAGECOUNT_MAX, at most
// BUF_USAGECOUNT_MAX full rotations are needed before some unpinned frame
// reaches zero, so the loop is bounded without needing an arbitrary iteration
// cap that could expire while frames are still evictable.
frame_id_t BufferPoolManager::victim_frame() {
    const size_t max_rotations = static_cast<size_t>(BUF_USAGECOUNT_MAX) + 2;
    const size_t scan_limit = pool_size_ * max_rotations;

    for (size_t i = 0; i < scan_limit; ++i) {
        frame_id_t fid = static_cast<frame_id_t>(clock_hand_);
        clock_hand_ = (clock_hand_ + 1) % pool_size_;

        auto& frame = frames_[fid];

        if (frame.pin_count > 0) {
            continue; // A pinned page is in use and can never be evicted
        }

        if (frame.usage_count > 0) {
            frame.usage_count--; // Second chance
            continue;
        }

        if (frame.is_dirty) {
            write_frame(frame);
        }
        if (frame.page_id != INVALID_PAGE_ID) {
            page_table_.erase(frame.page_id);
        }
        evictions_++;
        return fid;
    }

    throw std::runtime_error("BufferPoolManager: every frame is pinned; cannot evict. "
                             "A caller is holding pins it never released.");
}

} // namespace pg
