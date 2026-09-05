#pragma once

#include "pg/constants.h"
#include "pg/tuple.h"
#include "pg/tx.h"
#include "pg/mvcc.h"
#include "pg/heap.h"
#include "pg/index.h"
#include <cstdint>
#include <vector>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace pg {

// PostgreSQL B-Tree Index mapping key (item_id) -> physical heap CTIDs (page_id, slot_id)
// Note: As covered in the video, the index does NOT store column payloads or MVCC headers.
// It stores (key -> CTID). Multiple CTIDs can exist for the same key across updates.
class BTreeIndex : public Index {
public:
    BTreeIndex() = default;
    ~BTreeIndex() override = default;

    // Insert an index mapping (key -> ctid)
    void insert_entry(index_key_t key, const CTID& ctid) override;

    // Find all candidate CTIDs for a given key
    std::vector<CTID> find_entries(index_key_t key) override;
    std::vector<CTID> find_entries(index_key_t key) const;

    // Remove a specific (key, ctid) entry (e.g. during vacuum index pruning)
    bool remove_entry(index_key_t key, const CTID& ctid) override;

    // Total count of index entries (including multi-version entries)
    size_t num_entries() const override;

    // Count of distinct keys
    size_t num_unique_keys() const override;

    // Range scan: returns all (key, ctid) pairs where min_key <= key <= max_key
    std::vector<std::pair<index_key_t, CTID>> range_scan(index_key_t min_key, index_key_t max_key) override;
    std::vector<std::pair<index_key_t, CTID>> range_scan(index_key_t min_key, index_key_t max_key) const;

    // Clear all entries
    void clear();

    // Visual index dump
    std::string dump() const override;

private:
    std::multimap<index_key_t, CTID> tree_;
};

// Index-Assisted Point Query:
// 1. Searches Index for key -> candidate CTIDs
// 2. Fetches each candidate tuple from HeapFile
// 3. Evaluates MVCC visibility against the transaction Snapshot
// 4. Returns the single visible version
std::optional<std::pair<CTID, HeapTuple>> index_lookup(
    Index& index,
    HeapFile& heap,
    index_key_t key,
    const Snapshot& snapshot,
    const TransactionManager& tm
);

std::optional<std::pair<CTID, HeapTuple>> index_lookup(
    const BTreeIndex& index,
    HeapFile& heap,
    index_key_t key,
    const Snapshot& snapshot,
    const TransactionManager& tm
);

} // namespace pg
