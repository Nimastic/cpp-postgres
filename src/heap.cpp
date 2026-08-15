#include "pg/heap.h"
#include <iostream>

namespace pg {

HeapFile::HeapFile(std::unique_ptr<Pager> pager) : pager_(std::move(pager)) {
    if (pager_ && pager_->num_pages() == 0) {
        // Automatically allocate and initialize Page 0 for a new heap file
        page_id_t pid = pager_->allocate_page();
        Page p0;
        pager_->write_page(pid, p0.data());
    }
}

std::unique_ptr<HeapFile> HeapFile::open(const std::string& filepath) {
    auto pager = Pager::open(filepath);
    return std::make_unique<HeapFile>(std::move(pager));
}

CTID HeapFile::insert(const ItemRecord& record, tx_id_t xmin) {
    HeapTuple tuple;
    tuple.header.xmin = xmin;
    tuple.header.xmax = 0;
    tuple.header.infomask = 0;
    tuple.data = record;

    page_id_t target_page_id = static_cast<page_id_t>(pager_->num_pages() - 1);
    std::vector<uint8_t> page_buffer(PAGE_SIZE, 0);
    pager_->read_page(target_page_id, page_buffer.data());

    Page page(page_buffer.data());
    slot_id_t slot = page.insert_tuple(&tuple, sizeof(HeapTuple));

    if (slot != INVALID_SLOT_ID) {
        // Tuple fit in the current page
        CTID assigned_ctid(target_page_id, slot);
        size_t tuple_len = 0;
        uint8_t* tuple_mem = const_cast<uint8_t*>(page.get_tuple_ptr(slot, &tuple_len));
        reinterpret_cast<HeapTuple*>(tuple_mem)->header.t_ctid = assigned_ctid;

        pager_->write_page(target_page_id, page.data());
        return assigned_ctid;
    }

    // Current page is full -> allocate a brand new page
    page_id_t new_page_id = pager_->allocate_page();
    Page new_page;
    slot_id_t new_slot = new_page.insert_tuple(&tuple, sizeof(HeapTuple));
    if (new_slot == INVALID_SLOT_ID) {
        throw std::runtime_error("HeapFile: Failed to insert tuple into fresh page.");
    }

    CTID assigned_ctid(new_page_id, new_slot);
    size_t tuple_len = 0;
    uint8_t* tuple_mem = const_cast<uint8_t*>(new_page.get_tuple_ptr(new_slot, &tuple_len));
    reinterpret_cast<HeapTuple*>(tuple_mem)->header.t_ctid = assigned_ctid;

    pager_->write_page(new_page_id, new_page.data());
    return assigned_ctid;
}

CTID HeapFile::update(const CTID& old_ctid, const ItemRecord& new_record, tx_id_t tx_id) {
    auto old_tuple_opt = get(old_ctid);
    if (!old_tuple_opt.has_value()) {
        throw std::runtime_error("HeapFile::update: Target tuple not found at CTID " + old_ctid.to_string());
    }

    // Non-in-place update:
    // 1. Insert new tuple version with xmin = tx_id
    CTID new_ctid = insert(new_record, tx_id);

    // 2. Mark old tuple version with xmax = tx_id and point t_ctid to new_ctid
    TupleHeader updated_header = old_tuple_opt->header;
    updated_header.xmax = tx_id;
    updated_header.t_ctid = new_ctid;

    if (!update_tuple_header(old_ctid, updated_header)) {
        throw std::runtime_error("HeapFile::update: Failed to stamp xmax on old tuple at " + old_ctid.to_string());
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

    std::vector<uint8_t> page_buffer(PAGE_SIZE, 0);
    pager_->read_page(ctid.page, page_buffer.data());

    Page page(page_buffer.data());
    size_t len = 0;
    const uint8_t* ptr = page.get_tuple_ptr(ctid.slot, &len);
    if (ptr == nullptr || len < sizeof(HeapTuple)) {
        return std::nullopt;
    }

    return HeapTuple::deserialize(ptr, len);
}

bool HeapFile::update_tuple_header(const CTID& ctid, const TupleHeader& new_header) {
    if (!ctid.is_valid() || ctid.page >= pager_->num_pages()) {
        return false;
    }

    std::vector<uint8_t> page_buffer(PAGE_SIZE, 0);
    pager_->read_page(ctid.page, page_buffer.data());

    Page page(page_buffer.data());
    size_t len = 0;
    const uint8_t* ptr = page.get_tuple_ptr(ctid.slot, &len);
    if (ptr == nullptr || len < sizeof(HeapTuple)) {
        return false;
    }

    auto* tuple_ptr = reinterpret_cast<HeapTuple*>(const_cast<uint8_t*>(ptr));
    tuple_ptr->header = new_header;

    pager_->write_page(ctid.page, page.data());
    return true;
}

std::vector<std::pair<CTID, HeapTuple>> HeapFile::seq_scan() {
    std::vector<std::pair<CTID, HeapTuple>> results;
    size_t total_pages = pager_->num_pages();
    std::vector<uint8_t> page_buffer(PAGE_SIZE, 0);

    for (page_id_t pid = 0; pid < total_pages; ++pid) {
        pager_->read_page(pid, page_buffer.data());
        Page page(page_buffer.data());

        size_t slots = page.num_slots();
        for (slot_id_t s = 1; s <= slots; ++s) {
            auto lp = page.get_line_pointer(s);
            if (lp.has_value() && lp->flags() == ItemFlags::NORMAL) {
                size_t len = 0;
                const uint8_t* ptr = page.get_tuple_ptr(s, &len);
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
    auto all_tuples = seq_scan();

    for (const auto& [ctid, tuple] : all_tuples) {
        if (is_tuple_visible(tuple.header, snapshot, tm)) {
            visible_tuples.emplace_back(ctid, tuple);
        }
    }

    return visible_tuples;
}

} // namespace pg
