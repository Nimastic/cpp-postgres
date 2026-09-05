#include "pg/heap.h"
#include "pg/buffer_pool.h"
#include "pg/wal.h"
#include <iostream>

namespace pg {

HeapFile::HeapFile(std::unique_ptr<Pager> pager, BufferPoolManager* bpm, const std::string& fsm_path)
    : pager_(std::move(pager)), bpm_(bpm)
{
    if (pager_ && pager_->num_pages() == 0) {
        // A brand new relation still needs page 0 to exist before anything can
        // pin it, so this one write goes through the storage manager.
        page_id_t pid = pager_->allocate_page();
        PageBuffer p0;
        pager_->write_page(pid, p0.data());
    }

    if (bpm_ == nullptr) {
        // No pool supplied, so the relation makes its own. Every page access
        // then goes through it, including VACUUM and recovery.
        owned_bpm_ = std::make_unique<BufferPoolManager>(*pager_, DEFAULT_POOL_FRAMES);
        bpm_ = owned_bpm_.get();
    }

    std::string actual_fsm_path = fsm_path;
    if (actual_fsm_path.empty() && pager_) {
        std::string p = pager_->filepath();
        size_t dot = p.rfind(".db");
        if (dot != std::string::npos) {
            actual_fsm_path = p.substr(0, dot) + "_fsm.db";
        } else {
            actual_fsm_path = p + "_fsm.db";
        }
    }

    if (!actual_fsm_path.empty()) {
        fsm_ = FreeSpaceMap::open(actual_fsm_path);
        // If FSM has 0 pages but heap has pages, register each heap page's free space
        if (fsm_->num_fsm_pages() == 0 && pager_->num_pages() > 0) {
            for (page_id_t pid = 0; pid < pager_->num_pages(); ++pid) {
                PinnedPage page = pin(pid);
                if (page) {
                    fsm_->update_page(pid, page->free_space());
                }
            }
        }
    }
}

void HeapFile::set_bpm(BufferPoolManager* bpm) {
    if (bpm == nullptr || bpm == bpm_) {
        return;
    }
    // Flush whatever the outgoing pool is holding before handing the relation
    // over, or those changes would be stranded in a discarded cache.
    if (bpm_ != nullptr) {
        bpm_->flush_all();
    }
    bpm_ = bpm;
    owned_bpm_.reset();
}

std::unique_ptr<HeapFile> HeapFile::open(const std::string& filepath, BufferPoolManager* bpm, const std::string& fsm_path) {
    auto pager = Pager::open(filepath);
    return std::make_unique<HeapFile>(std::move(pager), bpm, fsm_path);
}

void HeapFile::flush() {
    if (bpm_) {
        bpm_->flush_all();
    }
    if (pager_) {
        pager_->sync();
    }
    if (fsm_) {
        fsm_->flush();
    }
}

PinnedPage HeapFile::pin(page_id_t page_id) {
    return PinnedPage(*bpm_, page_id);
}

void HeapFile::maybe_log_fpi(Page& page, page_id_t page_id) {
    if (wal_ != nullptr && wal_->needs_fpi(page_id)) {
        wal_->log_fpi(page_id, page.data());
    }
}

CTID HeapFile::insert(const ItemRecord& record, tx_id_t xmin) {
    HeapTuple tuple;
    tuple.header.xmin = xmin;
    tuple.header.xmax = 0;
    tuple.header.infomask = 0;
    tuple.data = record;

    size_t needed = sizeof(HeapTuple) + sizeof(LinePointer);

    // 1. Fast O(log M) search via Free Space Map
    if (fsm_) {
        page_id_t target_pid = fsm_->search_page(needed);
        if (target_pid != INVALID_PAGE_ID && target_pid < pager_->num_pages()) {
            PinnedPage page = pin(target_pid);
            if (page && page->free_space() >= needed) {
                maybe_log_fpi(*page, target_pid);
                slot_id_t slot = page->insert_tuple(&tuple, sizeof(HeapTuple));
                if (slot != INVALID_SLOT_ID) {
                    CTID assigned(target_pid, slot);
                    auto* mem = reinterpret_cast<HeapTuple*>(page->get_tuple_ptr_mut(slot));
                    mem->header.t_ctid = assigned;

                    if (wal_ != nullptr) {
                        lsn_t lsn = wal_->log_insert(xmin, target_pid, slot, *mem);
                        page->set_lsn(lsn + 1);
                    }
                    page.mark_dirty();
                    fsm_->update_page(target_pid, page->free_space());
                    return assigned;
                }
            }
        }
    }

    // 2. Fallback: check last page
    size_t total = pager_->num_pages();
    if (total > 0) {
        page_id_t last_pid = static_cast<page_id_t>(total - 1);
        PinnedPage page = pin(last_pid);
        if (page && page->free_space() >= needed) {
            maybe_log_fpi(*page, last_pid);
            slot_id_t slot = page->insert_tuple(&tuple, sizeof(HeapTuple));
            if (slot != INVALID_SLOT_ID) {
                CTID assigned(last_pid, slot);
                auto* mem = reinterpret_cast<HeapTuple*>(page->get_tuple_ptr_mut(slot));
                mem->header.t_ctid = assigned;

                if (wal_ != nullptr) {
                    lsn_t lsn = wal_->log_insert(xmin, last_pid, slot, *mem);
                    page->set_lsn(lsn + 1);
                }
                page.mark_dirty();
                if (fsm_) {
                    fsm_->update_page(last_pid, page->free_space());
                }
                return assigned;
            }
        }
    }

    // 3. Every existing page is full: extend the relation through the pool.
    page_id_t new_page_id = INVALID_PAGE_ID;
    Page* fresh = bpm_->new_page(&new_page_id);
    slot_id_t new_slot = fresh->insert_tuple(&tuple, sizeof(HeapTuple));
    if (new_slot == INVALID_SLOT_ID) {
        bpm_->unpin_page(new_page_id, true);
        throw std::runtime_error("HeapFile: Failed to insert tuple into fresh page.");
    }

    CTID assigned(new_page_id, new_slot);
    auto* mem = reinterpret_cast<HeapTuple*>(fresh->get_tuple_ptr_mut(new_slot));
    mem->header.t_ctid = assigned;

    if (wal_ != nullptr) {
        lsn_t lsn = wal_->log_insert(xmin, new_page_id, new_slot, *mem);
        fresh->set_lsn(lsn + 1);
    }
    if (fsm_) {
        fsm_->update_page(new_page_id, fresh->free_space());
    }
    bpm_->unpin_page(new_page_id, true);
    return assigned;
}

CTID HeapFile::update(const CTID& old_ctid, const ItemRecord& new_record, tx_id_t tx_id) {
    auto old_tuple_opt = get(old_ctid);
    if (!old_tuple_opt.has_value()) {
        throw std::runtime_error("HeapFile::update: Target tuple not found at CTID " + old_ctid.to_string());
    }

    // New version first, so its CTID is known and can go into the update record.
    CTID new_ctid = insert(new_record, tx_id);

    PinnedPage page = pin(old_ctid.page);
    if (!page) {
        throw std::runtime_error("HeapFile::update: cannot pin page " + std::to_string(old_ctid.page));
    }

    size_t len = 0;
    uint8_t* ptr = page->get_tuple_ptr_mut(old_ctid.slot, &len);
    if (ptr == nullptr || len < sizeof(HeapTuple)) {
        throw std::runtime_error("HeapFile::update: Failed to stamp xmax on old tuple at " + old_ctid.to_string());
    }

    maybe_log_fpi(*page, old_ctid.page);

    auto* old_mem = reinterpret_cast<HeapTuple*>(ptr);
    HeapTuple new_tuple = *old_mem;
    new_tuple.data = new_record;
    new_tuple.header.xmin = tx_id;
    new_tuple.header.xmax = 0;
    new_tuple.header.t_ctid = new_ctid;

    if (wal_ != nullptr) {
        lsn_t lsn = wal_->log_update(tx_id, old_ctid, new_ctid, new_tuple);
        page->set_lsn(lsn + 1);
    }

    old_mem->header.xmax = tx_id;
    old_mem->header.t_ctid = new_ctid;
    page.mark_dirty();

    return new_ctid;
}

std::optional<CTID> HeapFile::hot_update(const CTID& old_ctid, const ItemRecord& new_record, tx_id_t tx_id) {
    if (!old_ctid.is_valid() || old_ctid.page >= pager_->num_pages()) {
        return std::nullopt;
    }

    PinnedPage page = pin(old_ctid.page);
    if (!page) return std::nullopt;

    size_t old_len = 0;
    const uint8_t* old_ptr = page->get_tuple_ptr(old_ctid.slot, &old_len);
    if (old_ptr == nullptr || old_len < sizeof(HeapTuple)) {
        return std::nullopt;
    }

    HeapTuple new_tuple;
    new_tuple.header.xmin = tx_id;
    new_tuple.header.xmax = 0;
    new_tuple.header.infomask = HEAP_ONLY_TUPLE; // Reachable only by following the chain
    new_tuple.data = new_record;

    if (page->free_space() < sizeof(HeapTuple) + sizeof(LinePointer)) {
        return std::nullopt; // No room on this page; caller falls back to a normal update
    }

    maybe_log_fpi(*page, old_ctid.page);

    slot_id_t new_slot = page->insert_tuple(&new_tuple, sizeof(HeapTuple));
    if (new_slot == INVALID_SLOT_ID) {
        return std::nullopt;
    }

    CTID new_ctid(old_ctid.page, new_slot);

    auto* new_mem = reinterpret_cast<HeapTuple*>(page->get_tuple_ptr_mut(new_slot));
    new_mem->header.t_ctid = new_ctid;

    auto* old_mem = reinterpret_cast<HeapTuple*>(page->get_tuple_ptr_mut(old_ctid.slot, &old_len));
    old_mem->header.xmax = tx_id;
    old_mem->header.t_ctid = new_ctid;
    old_mem->header.infomask |= HEAP_HOT_UPDATED;

    if (wal_ != nullptr) {
        lsn_t lsn = wal_->log_update(tx_id, old_ctid, new_ctid, *new_mem);
        page->set_lsn(lsn + 1);
    }
    page.mark_dirty();
    if (fsm_) {
        fsm_->update_page(old_ctid.page, page->free_space());
    }

    return new_ctid;
}

bool HeapFile::delete_tuple(const CTID& target_ctid, tx_id_t tx_id) {
    auto tuple_opt = get(target_ctid);
    if (!tuple_opt.has_value()) {
        return false;
    }

    TupleHeader updated_header = tuple_opt->header;
    updated_header.xmax = tx_id;
    return update_tuple_header(target_ctid, updated_header);
}

std::optional<HeapTuple> HeapFile::get(const CTID& ctid) {
    if (!ctid.is_valid() || ctid.page >= pager_->num_pages()) {
        return std::nullopt;
    }

    PinnedPage page = pin(ctid.page);
    if (!page) return std::nullopt;

    size_t len = 0;
    const uint8_t* ptr = page->get_tuple_ptr(ctid.slot, &len);
    if (ptr == nullptr || len < sizeof(HeapTuple)) {
        return std::nullopt;
    }

    return HeapTuple::deserialize(ptr, len);
}

bool HeapFile::update_tuple_header(const CTID& ctid, const TupleHeader& new_header) {
    if (!ctid.is_valid() || ctid.page >= pager_->num_pages()) {
        return false;
    }

    PinnedPage page = pin(ctid.page);
    if (!page) return false;

    size_t len = 0;
    uint8_t* ptr = page->get_tuple_ptr_mut(ctid.slot, &len);
    if (ptr == nullptr || len < sizeof(HeapTuple)) {
        return false;
    }

    maybe_log_fpi(*page, ctid.page);

    auto* tuple_ptr = reinterpret_cast<HeapTuple*>(ptr);
    tuple_ptr->header = new_header;

    if (wal_ != nullptr) {
        // A header-only change is logged as an update of the tuple onto itself:
        // redo re-applies the same xmax/t_ctid stamp.
        lsn_t lsn = wal_->log_update(new_header.xmax, ctid, ctid, *tuple_ptr);
        page->set_lsn(lsn + 1);
    }
    page.mark_dirty();
    return true;
}

std::vector<std::pair<CTID, HeapTuple>> HeapFile::seq_scan() {
    std::vector<std::pair<CTID, HeapTuple>> results;
    size_t total_pages = pager_->num_pages();

    for (page_id_t pid = 0; pid < total_pages; ++pid) {
        PinnedPage page = pin(pid);
        if (!page) continue;

        size_t slots = page->num_slots();
        for (slot_id_t s = 1; s <= slots; ++s) {
            auto lp = page->get_line_pointer(s);
            if (lp.has_value() && lp->flags() == ItemFlags::NORMAL) {
                size_t len = 0;
                const uint8_t* ptr = page->get_tuple_ptr(s, &len);
                if (ptr != nullptr && len >= sizeof(HeapTuple)) {
                    results.emplace_back(CTID(pid, s), HeapTuple::deserialize(ptr, len));
                }
            }
        }
    }

    return results;
}

std::vector<std::pair<CTID, HeapTuple>> HeapFile::seq_scan(const Snapshot& snapshot, const TransactionManager& tm) {
    std::vector<std::pair<CTID, HeapTuple>> visible_tuples;
    size_t total_pages = pager_->num_pages();

    // Filter while scanning rather than materialising the whole relation first.
    for (page_id_t pid = 0; pid < total_pages; ++pid) {
        PinnedPage page = pin(pid);
        if (!page) continue;

        size_t slots = page->num_slots();
        for (slot_id_t s = 1; s <= slots; ++s) {
            auto lp = page->get_line_pointer(s);
            if (!lp.has_value() || lp->flags() != ItemFlags::NORMAL) continue;

            size_t len = 0;
            const uint8_t* ptr = page->get_tuple_ptr(s, &len);
            if (ptr == nullptr || len < sizeof(HeapTuple)) continue;

            HeapTuple t = HeapTuple::deserialize(ptr, len);
            if (is_tuple_visible(t.header, snapshot, tm)) {
                visible_tuples.emplace_back(CTID(pid, s), t);
            }
        }
    }

    return visible_tuples;
}


std::optional<std::pair<CTID, HeapTuple>> HeapFile::hot_search(
    const CTID& root, const Snapshot& snapshot, const TransactionManager& tm) {

    if (!root.is_valid() || root.page >= pager_->num_pages()) {
        return std::nullopt;
    }

    PinnedPage page = pin(root.page);
    if (!page) return std::nullopt;

    slot_id_t slot = root.slot;

    // A chain lives entirely on one page, so it cannot be longer than the number
    // of line pointers on it. Bounding the walk keeps a corrupt t_ctid cycle
    // from spinning forever.
    const size_t max_steps = page->num_slots() + 1;

    for (size_t step = 0; step < max_steps; ++step) {
        auto lp = page->get_line_pointer(slot);
        if (!lp.has_value()) {
            return std::nullopt;
        }

        if (lp->flags() == ItemFlags::REDIRECT) {
            // Pruned root: lp_offset carries the successor slot number.
            slot = static_cast<slot_id_t>(lp->lp_offset);
            if (slot == INVALID_SLOT_ID) return std::nullopt;
            continue;
        }

        if (lp->flags() != ItemFlags::NORMAL) {
            return std::nullopt; // DEAD or UNUSED: the chain ends here
        }

        size_t len = 0;
        const uint8_t* ptr = page->get_tuple_ptr(slot, &len);
        if (ptr == nullptr || len < sizeof(HeapTuple)) {
            return std::nullopt;
        }

        HeapTuple tuple = HeapTuple::deserialize(ptr, len);
        if (is_tuple_visible(tuple.header, snapshot, tm)) {
            return std::make_pair(CTID(root.page, slot), tuple);
        }

        // Not visible. If this version was HOT-updated, its successor is on this
        // same page and may be the one the snapshot can see.
        bool has_successor = (tuple.header.infomask & HEAP_HOT_UPDATED) &&
                             tuple.header.t_ctid.page == root.page &&
                             tuple.header.t_ctid.slot != slot;
        if (!has_successor) {
            return std::nullopt;
        }
        slot = tuple.header.t_ctid.slot;
    }

    return std::nullopt;
}

} // namespace pg
