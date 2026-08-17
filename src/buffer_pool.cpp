#include "pg/buffer_pool.h"
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

Page* BufferPoolManager::fetch_page(page_id_t page_id) {
    // 1. Check if page is already resident in RAM (Cache Hit)
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        frame_id_t fid = it->second;
        frames_[fid].pin_count++;
        frames_[fid].usage_count++;
        return reinterpret_cast<Page*>(frames_[fid].data);
    }

    // 2. Cache Miss: Verify page exists on disk
    if (page_id >= pager_.num_pages()) {
        throw std::out_of_range("BufferPoolManager: Requested page_id " + std::to_string(page_id) +
                                " exceeds total on-disk pages " + std::to_string(pager_.num_pages()));
    }

    // 3. Acquire a frame: from free list or via Clock-Sweep eviction
    frame_id_t fid = INVALID_FRAME_ID;
    if (!free_list_.empty()) {
        fid = free_list_.back();
        free_list_.pop_back();
    } else {
        fid = victim_frame();
    }

    // 4. Read page bytes from disk into the frame
    pager_.read_page(page_id, frames_[fid].data);

    // 5. Initialize frame metadata
    frames_[fid].page_id = page_id;
    frames_[fid].pin_count = 1;
    frames_[fid].is_dirty = false;
    frames_[fid].usage_count = 1;

    // 6. Record in page table
    page_table_[page_id] = fid;

    return reinterpret_cast<Page*>(frames_[fid].data);
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
    if (frames_[fid].is_dirty && frames_[fid].page_id != INVALID_PAGE_ID) {
        pager_.write_page(frames_[fid].page_id, frames_[fid].data);
        frames_[fid].is_dirty = false;
    }

    return true;
}

void BufferPoolManager::flush_all() {
    for (const auto& [pid, fid] : page_table_) {
        if (frames_[fid].is_dirty && frames_[fid].page_id != INVALID_PAGE_ID) {
            pager_.write_page(frames_[fid].page_id, frames_[fid].data);
            frames_[fid].is_dirty = false;
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

frame_id_t BufferPoolManager::victim_frame() {
    size_t scan_limit = pool_size_ * 2; // Up to 2 full rotations of the clock hand

    for (size_t i = 0; i < scan_limit; ++i) {
        frame_id_t fid = static_cast<frame_id_t>(clock_hand_);
        clock_hand_ = (clock_hand_ + 1) % pool_size_;

        auto& frame = frames_[fid];

        // An active pinned page CANNOT be evicted
        if (frame.pin_count == 0) {
            if (frame.usage_count > 0) {
                // Second chance: decrement usage count and continue scanning
                frame.usage_count--;
            } else {
                // Found eviction victim!
                // If dirty, flush to disk before overwriting frame
                if (frame.is_dirty && frame.page_id != INVALID_PAGE_ID) {
                    pager_.write_page(frame.page_id, frame.data);
                    frame.is_dirty = false;
                }

                // Remove evicted page mapping from page table
                if (frame.page_id != INVALID_PAGE_ID) {
                    page_table_.erase(frame.page_id);
                }

                return fid;
            }
        }
    }

    throw std::runtime_error("BufferPoolManager: All buffer frames are pinned! Cannot evict any page.");
}

} // namespace pg
