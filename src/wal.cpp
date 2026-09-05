#include "pg/wal.h"
#include "pg/heap.h"
#include "pg/buffer_pool.h"
#include "pg/page.h"
#include <iostream>
#include <cstring>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>

namespace pg {

#pragma pack(push, 1)
struct WALUpdatePayload {
    CTID old_ctid;
    CTID new_ctid;
    HeapTuple new_tuple;
};
#pragma pack(pop)


static uint32_t crc32(const void* data, size_t len, uint32_t init_crc = 0) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t crc = ~init_crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        }
    }
    return ~crc;
}

uint32_t WALRecord::calculate_crc() const {
    WALRecordHeader temp = header;
    temp.crc = 0;
    uint32_t c = crc32(&temp, sizeof(WALRecordHeader));
    if (!payload.empty()) {
        c = crc32(payload.data(), payload.size(), c);
    }
    return c;
}

bool WALRecord::verify_crc() const {
    return header.crc == calculate_crc();
}

WALManager::WALManager(const std::string& wal_path, BufferPoolManager* bpm)
    : wal_path_(wal_path), bpm_(bpm), file_(File::open(wal_path))
{
    current_lsn_ = file_.size();
    flushed_lsn_ = current_lsn_;
}

WALManager::~WALManager() {
    try {
        flush();
    } catch (...) {
        // A throwing destructor would take the process down during unwinding.
    }
}

std::unique_ptr<WALManager> WALManager::open(const std::string& wal_path, BufferPoolManager* bpm) {
    return std::make_unique<WALManager>(wal_path, bpm);
}

lsn_t WALManager::append_record(WALRecordType type, tx_id_t tx_id, page_id_t page_id, slot_id_t slot_id, const void* data, size_t len) {
    WALRecord rec;
    rec.header.lsn = current_lsn_;
    rec.header.prev_lsn = prev_lsn_;
    rec.header.tx_id = tx_id;
    rec.header.type = type;
    rec.header.page_id = page_id;
    rec.header.slot_id = slot_id;
    rec.header.payload_len = static_cast<uint32_t>(len);

    if (data != nullptr && len > 0) {
        rec.payload.resize(len);
        std::memcpy(rec.payload.data(), data, len);
    }

    rec.header.crc = rec.calculate_crc();

    // One positioned write for the whole record. Because File uses pwrite-style
    // positioned I/O there is no shared cursor and no sticky error state, so a
    // preceding read that hit end-of-file cannot silently turn this into a
    // no-op the way std::fstream's eofbit did.
    std::vector<uint8_t> buf(sizeof(WALRecordHeader) + len);
    std::memcpy(buf.data(), &rec.header, sizeof(WALRecordHeader));
    if (len > 0) {
        std::memcpy(buf.data() + sizeof(WALRecordHeader), rec.payload.data(), len);
    }
    file_.write_at(current_lsn_, buf.data(), buf.size());

    prev_lsn_ = current_lsn_;
    current_lsn_ += buf.size();

    return rec.header.lsn;
}

lsn_t WALManager::log_insert(tx_id_t tx_id, page_id_t page_id, slot_id_t slot_id, const HeapTuple& tuple) {
    return append_record(WALRecordType::INSERT, tx_id, page_id, slot_id, &tuple, sizeof(HeapTuple));
}

lsn_t WALManager::log_update(tx_id_t tx_id, const CTID& old_ctid, const CTID& new_ctid, const HeapTuple& new_tuple) {
    WALUpdatePayload payload{old_ctid, new_ctid, new_tuple};
    return append_record(WALRecordType::UPDATE, tx_id, new_ctid.page, new_ctid.slot, &payload, sizeof(WALUpdatePayload));
}

lsn_t WALManager::log_commit(tx_id_t tx_id) {
    lsn_t lsn = append_record(WALRecordType::COMMIT, tx_id, INVALID_PAGE_ID, INVALID_SLOT_ID, nullptr, 0);
    flush(lsn); // Force WAL to disk on transaction commit (Write-Ahead Durability!)
    return lsn;
}

lsn_t WALManager::log_abort(tx_id_t tx_id) {
    lsn_t lsn = append_record(WALRecordType::ABORT, tx_id, INVALID_PAGE_ID, INVALID_SLOT_ID, nullptr, 0);
    flush(lsn);
    return lsn;
}

