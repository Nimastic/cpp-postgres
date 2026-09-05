#pragma once

#include "pg/constants.h"
#include "pg/tuple.h"
#include <cstdint>
#include <vector>
#include <utility>
#include <string>

namespace pg {

using index_key_t = int32_t;

// Abstract Index Interface
// Implemented by both in-memory BTreeIndex (Item 6) and on-disk DiskBTree (Item 14).
// Allows the Engine, Vacuum, and query execution layers to operate polymorphically
// over any secondary index implementation.
class Index {
public:
    virtual ~Index() = default;

    // Insert an index entry (key -> CTID)
    virtual void insert_entry(index_key_t key, const CTID& ctid) = 0;

    // Find all candidate CTIDs for a given search key
    virtual std::vector<CTID> find_entries(index_key_t key) = 0;

    // Remove a specific (key, ctid) mapping (e.g. during VACUUM index pruning)
    virtual bool remove_entry(index_key_t key, const CTID& ctid) = 0;

    // Range scan: returns all (key, ctid) pairs where min_key <= key <= max_key
    virtual std::vector<std::pair<index_key_t, CTID>> range_scan(index_key_t min_key, index_key_t max_key) = 0;

    // Total count of index entries (including multi-version entries)
    virtual size_t num_entries() const = 0;

    // Count of distinct keys
    virtual size_t num_unique_keys() const = 0;

    // Human-readable diagnostic dump of index structure
    virtual std::string dump() const = 0;
};

} // namespace pg
