#pragma once

#include "pg/constants.h"
#include "pg/tuple.h"
#include "pg/pager.h"
#include "pg/page.h"
#include "pg/buffer_pool.h"
#include <cstdint>
#include <vector>
#include <memory>
#include <string>
#include <optional>
#include <algorithm>
#include <cstring>
#include "pg/index.h"

namespace pg {

// Flags for BTreePageHeader
constexpr uint8_t BTREE_LEAF = 0x01;
constexpr uint8_t BTREE_ROOT = 0x02;

// B-Tree Page Limits (Splits when exceeded)
constexpr size_t BTREE_MAX_LEAF_KEYS     = 64; // Max keys per leaf page before split
constexpr size_t BTREE_MAX_INTERNAL_KEYS = 64; // Max keys per internal page before split

#pragma pack(push, 1)
// 12-byte header stored at the start of every 8KB B-Tree disk page
struct BTreePageHeader {
    uint8_t   level{0};                       // 0 = Leaf node, 1+ = Internal node
    uint8_t   flags{0};                       // BTREE_LEAF, BTREE_ROOT
    uint16_t  num_keys{0};                    // Number of active keys on this page
    page_id_t right_sibling{INVALID_PAGE_ID}; // Linked-list pointer to right leaf sibling
    page_id_t parent{INVALID_PAGE_ID};        // Parent node page ID
};

// 10-byte entry stored on Leaf pages (Key -> CTID)
struct BTreeLeafEntry {
    index_key_t key{0};
    CTID        ctid{};
};

// 8-byte entry stored on Internal pages (Key -> Child Page ID)
struct BTreeInternalEntry {
    index_key_t key{0};
    page_id_t   child_page_id{INVALID_PAGE_ID};
};
#pragma pack(pop)

static_assert(sizeof(BTreePageHeader) == 12, "BTreePageHeader must be exactly 12 bytes");
static_assert(sizeof(BTreeLeafEntry) == 10, "BTreeLeafEntry must be exactly 10 bytes");
static_assert(sizeof(BTreeInternalEntry) == 8, "BTreeInternalEntry must be exactly 8 bytes");

// On-Disk B-Tree Index Manager
// Enforces:
// 1. Dedicated 8KB B-Tree disk pages managed by Pager & BufferPool
// 2. Leaf nodes storing (Key -> CTID) and Internal nodes storing (Key -> Child Page ID)
// 3. Node splitting with median promotion and root height increases
// 4. Linked leaf page traversal for fast range scans
// 5. Zero-heap-scan instant persistence across restarts
// 6. Conforms to abstract Index interface (used polymorphically by Engine and Vacuum)
class DiskBTree : public Index {
public:
    explicit DiskBTree(std::unique_ptr<Pager> pager, BufferPoolManager* bpm = nullptr);
    ~DiskBTree() override;

    // Disable copy
    DiskBTree(const DiskBTree&) = delete;
    DiskBTree& operator=(const DiskBTree&) = delete;

    // Factory method: opens or creates on-disk B-Tree index with an internal buffer pool
    static std::unique_ptr<DiskBTree> open(const std::string& filepath, BufferPoolManager* bpm = nullptr);

    // Insert an index entry (key -> ctid) with automatic node splitting
    void insert_entry(index_key_t key, const CTID& ctid) override;

    // Find all candidate CTIDs for a given key
    std::vector<CTID> find_entries(index_key_t key) override;

    // Remove a specific (key, ctid) entry (e.g. during VACUUM Phase 2 index pruning)
    bool remove_entry(index_key_t key, const CTID& ctid) override;

    // Range scan: returns all (key, ctid) pairs where min_key <= key <= max_key
    std::vector<std::pair<index_key_t, CTID>> range_scan(index_key_t min_key, index_key_t max_key) override;

    // Total count of entries across all leaf pages
    size_t num_entries() const override;

    // Count of distinct keys
    size_t num_unique_keys() const override;

    // Human-readable diagnostic dump of the tree structure
    std::string dump() const override;

    // Flush dirty buffer frames to disk and fsync the underlying file
    void flush();

    // Inspection
    page_id_t root_page_id() const { return root_page_id_; }
    size_t num_pages() const { return pager_->num_pages(); }
    Pager& pager() { return *pager_; }
    BufferPoolManager* bpm() { return bpm_; }

private:
    mutable std::unique_ptr<Pager> pager_;
    mutable std::unique_ptr<BufferPoolManager> bpm_owned_;
    mutable BufferPoolManager* bpm_{nullptr};
    page_id_t root_page_id_{0};

    // Internal Page I/O helpers
    void read_page(page_id_t page_id, void* buffer) const;
    void write_page(page_id_t page_id, const void* buffer);

    // Tree descent and split helpers
    page_id_t find_leaf_page(index_key_t key) const;
    page_id_t leftmost_leaf_page() const;
    void split_leaf_page(page_id_t leaf_pid);
    void insert_into_internal(page_id_t internal_pid, index_key_t key, page_id_t right_child_pid);
    void split_internal_page(page_id_t internal_pid);
};

} // namespace pg
