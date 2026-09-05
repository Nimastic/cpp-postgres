#include "pg/vacuum.h"
#include "pg/buffer_pool.h"
#include <iostream>

namespace pg {

// Phase 1: decide which tuples are dead, flag them LP_DEAD, and note the index
// keys that will need cleaning. Nothing is freed here.
size_t Vacuum::collect_dead(Page& page, page_id_t page_id, tx_id_t oldest_xmin,
                            const TransactionManager& tm,
                            std::vector<std::pair<index_key_t, CTID>>& dead_out,
                            size_t* out_bytes, size_t* out_redirects) {
    size_t dead_count = 0;
    size_t reclaimed_bytes = 0;
    size_t redirects = 0;
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

        // The inserting transaction rolled back: this version never existed.
        if (tm.get_status(tuple.header.xmin) == TransactionStatus::ABORTED) {
            is_dead = true;
        }
        // Deleted or superseded, the deleter committed, and no snapshot that
        // could still see it remains open.
        else if (tuple.header.xmax != 0 &&
                 tm.get_status(tuple.header.xmax) == TransactionStatus::COMMITTED &&
                 tuple.header.xmax < oldest_xmin) {
            is_dead = true;
        }

        if (!is_dead) {
            continue;
        }

        // A HOT chain root cannot simply be freed: the index points at this slot
        // and the live version further down the chain is heap-only, reachable
        // through nothing else. Turn the root into a redirect so the index entry
        // keeps working while the tuple bytes go away.
        bool hot_root = (tuple.header.infomask & HEAP_HOT_UPDATED) &&
                        tuple.header.t_ctid.page == page_id &&
                        tuple.header.t_ctid.slot != s;
        if (hot_root) {
            LinePointer redirect;
            redirect.set(tuple.header.t_ctid.slot, 0, ItemFlags::REDIRECT);
            page.set_line_pointer(s, redirect);
            reclaimed_bytes += len;
            redirects++;
            continue;
        }

        LinePointer dead_lp;
        dead_lp.set(0, 0, ItemFlags::DEAD);
        page.set_line_pointer(s, dead_lp);
        dead_out.emplace_back(tuple.data.item_id, CTID(page_id, s));
        dead_count++;
        reclaimed_bytes += len;
    }

    if (out_bytes != nullptr) *out_bytes = reclaimed_bytes;
    if (out_redirects != nullptr) *out_redirects = redirects;
    return dead_count;
}

// Phase 3: the indexes are clean, so the slots can finally be released.
size_t Vacuum::reclaim(Page& page) {
    size_t freed = 0;
    size_t slots = page.num_slots();
    for (slot_id_t s = 1; s <= slots; ++s) {
        auto lp = page.get_line_pointer(s);
        if (lp.has_value() && lp->flags() == ItemFlags::DEAD) {
            LinePointer unused;
            unused.set(0, 0, ItemFlags::UNUSED);
            page.set_line_pointer(s, unused);
            freed++;
        }
    }
    if (freed > 0) {
        page.defragment();
    }
    return freed;
}

VacuumStats Vacuum::run(HeapFile& heap, const TransactionManager& tm, Index& index) {
    std::vector<Index*> one{&index};
    return run(heap, tm, one);
}

VacuumStats Vacuum::run(HeapFile& heap, const TransactionManager& tm, BTreeIndex& index) {
    Index& idx = index;
    return run(heap, tm, idx);
}

VacuumStats Vacuum::run(HeapFile& heap, const TransactionManager& tm,
                        const std::vector<BTreeIndex*>& indexes) {
    std::vector<Index*> idxs;
    for (auto* idx : indexes) idxs.push_back(idx);
    return run(heap, tm, idxs);
}

VacuumStats Vacuum::run(HeapFile& heap, const TransactionManager& tm,
                        const std::vector<Index*>& indexes) {
    // The cutoff is the oldest snapshot horizon in the system, not the oldest
    // running transaction id. A transaction that started long ago still holds a
    // snapshot that may need row versions deleted by transactions numbered
    // below it, so using the id would reclaim rows out from under it.
    tx_id_t oldest_xmin = tm.oldest_snapshot_xmin();

    VacuumStats stats;
    stats.pages_scanned = heap.num_pages();

    BufferPoolManager* bpm = heap.bpm();
    if (bpm == nullptr) {
        throw std::runtime_error("Vacuum: requires a buffer pool; reading the relation directly "
                                 "would race with pages cached in the pool");
    }

    // ---- Phase 1: flag dead tuples, collect their TIDs -------------------
    std::vector<std::pair<index_key_t, CTID>> dead_tids;
    for (page_id_t pid = 0; pid < stats.pages_scanned; ++pid) {
        PinnedPage page(*bpm, pid);
        if (!page) continue;

        size_t bytes = 0;
        size_t redirects = 0;
        size_t found = collect_dead(*page, pid, oldest_xmin, tm, dead_tids, &bytes, &redirects);
        if (found > 0 || redirects > 0) {
            stats.bytes_reclaimed += bytes;
            stats.hot_roots_redirected += redirects;
            page.mark_dirty();
        }
    }

    if (dead_tids.empty()) {
        return stats;
    }

    // ---- Phase 2: clean the indexes --------------------------------------
    // Until this completes, none of the flagged slots may be reused.
    for (Index* index : indexes) {
        if (index == nullptr) continue;
        for (const auto& [key, ctid] : dead_tids) {
            if (index->remove_entry(key, ctid)) {
                stats.index_entries_removed++;
            }
        }
    }

    // ---- Phase 3: release the slots and compact ---------------------------
    for (page_id_t pid = 0; pid < stats.pages_scanned; ++pid) {
        PinnedPage page(*bpm, pid);
        if (!page) continue;

        size_t freed = reclaim(*page);
        if (freed > 0) {
            stats.dead_tuples_reclaimed += freed;
            page.mark_dirty();
            heap.fsm().update_page(pid, page->free_space());
        }
    }

    heap.fsm().flush();
    return stats;
}

} // namespace pg
