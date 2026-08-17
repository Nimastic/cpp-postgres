#pragma once

#include "pg/constants.h"
#include "pg/tuple.h"
#include "pg/tx.h"
#include "pg/mvcc.h"
#include "pg/heap.h"
#include <cstdint>
#include <vector>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace pg {

using index_key_t = int32_t;

// PostgreSQL B-Tree Index mapping key (item_id) -> physical heap CTIDs (page_id, slot_id)
// Note: As covered in the video, the index does NOT store column payloads or MVCC headers.
// It stores (key -> CTID). Multiple CTIDs can exist for the same key across updates.
class BTreeIndex {
public:
    BTreeIndex() = default;
    ~BTreeIndex() = default;

    // Insert an index mapping (key -> ctid)
    void insert_entry(index_key_t key, const CTID& ctid);

    // Find all candidate CTIDs for a given key
    std::vector<CTID> find_entries(index_key_t key) const;

    // Remove a specific (key, ctid) entry (e.g. during vacuum index pruning)
    bool remove_entry(index_key_t key, const CTID& ctid);

    // Total count of index entries (including multi-version entries)
    size_t num_entries() const;

    // Count of distinct keys
    size_t num_unique_keys() const;

    // Range scan: returns all (key, ctid) pairs where min_key <= key <= max_key
    std::vector<std::pair<index_key_t, CTID>> range_scan(index_key_t min_key, index_key_t max_key) const;

    // Clear all entries
    void clear();

    // Visual index dump
    std::string dump() const;

private:
    std::multimap<index_key_t, CTID> tree_;
};

// Index-Assisted Point Query:
// 1. Searches BTreeIndex for key -> candidate CTIDs
// 2. Fetches each candidate tuple from HeapFile
// 3. Evaluates MVCC visibility against the transaction Snapshot
// 4. Returns the single visible version
std::optional<std::pair<CTID, HeapTuple>> index_lookup(
    const BTreeIndex& index,
    HeapFile& heap,
    index_key_t key,
    const Snapshot& snapshot,
    const TransactionManager& tm
);

} // namespace pg
