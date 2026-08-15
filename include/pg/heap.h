#pragma once

#include "pg/pager.h"
#include "pg/page.h"
#include "pg/tuple.h"
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

    // Fetch a single tuple by its physical CTID (page_id, slot_id)
    std::optional<HeapTuple> get(const CTID& ctid);

    // Update the tuple header at a specific CTID (used in Item 4 for setting xmax)
    bool update_tuple_header(const CTID& ctid, const TupleHeader& new_header);

    // Sequential Scan: Returns all tuples in the heap file along with their CTIDs
    std::vector<std::pair<CTID, HeapTuple>> seq_scan();

    // Pager inspection
    Pager& pager() { return *pager_; }
    const Pager& pager() const { return *pager_; }
    size_t num_pages() const { return pager_->num_pages(); }

private:
    std::unique_ptr<Pager> pager_;
};

} // namespace pg