lsn_t WALManager::log_checkpoint() {
    // Order matters. Flush every dirty frame, force the relation to stable
    // storage, and only then record that the checkpoint happened -- so that a
    // recovery starting at this record can trust every page before it.
    if (bpm_ != nullptr) {
        bpm_->flush_all();
        bpm_->pager().sync();
    }

    lsn_t lsn = append_record(WALRecordType::CHECKPOINT, 0, INVALID_PAGE_ID, INVALID_SLOT_ID, nullptr, 0);
    checkpoint_lsn_ = lsn;
    fpi_written_pages_.clear(); // Next write to each page logs a fresh full-page image
    flush(current_lsn_);
    return lsn;
}

lsn_t WALManager::log_fpi(page_id_t page_id, const void* page_data) {
    lsn_t lsn = append_record(WALRecordType::FPI, 0, page_id, INVALID_SLOT_ID, page_data, PAGE_SIZE);
    fpi_written_pages_.insert(page_id);
    return lsn;
}

void WALManager::flush(lsn_t target_lsn) {
    if (!file_.is_open() || target_lsn <= flushed_lsn_) {
        return;
    }
    // fdatasync / _commit. This is the syscall the D in ACID rests on: without
    // it the bytes sit in the OS page cache and a power cut loses them.
    file_.sync();
    flushed_lsn_ = std::max(flushed_lsn_, std::min(target_lsn, current_lsn_));
}

void WALManager::flush() {
    flush(current_lsn_);
}

void WALManager::reset_after_checkpoint() {
    fpi_written_pages_.clear();
}

namespace {

// Every redo and undo action goes through the buffer pool so that RAM and disk
// cannot disagree afterwards. Recovery writing straight to the Pager was the
// root of the vacuum divergence bug: two writers, two paths, last flush wins.
struct RecoveryPager {
    HeapFile& heap;
    BufferPoolManager* bpm;

    void ensure_pages(page_id_t upto) {
        while (heap.num_pages() <= upto) {
            if (bpm != nullptr) {
                page_id_t pid = INVALID_PAGE_ID;
                bpm->new_page(&pid);
                bpm->unpin_page(pid, true);
            } else {
                heap.pager().allocate_page();
                PageBuffer fresh;
                heap.pager().write_page(static_cast<page_id_t>(heap.num_pages() - 1), fresh.data());
            }
        }
    }

    // Apply `fn` to a page, then stamp the page LSN so a second replay of the
    // same record is a no-op. This LSN comparison is what makes redo idempotent
    // and lets recovery survive a crash during recovery.
    template <typename Fn>
    bool with_page(page_id_t pid, lsn_t record_lsn, lsn_t end_lsn, Fn&& fn) {
        ensure_pages(pid);
        if (bpm != nullptr) {
            PinnedPage pin(*bpm, pid);
            if (!pin) return false;
            if (pin->lsn() > record_lsn) return false;  // already reflected on this page
            fn(*pin);
            pin->set_lsn(end_lsn);
            pin.mark_dirty();
            return true;
        }
        PageBuffer buf;
        heap.pager().read_page(pid, buf.data());
        if (buf->lsn() > record_lsn) return false;
        fn(*buf);
        buf->set_lsn(end_lsn);
        heap.pager().write_page(pid, buf.data());
        return true;
    }
};

} // namespace

