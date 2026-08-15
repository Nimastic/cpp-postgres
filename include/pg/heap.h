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

class HeapFile {
public:
    explicit HeapFile(std::unique_ptr<Pager> pager);
    ~HeapFile() = default;

    // Disable copy
    HeapFile(const HeapFile&) = delete;
    HeapFile& operator=(const HeapFile&) = delete;

    // Move support
    HeapFile(HeapFile&&) noexcept = default;
    HeapFile& operator=(HeapFile&&) noexcept = default;

    // Factory method
    static std::unique_ptr<HeapFile> open(const std::string& filepath);

    // Insert a new ItemRecord into the heap. Returns the assigned CTID.
    // If all existing pages are full, a new 8KB page is automatically allocated.
    CTID insert(const ItemRecord& record, tx_id_t xmin = 0);

    // Non-in-place MVCC update: stamps xmax on old tuple and inserts new tuple with xmin
    CTID update(const CTID& old_ctid, const ItemRecord& new_record, tx_id_t tx_id);

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

private:
    std::unique_ptr<Pager> pager_;
};

} // namespace pg
