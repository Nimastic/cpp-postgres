#pragma once

#include "pg/constants.h"
#include "pg/pager.h"
#include <cstdint>
#include <memory>
#include <string>
#include <algorithm>
#include <vector>

namespace pg {

// FSM Tree Constants (Depth 12 Complete Binary Tree)
constexpr size_t FSM_TREE_DEPTH     = 12;
constexpr size_t FSM_LEAF_COUNT     = 4096;               // 2^12 leaf nodes per 8KB FSM page
constexpr size_t FSM_INTERNAL_NODES = 4095;               // 2^12 - 1 internal nodes
constexpr size_t FSM_TOTAL_NODES    = FSM_LEAF_COUNT + FSM_INTERNAL_NODES; // 8,191 nodes
constexpr size_t FSM_CATEGORIES     = 256;                // 0 to 255
constexpr size_t FSM_BYTES_PER_CAT  = 32;                 // 32 bytes per category step

#pragma pack(push, 1)
// 8,192-byte physical FSM page layout
struct FsmPage {
    uint8_t nodes[FSM_TOTAL_NODES]{}; // Complete binary max-heap: 0=root, 4095..8190=leaves
    uint8_t reserved{0};              // Pad to PAGE_SIZE (8192 bytes)
};
#pragma pack(pop)

static_assert(sizeof(FsmPage) == PAGE_SIZE, "FsmPage must be exactly 8192 bytes");

// Auxiliary Free Space Map (FSM) Manager
// Manages:
// 1. O(log M) binary max-heap tree search for pages with free space
// 2. O(log M) space updates when tuples are inserted, deleted, or vacuumed
// 3. Compact 1-byte categorical free-space discretization (0..255)
// 4. Multi-page scaling for large tables (> 4,096 heap pages per FSM page)
class FreeSpaceMap {
public:
    explicit FreeSpaceMap(std::unique_ptr<Pager> pager);
    ~FreeSpaceMap();

    FreeSpaceMap(const FreeSpaceMap&) = delete;
    FreeSpaceMap& operator=(const FreeSpaceMap&) = delete;
    FreeSpaceMap(FreeSpaceMap&&) noexcept = default;
    FreeSpaceMap& operator=(FreeSpaceMap&&) noexcept = default;

    static std::unique_ptr<FreeSpaceMap> open(const std::string& fsm_filepath);

    // Converts byte count to categorical value (0 .. 255)
    static uint8_t bytes_to_category(size_t free_bytes) {
        if (free_bytes >= PAGE_SIZE) return 255;
        return static_cast<uint8_t>(free_bytes / FSM_BYTES_PER_CAT);
    }

    // Converts required byte count to minimum target category (0 .. 255)
    static uint8_t required_category(size_t needed_bytes) {
        if (needed_bytes == 0) return 0;
        size_t cat = (needed_bytes + FSM_BYTES_PER_CAT - 1) / FSM_BYTES_PER_CAT;
        return static_cast<uint8_t>(std::min<size_t>(255, cat));
    }

    // Converts category to approximate available bytes
    static size_t category_to_bytes(uint8_t category) {
        return static_cast<size_t>(category) * FSM_BYTES_PER_CAT;
    }

    // Finds the first page with at least needed_bytes free space.
    // Returns heap page_id, or INVALID_PAGE_ID if no known page has sufficient space.
    page_id_t search_page(size_t needed_bytes);

    // Updates the recorded free space for heap_page_id in O(log M) time.
    void update_page(page_id_t heap_page_id, size_t free_bytes);

    // Gets the current category for heap_page_id
    uint8_t get_category(page_id_t heap_page_id) const;

    // Flush dirty FSM pages to disk
    void flush();

    size_t num_fsm_pages() const { return pages_.size(); }
    Pager& pager() { return *pager_; }

private:
    std::unique_ptr<Pager> pager_;
    std::vector<FsmPage> pages_;
    std::vector<bool> dirty_;

    void ensure_capacity_for_heap_page(page_id_t heap_pid);
};

} // namespace pg
