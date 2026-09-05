#include "pg/fsm.h"
#include <cstring>
#include <iostream>

namespace pg {

FreeSpaceMap::FreeSpaceMap(std::unique_ptr<Pager> pager) : pager_(std::move(pager)) {
    if (pager_) {
        size_t np = pager_->num_pages();
        pages_.resize(np);
        dirty_.assign(np, false);
        for (size_t p = 0; p < np; ++p) {
            pager_->read_page(static_cast<page_id_t>(p), &pages_[p]);
        }
    }
}

FreeSpaceMap::~FreeSpaceMap() {
    try {
        flush();
    } catch (...) {
        // Never throw out of destructor
    }
}

std::unique_ptr<FreeSpaceMap> FreeSpaceMap::open(const std::string& fsm_filepath) {
    auto pager = Pager::open(fsm_filepath);
    return std::make_unique<FreeSpaceMap>(std::move(pager));
}

void FreeSpaceMap::ensure_capacity_for_heap_page(page_id_t heap_pid) {
    if (!pager_) return;

    size_t needed_fsm_pages = (heap_pid / FSM_LEAF_COUNT) + 1;
    while (pages_.size() < needed_fsm_pages) {
        page_id_t new_pid = pager_->allocate_page();
        FsmPage fresh{};
        pager_->write_page(new_pid, &fresh);
        pages_.push_back(fresh);
        dirty_.push_back(false);
    }
}

page_id_t FreeSpaceMap::search_page(size_t needed_bytes) {
    if (pages_.empty() || needed_bytes > PAGE_SIZE) {
        return INVALID_PAGE_ID;
    }

    uint8_t target_cat = required_category(needed_bytes);

    for (size_t fsm_pid = 0; fsm_pid < pages_.size(); ++fsm_pid) {
        // Quick root check: if the max category across the entire 4096-page range
        // is less than target_cat, skip the entire FSM page in O(1)!
        if (pages_[fsm_pid].nodes[0] < target_cat) {
            continue;
        }

        // Descend the binary max-heap tree
        size_t i = 0;
        while (i < FSM_INTERNAL_NODES) {
            size_t left = 2 * i + 1;
            size_t right = 2 * i + 2;

            if (left < FSM_TOTAL_NODES && pages_[fsm_pid].nodes[left] >= target_cat) {
                i = left;
            } else if (right < FSM_TOTAL_NODES && pages_[fsm_pid].nodes[right] >= target_cat) {
                i = right;
            } else {
                break;
            }
        }

        if (i >= FSM_INTERNAL_NODES && i < FSM_TOTAL_NODES) {
            size_t slot = i - FSM_INTERNAL_NODES;
            return static_cast<page_id_t>(fsm_pid * FSM_LEAF_COUNT + slot);
        }
    }

    return INVALID_PAGE_ID;
}

void FreeSpaceMap::update_page(page_id_t heap_page_id, size_t free_bytes) {
    ensure_capacity_for_heap_page(heap_page_id);

    size_t fsm_pid = heap_page_id / FSM_LEAF_COUNT;
    size_t slot = heap_page_id % FSM_LEAF_COUNT;
    size_t leaf = FSM_INTERNAL_NODES + slot;

    uint8_t new_cat = bytes_to_category(free_bytes);
    if (pages_[fsm_pid].nodes[leaf] == new_cat) {
        return; // No category change
    }

    pages_[fsm_pid].nodes[leaf] = new_cat;
    dirty_[fsm_pid] = true;

    // Bubble up maximum category to root
    size_t i = leaf;
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        size_t sibling = (i % 2 == 1) ? (i + 1) : (i - 1);
        uint8_t sibling_val = (sibling < FSM_TOTAL_NODES) ? pages_[fsm_pid].nodes[sibling] : 0;
        uint8_t new_max = std::max(pages_[fsm_pid].nodes[i], sibling_val);

        if (pages_[fsm_pid].nodes[parent] == new_max) {
            break; // Monotonic property holds above this level
        }

        pages_[fsm_pid].nodes[parent] = new_max;
        i = parent;
    }
}

uint8_t FreeSpaceMap::get_category(page_id_t heap_page_id) const {
    size_t fsm_pid = heap_page_id / FSM_LEAF_COUNT;
    if (fsm_pid >= pages_.size()) {
        return 0;
    }
    size_t slot = heap_page_id % FSM_LEAF_COUNT;
    return pages_[fsm_pid].nodes[FSM_INTERNAL_NODES + slot];
}

void FreeSpaceMap::flush() {
    if (!pager_) return;

    for (size_t fsm_pid = 0; fsm_pid < pages_.size(); ++fsm_pid) {
        if (dirty_[fsm_pid]) {
            pager_->write_page(static_cast<page_id_t>(fsm_pid), &pages_[fsm_pid]);
            dirty_[fsm_pid] = false;
        }
    }
    pager_->sync();
}

} // namespace pg
