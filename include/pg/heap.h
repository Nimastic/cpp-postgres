#pragma once

#include "pg/pager.h"
#include "pg/page.h"
#include "pg/tuple.h"
#include "pg/tx.h"
#include "pg/mvcc.h"
#include <memory>
#include <vector>
#include <optional>
#include <string>

namespace pg {

// Forward declare to avoid circular dependency
class BufferPoolManager;

class HeapFile {
public:
    explicit HeapFile(std::unique_ptr<Pager> pager, BufferPoolManager* bpm = nullptr);
    ~HeapFile() = default;

    // Disable copy
    HeapFile(const HeapFile&) = delete;
    HeapFile& operator=(const HeapFile&) = delete;

    // Move support
    HeapFile(HeapFile&&) noexcept = default;
    HeapFile& operator=(HeapFile&&) noexcept = default;

    // Factory method
    static std::unique_ptr<HeapFile> open(const std::string& filepath, BufferPoolManager* bpm = nullptr);

    // Insert a new ItemRecord into the heap. Returns the assigned CTID.
    // If all existing pages are full, a new 8KB page is automatically allocated.
    CTID insert(const ItemRecord& record, tx_id_t xmin = 0);

    // Non-in-place MVCC update: stamps xmax on old tuple and inserts new tuple with xmin
    CTID update(const CTID& old_ctid, const ItemRecord& new_record, tx_id_t tx_id);

    // HOT (Heap-Only Tuple) update: places new version on SAME page as old version.
    // Prerequisites: (1) indexed columns did not change, (2) same page has free space.
    // Returns the new CTID on the same page, or std::nullopt if HOT is not possible.
    // On success, the old tuple gets HEAP_HOT_UPDATED and the new tuple gets HEAP_ONLY_TUPLE.
    // NO index writes are needed!
    std::optional<CTID> hot_update(const CTID& old_ctid, const ItemRecord& new_record, tx_id_t tx_id);

    // MVCC delete: stamps xmax on target tuple
    bool delete_tuple(const CTID& target_ctid, tx_id_t tx_id);

    // Fetch a single tuple by its physical CTID (page_id, slot_id)
    std::optional<HeapTuple> get(const CTID& ctid);

    // Update the tuple header at a specific CTID (used for setting xmax / t_ctid)
    bool update_tuple_header(const CTID& ctid, const TupleHeader& new_header);

    // Physical Sequential Scan: Returns all physical tuples in heap (unfiltered)
    std::vector<std::pair<CTID, HeapTuple>> seq_scan();

    // MVCC Snapshot Sequential Scan: Returns only tuples visible to the given snapshot
    std::vector<std::pair<CTID, HeapTuple>> seq_scan(const Snapshot& snapshot, const TransactionManager& tm);

    // Pager inspection
    Pager& pager() { return *pager_; }
    const Pager& pager() const { return *pager_; }
    size_t num_pages() const { return pager_->num_pages(); }

    // Buffer pool access
    BufferPoolManager* bpm() { return bpm_; }
    void set_bpm(BufferPoolManager* bpm) { bpm_ = bpm; }

private:
    std::unique_ptr<Pager> pager_;
    BufferPoolManager* bpm_{nullptr}; // Optional: when non-null, all I/O goes through shared_buffers

    // Internal helpers: read/write pages through BPM when available, else direct Pager
    Page read_page_internal(page_id_t page_id, std::vector<uint8_t>& buffer);
    void write_page_internal(page_id_t page_id, const Page& page);
};

} // namespace pg