size_t WALManager::recover(HeapFile& heap, TransactionManager& tm) {
    flush();

    RecoveryPager rp{heap, bpm_};

    std::vector<WALRecord> all_records;
    std::set<tx_id_t> all_active_candidates;
    std::set<tx_id_t> committed_txs;
    std::set<tx_id_t> aborted_txs;
    size_t last_checkpoint_idx = 0;
    bool found_checkpoint = false;

    // =========================================================================
    // PASS 1: ANALYSIS -- scan the log, find the last checkpoint, and work out
    // which transactions committed, aborted, or were still in flight at the crash.
    // =========================================================================
    const uint64_t log_size = file_.size();
    uint64_t offset = 0;

    while (offset + sizeof(WALRecordHeader) <= log_size) {
        WALRecord rec;
        file_.read_at(offset, &rec.header, sizeof(WALRecordHeader));

        uint64_t payload_off = offset + sizeof(WALRecordHeader);
        if (payload_off + rec.header.payload_len > log_size) {
            break; // Torn record at the tail of the log
        }
        if (rec.header.payload_len > 0) {
            rec.payload.resize(rec.header.payload_len);
            file_.read_at(payload_off, rec.payload.data(), rec.header.payload_len);
        }

        if (!rec.verify_crc()) {
            std::cerr << "WAL Recovery: Corrupted WAL record detected at LSN " << rec.header.lsn << std::endl;
            break;
        }

        if (rec.header.tx_id != 0) {
            all_active_candidates.insert(rec.header.tx_id);
        }

        if (rec.header.type == WALRecordType::COMMIT) {
            committed_txs.insert(rec.header.tx_id);
            tm.set_status(rec.header.tx_id, TransactionStatus::COMMITTED);
        } else if (rec.header.type == WALRecordType::ABORT) {
            aborted_txs.insert(rec.header.tx_id);
            tm.set_status(rec.header.tx_id, TransactionStatus::ABORTED);
        } else if (rec.header.type == WALRecordType::CHECKPOINT) {
            last_checkpoint_idx = all_records.size();
            checkpoint_lsn_ = rec.header.lsn;
            found_checkpoint = true;
        } else if (rec.header.type == WALRecordType::CLR) {
            // A compensation record proves its action was already undone. CLRs
            // are never themselves undone, which is what stops recovery looping
            // if the system crashes while recovering.
            aborted_txs.insert(rec.header.tx_id);
        }

        offset = payload_off + rec.header.payload_len;
        all_records.push_back(std::move(rec));
    }

    // Losers: seen in the log, never reached a COMMIT or ABORT record.
    std::unordered_set<tx_id_t> active_tx_table;
    for (tx_id_t tx_id : all_active_candidates) {
        if (committed_txs.find(tx_id) == committed_txs.end() &&
            aborted_txs.find(tx_id) == aborted_txs.end()) {
            active_tx_table.insert(tx_id);
        }
    }

    // =========================================================================
    // PASS 2: REDO -- repeat history for winners and losers alike, so the heap
    // is returned to its exact state at the moment of the crash before anything
    // is rolled back.
    // =========================================================================
    size_t replayed_count = 0;
    size_t start_idx = found_checkpoint ? last_checkpoint_idx : 0;

    for (size_t i = start_idx; i < all_records.size(); ++i) {
        const auto& rec = all_records[i];
        lsn_t end_lsn = rec.header.lsn + sizeof(WALRecordHeader) + rec.header.payload_len;

        if (rec.header.type == WALRecordType::FPI) {
            if (rec.payload.size() != PAGE_SIZE) continue;
            page_id_t pid = rec.header.page_id;
            rp.ensure_pages(pid);
            // A full-page image is applied unconditionally: its whole purpose is
            // to overwrite a page that may have been torn mid-write, so that
            // page's own LSN cannot be trusted.
            if (bpm_ != nullptr) {
                PinnedPage pin(*bpm_, pid);
                if (pin) {
                    std::memcpy(pin->data(), rec.payload.data(), PAGE_SIZE);
                    pin->set_lsn(end_lsn);
                    pin.mark_dirty();
                }
            } else {
                heap.pager().write_page(pid, rec.payload.data());
            }
            replayed_count++;

        } else if (rec.header.type == WALRecordType::INSERT) {
            if (rec.payload.size() < sizeof(HeapTuple)) continue;
            HeapTuple tuple = HeapTuple::deserialize(rec.payload.data(), rec.payload.size());
            slot_id_t slot = rec.header.slot_id;
            bool did = rp.with_page(rec.header.page_id, rec.header.lsn, end_lsn, [&](Page& page) {
                // Restore to the exact slot that was logged. Letting the page
                // choose its own slot would shift CTIDs and leave every index
                // entry pointing at the wrong row.
                page.insert_tuple_at(slot, &tuple, sizeof(HeapTuple));
            });
            if (did) replayed_count++;

        } else if (rec.header.type == WALRecordType::UPDATE) {
            if (rec.payload.size() < sizeof(WALUpdatePayload)) continue;
            WALUpdatePayload upd;
            std::memcpy(&upd, rec.payload.data(), sizeof(WALUpdatePayload));
            tx_id_t xid = rec.header.tx_id;

            bool did = rp.with_page(upd.old_ctid.page, rec.header.lsn, end_lsn, [&](Page& page) {
                size_t len = 0;
                uint8_t* ptr = page.get_tuple_ptr_mut(upd.old_ctid.slot, &len);
                if (ptr != nullptr && len >= sizeof(HeapTuple)) {
                    auto* old_mem = reinterpret_cast<HeapTuple*>(ptr);
                    old_mem->header.xmax = xid;
                    old_mem->header.t_ctid = upd.new_ctid;
                    if (upd.old_ctid.page == upd.new_ctid.page) {
                        old_mem->header.infomask |= HEAP_HOT_UPDATED;
                    }
                }
                if (upd.old_ctid.page == upd.new_ctid.page) {
                    page.insert_tuple_at(upd.new_ctid.slot, &upd.new_tuple, sizeof(HeapTuple));
                }
            });

            if (upd.old_ctid.page != upd.new_ctid.page) {
                rp.with_page(upd.new_ctid.page, rec.header.lsn, end_lsn, [&](Page& page) {
                    page.insert_tuple_at(upd.new_ctid.slot, &upd.new_tuple, sizeof(HeapTuple));
                });
            }
            if (did) replayed_count++;
        }
    }

    // =========================================================================
    // PASS 3: UNDO -- roll back the losers, newest action first, writing a
    // compensation record for each so the work is not repeated if we crash here.
    // =========================================================================
    if (!active_tx_table.empty()) {
        for (int i = static_cast<int>(all_records.size()) - 1; i >= static_cast<int>(start_idx); --i) {
            const auto& rec = all_records[i];
            if (active_tx_table.find(rec.header.tx_id) == active_tx_table.end()) {
                continue;
            }

            if (rec.header.type == WALRecordType::INSERT) {
                page_id_t pid = rec.header.page_id;
                slot_id_t slot = rec.header.slot_id;
                if (pid >= heap.num_pages()) continue;

                lsn_t clr = append_record(WALRecordType::CLR, rec.header.tx_id, pid, slot, nullptr, 0);
                lsn_t clr_end = clr + sizeof(WALRecordHeader);

                if (bpm_ != nullptr) {
                    PinnedPage pin(*bpm_, pid);
                    if (pin) {
                        LinePointer lp; lp.set(0, 0, ItemFlags::UNUSED);
                        pin->set_line_pointer(slot, lp);
                        pin->set_lsn(clr_end);
                        pin.mark_dirty();
                    }
                } else {
                    PageBuffer buf;
                    heap.pager().read_page(pid, buf.data());
                    LinePointer lp; lp.set(0, 0, ItemFlags::UNUSED);
                    buf->set_line_pointer(slot, lp);
                    buf->set_lsn(clr_end);
                    heap.pager().write_page(pid, buf.data());
                }

            } else if (rec.header.type == WALRecordType::UPDATE) {
                if (rec.payload.size() < sizeof(WALUpdatePayload)) continue;
                WALUpdatePayload upd;
                std::memcpy(&upd, rec.payload.data(), sizeof(WALUpdatePayload));

                lsn_t clr = append_record(WALRecordType::CLR, rec.header.tx_id,
                                          upd.new_ctid.page, upd.new_ctid.slot, nullptr, 0);
                lsn_t clr_end = clr + sizeof(WALRecordHeader);

                auto undo_old = [&](Page& page) {
                    size_t len = 0;
                    uint8_t* ptr = page.get_tuple_ptr_mut(upd.old_ctid.slot, &len);
                    if (ptr != nullptr && len >= sizeof(HeapTuple)) {
                        auto* old_mem = reinterpret_cast<HeapTuple*>(ptr);
                        old_mem->header.xmax = 0;
                        old_mem->header.t_ctid = upd.old_ctid;
                        old_mem->header.infomask &= static_cast<uint16_t>(~HEAP_HOT_UPDATED);
                    }
                };
                auto undo_new = [&](Page& page) {
                    LinePointer lp; lp.set(0, 0, ItemFlags::UNUSED);
                    page.set_line_pointer(upd.new_ctid.slot, lp);
                };

                if (upd.old_ctid.page == upd.new_ctid.page) {
                    if (upd.old_ctid.page < heap.num_pages()) {
                        rp.with_page(upd.old_ctid.page, clr_end, clr_end, [&](Page& page) {
                            undo_old(page); undo_new(page);
                        });
                    }
                } else {
                    if (upd.old_ctid.page < heap.num_pages()) {
                        rp.with_page(upd.old_ctid.page, clr_end, clr_end, undo_old);
                    }
                    if (upd.new_ctid.page < heap.num_pages()) {
                        rp.with_page(upd.new_ctid.page, clr_end, clr_end, undo_new);
                    }
                }
            }
        }

        // Finalise the losers. The ABORT record now actually reaches the log:
        // positioned writes have no sticky end-of-file state to suppress them.
        for (tx_id_t tx_id : active_tx_table) {
            tm.set_status(tx_id, TransactionStatus::ABORTED);
            log_abort(tx_id);
        }
    }

    if (bpm_ != nullptr) {
        bpm_->flush_all();
        bpm_->pager().sync();
    }
    flush();

    return replayed_count;
}

} // namespace pg
