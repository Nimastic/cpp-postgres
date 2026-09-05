#include "pg/clog.h"
#include <iostream>
#include <vector>
#include <cstring>
#include <stdexcept>

namespace pg {

CLogManager::CLogManager(std::unique_ptr<Pager> pager, size_t num_buffers)
    : pager_(std::move(pager))
{
    size_t count = std::max<size_t>(1, num_buffers);
    frames_.resize(count);

    if (pager_ && pager_->num_pages() == 0) {
        // Initialize Page 0 with zeroes (all transactions start as IN_PROGRESS = 0b00)
        page_id_t pid = pager_->allocate_page();
        std::vector<uint8_t> blank_page(PAGE_SIZE, 0);
        pager_->write_page(pid, blank_page.data());
    }
}

CLogManager::~CLogManager() {
    try {
        flush();
    } catch (...) {
        // Destructors must never throw
    }
}

std::unique_ptr<CLogManager> CLogManager::open(const std::string& filepath, size_t num_buffers) {
    auto pager = Pager::open(filepath);
    return std::make_unique<CLogManager>(std::move(pager), num_buffers);
}

void CLogManager::ensure_page_exists(page_id_t target_page_id) {
    if (!pager_) return;
    while (pager_->num_pages() <= target_page_id) {
        page_id_t new_pid = pager_->allocate_page();
        std::vector<uint8_t> blank_page(PAGE_SIZE, 0);
        pager_->write_page(new_pid, blank_page.data());
    }
}

size_t CLogManager::victim_frame() const {
    // 1. Look for an empty/invalid slot
    for (size_t i = 0; i < frames_.size(); ++i) {
        if (!frames_[i].valid) {
            return i;
        }
    }

    // 2. Select the valid frame with the oldest (minimum) LRU counter
    size_t min_idx = 0;
    uint64_t min_lru = UINT64_MAX;
    for (size_t i = 0; i < frames_.size(); ++i) {
        if (frames_[i].lru_counter < min_lru) {
            min_lru = frames_[i].lru_counter;
            min_idx = i;
        }
    }
    return min_idx;
}

void CLogManager::write_frame(size_t frame_idx) const {
    if (frame_idx >= frames_.size() || !pager_) return;
    auto& frame = frames_[frame_idx];
    if (frame.valid && frame.dirty) {
        pager_->write_page(frame.page_id, frame.data);
        frame.dirty = false;
    }
}

size_t CLogManager::find_or_load_frame(page_id_t page_id) const {
    // 1. Check if page_id is already resident in SLRU cache
    for (size_t i = 0; i < frames_.size(); ++i) {
        if (frames_[i].valid && frames_[i].page_id == page_id) {
            frames_[i].lru_counter = ++lru_clock_;
            cache_hits_++;
            return i;
        }
    }

    // 2. Cache Miss: select victim frame
    cache_misses_++;
    size_t victim_idx = victim_frame();

    // Write back victim if dirty
    write_frame(victim_idx);

    auto& frame = frames_[victim_idx];
    if (pager_ && page_id < pager_->num_pages()) {
        pager_->read_page(page_id, frame.data);
    } else {
        std::memset(frame.data, 0, PAGE_SIZE);
    }

    frame.page_id = page_id;
    frame.valid = true;
    frame.dirty = false;
    frame.lru_counter = ++lru_clock_;

    return victim_idx;
}

void CLogManager::set_status(tx_id_t tx_id, TransactionStatus status) {
    page_id_t page_id = static_cast<page_id_t>(tx_id / CLOG_TXS_PER_PAGE);
    size_t tx_within_page = tx_id % CLOG_TXS_PER_PAGE;
    size_t byte_offset = tx_within_page / CLOG_TXS_PER_BYTE;
    size_t bit_shift = (tx_within_page % CLOG_TXS_PER_BYTE) * 2;

    ensure_page_exists(page_id);

    size_t frame_idx = find_or_load_frame(page_id);
    auto& frame = frames_[frame_idx];

    uint8_t status_bits = static_cast<uint8_t>(status) & CLOG_STATUS_MASK;
    frame.data[byte_offset] &= ~(CLOG_STATUS_MASK << bit_shift); // Clear 2 bits
    frame.data[byte_offset] |= (status_bits << bit_shift);       // Set 2 bits
    frame.dirty = true;
}

TransactionStatus CLogManager::get_status(tx_id_t tx_id) const {
    page_id_t page_id = static_cast<page_id_t>(tx_id / CLOG_TXS_PER_PAGE);
    if (pager_ && page_id >= pager_->num_pages()) {
        return TransactionStatus::IN_PROGRESS; // If page doesn't exist yet, tx is IN_PROGRESS (0b00)
    }

    size_t tx_within_page = tx_id % CLOG_TXS_PER_PAGE;
    size_t byte_offset = tx_within_page / CLOG_TXS_PER_BYTE;
    size_t bit_shift = (tx_within_page % CLOG_TXS_PER_BYTE) * 2;

    size_t frame_idx = find_or_load_frame(page_id);
    const auto& frame = frames_[frame_idx];

    uint8_t status_bits = (frame.data[byte_offset] >> bit_shift) & CLOG_STATUS_MASK;
    return static_cast<TransactionStatus>(status_bits);
}

void CLogManager::flush() {
    if (!pager_) return;

    for (size_t i = 0; i < frames_.size(); ++i) {
        write_frame(i);
    }
    pager_->sync();
}

size_t CLogManager::dirty_frames() const {
    size_t count = 0;
    for (const auto& frame : frames_) {
        if (frame.valid && frame.dirty) {
            count++;
        }
    }
    return count;
}

} // namespace pg
