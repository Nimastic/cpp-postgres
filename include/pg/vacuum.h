#pragma once

#include "pg/heap.h"
#include "pg/tx.h"
#include "pg/index.h"
#include "pg/btree.h"
#include <cstddef>
#include <vector>

namespace pg {

struct VacuumStats {
    size_t pages_scanned{0};
    size_t dead_tuples_reclaimed{0};   // line pointers demoted to UNUSED
    size_t bytes_reclaimed{0};
    size_t index_entries_removed{0};
    size_t hot_roots_redirected{0};
};

// Three-phase VACUUM, following PostgreSQL's structure.
//
// A single pass that frees a line pointer outright is only safe on a table with
// no indexes. With an index present, freeing the slot immediately would let a
// later INSERT recycle it while a stale index entry still points there, and the
// index would resolve that key to an unrelated row. So the work splits:
//
//   Phase 1  scan the heap, mark dead tuples LP_DEAD, collect their TIDs
//   Phase 2  remove every index entry pointing at a collected TID
//   Phase 3  demote LP_DEAD to LP_UNUSED, then compact the page
//
// Only after phase 2 is a slot genuinely free.
class Vacuum {
public:
    // Vacuum the heap and every index given. Passing no index is only correct
    // for a table that truly has none.
    static VacuumStats run(HeapFile& heap, const TransactionManager& tm,
                           const std::vector<Index*>& indexes = {});

    // Convenience overload for a single index.
    static VacuumStats run(HeapFile& heap, const TransactionManager& tm, Index& index);

    // Backwards-compatible overloads for legacy BTreeIndex callers
    static VacuumStats run(HeapFile& heap, const TransactionManager& tm,
                           const std::vector<BTreeIndex*>& indexes);
    static VacuumStats run(HeapFile& heap, const TransactionManager& tm, BTreeIndex& index);

private:
    // Phase 1 for one page: flag dead tuples and collect their TIDs.
    static size_t collect_dead(Page& page, page_id_t page_id, tx_id_t oldest_xmin,
                               const TransactionManager& tm,
                               std::vector<std::pair<index_key_t, CTID>>& dead_out,
                               size_t* out_bytes, size_t* out_redirects);

    // Phase 3 for one page: LP_DEAD -> LP_UNUSED, then defragment.
    static size_t reclaim(Page& page);
};

} // namespace pg
