#pragma once

#include "pg/heap.h"
#include "pg/tx.h"
#include <cstddef>

namespace pg {

struct VacuumStats {
    size_t pages_scanned{0};
    size_t dead_tuples_reclaimed{0};
    size_t bytes_reclaimed{0};
};

class Vacuum {
public:
    // Run a full table vacuum pass over the heap file
    static VacuumStats run(HeapFile& heap, const TransactionManager& tm);

    // Vacuum and defragment a single page
    static size_t vacuum_page(Page& page, tx_id_t oldest_xmin, const TransactionManager& tm, size_t* out_bytes = nullptr);
};

} // namespace pg
