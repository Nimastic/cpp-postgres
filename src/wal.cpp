#include "pg/wal.h"
#include "pg/heap.h"
#include <iostream>
#include <cstring>
#include <set>
#include <unordered_map>

namespace pg {

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

WALManager::WALManager(const std::string& wal_path) : wal_path_(wal_path) {
    stream_.open(wal_path_, std::ios::in | std::ios::out | std::ios::binary);
    if (!stream_.is_open()) {
        stream_.clear();
        stream_.open(wal_path_, std::ios::out | std::ios::binary | std::ios::trunc);
        stream_.close();
        stream_.open(wal_path_, std::ios::in | std::ios::out | std::ios::binary);
    }

    if (!stream_.is_open()) {
        throw std::runtime_error("WALManager: Failed to open WAL file: " + wal_path_);
    }

    stream_.seekp(0, std::ios::end);
    current_lsn_ = static_cast<lsn_t>(stream_.tellp());
    flushed_lsn_ = current_lsn_;
}

WALManager::~WALManager() {
    flush();
    if (stream_.is_open()) {
        stream_.close();
    }
}

std::unique_ptr<WALManager> WALManager::open(const std::string& wal_path) {
    return std::make_unique<WALManager>(wal_path);
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

    stream_.seekp(current_lsn_, std::ios::beg);
    stream_.write(reinterpret_cast<const char*>(&rec.header), sizeof(WALRecordHeader));
    if (!rec.payload.empty()) {
        stream_.write(reinterpret_cast<const char*>(rec.payload.data()), rec.payload.size());
    }

    prev_lsn_ = current_lsn_;
    current_lsn_ += sizeof(WALRecordHeader) + len;

    return rec.header.lsn;
}

lsn_t WALManager::log_insert(tx_id_t tx_id, page_id_t page_id, slot_id_t slot_id, const HeapTuple& tuple) {
    return append_record(WALRecordType::INSERT, tx_id, page_id, slot_id, &tuple, sizeof(HeapTuple));
}

#pragma pack(push, 1)
struct WALUpdatePayload {
    CTID old_ctid;
    CTID new_ctid;
    HeapTuple new_tuple;
};
#pragma pack(pop)

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

void WALManager::flush(lsn_t target_lsn) {
    if (stream_.is_open()) {
        stream_.flush();
        flushed_lsn_ = std::max(flushed_lsn_, target_lsn);
    }
}

void WALManager::flush() {
    flush(current_lsn_);
}

size_t WALManager::recover(HeapFile& heap, TransactionManager& tm) {
    flush();
    stream_.clear();
    stream_.seekg(0, std::ios::beg);

    std::vector<WALRecord> all_records;
    std::set<tx_id_t> committed_txs;
    std::set<tx_id_t> aborted_txs;

    // -------------------------------------------------------------------------
    // PASS 1: Scan all records from byte 0, verify CRC, and determine tx outcomes
    // -------------------------------------------------------------------------
    while (stream_.good() && stream_.peek() != EOF) {
        WALRecord rec;
        stream_.read(reinterpret_cast<char*>(&rec.header), sizeof(WALRecordHeader));
        if (stream_.gcount() < static_cast<std::streamsize>(sizeof(WALRecordHeader))) {
            break; // End of valid WAL
        }

        if (rec.header.payload_len > 0) {
            rec.payload.resize(rec.header.payload_len);
            stream_.read(reinterpret_cast<char*>(rec.payload.data()), rec.header.payload_len);
            if (stream_.gcount() < static_cast<std::streamsize>(rec.header.payload_len)) {
                break; // Torn log record at EOF
            }
        }

        if (!rec.verify_crc()) {
            std::cerr << "WAL Recovery: Corrupted WAL record detected at LSN " << rec.header.lsn << std::endl;
            break;
        }

        if (rec.header.type == WALRecordType::COMMIT) {
            committed_txs.insert(rec.header.tx_id);
            tm.set_status(rec.header.tx_id, TransactionStatus::COMMITTED);
        } else if (rec.header.type == WALRecordType::ABORT) {
            aborted_txs.insert(rec.header.tx_id);
            tm.set_status(rec.header.tx_id, TransactionStatus::ABORTED);
        }

        all_records.push_back(std::move(rec));
    }

    // -------------------------------------------------------------------------
    // PASS 2: REDO Replay onto Heap Pages
    // -------------------------------------------------------------------------
    size_t replayed_count = 0;
    std::vector<uint8_t> page_buffer(PAGE_SIZE, 0);

    for (const auto& rec : all_records) {
        lsn_t record_end_lsn = rec.header.lsn + sizeof(WALRecordHeader) + rec.header.payload_len;

        if (rec.header.type == WALRecordType::INSERT) {
            if (rec.payload.size() >= sizeof(HeapTuple)) {
                HeapTuple tuple = HeapTuple::deserialize(rec.payload.data(), rec.payload.size());
                page_id_t pid = rec.header.page_id;

                // Ensure heap file has allocated pages up to pid
                while (heap.num_pages() <= pid) {
                    heap.pager().allocate_page();
                    Page fresh_page;
                    heap.pager().write_page(heap.num_pages() - 1, fresh_page.data());
                }

                heap.pager().read_page(pid, page_buffer.data());
                Page page(page_buffer.data());

                // Check if page already has this change (LSN idempotency)
                if (page.header().pd_lsn <= rec.header.lsn) {
                    // Check if slot exists or needs insertion
                    auto lp_opt = page.get_line_pointer(rec.header.slot_id);
                    if (!lp_opt.has_value() || lp_opt->flags() == ItemFlags::UNUSED) {
                        page.insert_tuple(&tuple, sizeof(HeapTuple));
                    }
                    page.header().pd_lsn = record_end_lsn;
                    heap.pager().write_page(pid, page.data());
                    replayed_count++;
                }
            }
        } else if (rec.header.type == WALRecordType::UPDATE) {
            if (rec.payload.size() >= sizeof(WALUpdatePayload)) {
                WALUpdatePayload upd;
                std::memcpy(&upd, rec.payload.data(), sizeof(WALUpdatePayload));

                page_id_t old_pid = upd.old_ctid.page;
                page_id_t new_pid = upd.new_ctid.page;

                while (heap.num_pages() <= std::max(old_pid, new_pid)) {
                    heap.pager().allocate_page();
                    Page fresh_page;
                    heap.pager().write_page(heap.num_pages() - 1, fresh_page.data());
                }

                if (old_pid == new_pid) {
                    // Same-page update
                    heap.pager().read_page(old_pid, page_buffer.data());
                    Page page(page_buffer.data());

                    if (page.header().pd_lsn <= rec.header.lsn) {
                        // 1. Stamp old tuple
                        size_t old_len = 0;
                        const uint8_t* old_ptr = page.get_tuple_ptr(upd.old_ctid.slot, &old_len);
                        if (old_ptr != nullptr && old_len >= sizeof(HeapTuple)) {
                            auto* old_mem = reinterpret_cast<HeapTuple*>(const_cast<uint8_t*>(old_ptr));
                            old_mem->header.xmax = rec.header.tx_id;
                            old_mem->header.t_ctid = upd.new_ctid;
                        }

                        // 2. Insert new tuple
                        auto lp_opt = page.get_line_pointer(upd.new_ctid.slot);
                        if (!lp_opt.has_value() || lp_opt->flags() == ItemFlags::UNUSED) {
                            page.insert_tuple(&upd.new_tuple, sizeof(HeapTuple));
                        }

                        page.header().pd_lsn = record_end_lsn;
                        heap.pager().write_page(old_pid, page.data());
                        replayed_count++;
                    }
                } else {
                    // Cross-page update
                    // 1. Stamp old page
                    heap.pager().read_page(old_pid, page_buffer.data());
                    Page old_page(page_buffer.data());
                    size_t old_len = 0;
                    const uint8_t* old_ptr = old_page.get_tuple_ptr(upd.old_ctid.slot, &old_len);
                    if (old_ptr != nullptr && old_len >= sizeof(HeapTuple)) {
                        auto* old_mem = reinterpret_cast<HeapTuple*>(const_cast<uint8_t*>(old_ptr));
                        old_mem->header.xmax = rec.header.tx_id;
                        old_mem->header.t_ctid = upd.new_ctid;
                        old_page.header().pd_lsn = record_end_lsn;
                        heap.pager().write_page(old_pid, old_page.data());
                    }

                    // 2. Insert on new page
                    heap.pager().read_page(new_pid, page_buffer.data());
                    Page new_page(page_buffer.data());
                    if (new_page.header().pd_lsn <= rec.header.lsn) {
                        auto lp_opt = new_page.get_line_pointer(upd.new_ctid.slot);
                        if (!lp_opt.has_value() || lp_opt->flags() == ItemFlags::UNUSED) {
                            new_page.insert_tuple(&upd.new_tuple, sizeof(HeapTuple));
                        }
                        new_page.header().pd_lsn = record_end_lsn;
                        heap.pager().write_page(new_pid, new_page.data());
                    }
                    replayed_count++;
                }
            }
        }
    }

    return replayed_count;
}

} // namespace pg
