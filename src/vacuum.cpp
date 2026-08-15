#include "pg/vacuum.h"
#include <iostream>

namespace pg {

size_t Vacuum::vacuum_page(Page& page, tx_id_t oldest_xmin, const TransactionManager& tm, size_t* out_bytes) {
    size_t dead_count = 0;
    size_t reclaimed_bytes = 0;
    size_t slots = page.num_slots();

    for (slot_id_t s = 1; s <= slots; ++s) {
        auto lp_opt = page.get_line_pointer(s);
        if (!lp_opt.has_value() || lp_opt->flags() != ItemFlags::NORMAL) {
            continue;
        }

        size_t len = 0;
        const uint8_t* ptr = page.get_tuple_ptr(s, &len);
        if (ptr == nullptr || len < sizeof(HeapTuple)) {
            continue;
        }

        HeapTuple tuple = HeapTuple::deserialize(ptr, len);
        bool is_dead = false;

        // Condition 1: Tuple creation was aborted
        if (tm.get_status(tuple.header.xmin) == TransactionStatus::ABORTED) {
            is_dead = true;
        }
        // Condition 2: Tuple was deleted/updated, deleter committed, AND xmax < oldest_active_xmin
        else if (tuple.header.xmax != 0 &&
                 tm.get_status(tuple.header.xmax) == TransactionStatus::COMMITTED &&
                 tuple.header.xmax < oldest_xmin) {
            is_dead = true;
        }

        if (is_dead) {
            LinePointer dead_lp;
            dead_lp.set(0, 0, ItemFlags::DEAD);
            page.set_line_pointer(s, dead_lp);
            dead_count++;
            reclaimed_bytes += len;
        }
    }

    if (dead_count > 0) {
        // Defragment and compact surviving tuples
        page.defragment();
    }

    if (out_bytes != nullptr) {
        *out_bytes = reclaimed_bytes;
    }

    return dead_count;
}

VacuumStats Vacuum::run(HeapFile& heap, const TransactionManager& tm) {
    tx_id_t oldest_xmin = tm.oldest_active_xmin();
    VacuumStats stats;
    stats.pages_scanned = heap.num_pages();

    std::vector<uint8_t> page_buffer(PAGE_SIZE, 0);

    for (page_id_t pid = 0; pid < stats.pages_scanned; ++pid) {
        heap.pager().read_page(pid, page_buffer.data());
        Page page(page_buffer.data());

        size_t bytes = 0;
        size_t reclaimed_count = vacuum_page(page, oldest_xmin, tm, &bytes);

        if (reclaimed_count > 0) {
            heap.pager().write_page(pid, page.data());
            stats.dead_tuples_reclaimed += reclaimed_count;
            stats.bytes_reclaimed += bytes;
        }
    }

    return stats;
}

} // namespace pg
