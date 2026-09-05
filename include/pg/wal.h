#pragma once

#include "pg/constants.h"
#include "pg/tuple.h"
#include "pg/tx.h"
#include <cstdint>
#include <vector>
#include <string>
#include "pg/file.h"
#include <memory>
#include <optional>
#include <unordered_set>

namespace pg {

class HeapFile;
class BufferPoolManager;
class ToastManager;

using lsn_t = uint64_t;
constexpr lsn_t INVALID_LSN = 0;

enum class WALRecordType : uint8_t {
    INVALID = 0,
    INSERT = 1,
    UPDATE = 2,
    COMMIT = 3,
    ABORT = 4,
    CHECKPOINT = 5,
    FPI = 6, // Full-Page Image (Torn-page protection)
    CLR = 7, // Compensation Log Record (Undo compensation)
    TOAST_INSERT = 8 // TOAST chunk insertion
};

#pragma pack(push, 1)
// 35-byte binary header for each WAL record in wal.log
struct WALRecordHeader {
    lsn_t         lsn{0};            // Byte offset of this record in WAL file
    lsn_t         prev_lsn{0};       // Byte offset of previous record
    tx_id_t       tx_id{0};          // Transaction that produced this log
    WALRecordType type{WALRecordType::INVALID}; // Record type
    page_id_t     page_id{INVALID_PAGE_ID};     // Affected heap page ID
    slot_id_t     slot_id{INVALID_SLOT_ID};     // Affected slot ID
    uint32_t      payload_len{0};    // Byte length of following payload
    uint32_t      crc{0};            // CRC32 checksum for corruption detection
};
#pragma pack(pop)

static_assert(sizeof(WALRecordHeader) == 35, "WALRecordHeader must be exactly 35 bytes");

// Complete in-memory WAL record
struct WALRecord {
    WALRecordHeader header;
    std::vector<uint8_t> payload;

    uint32_t calculate_crc() const;
    bool verify_crc() const;
};

// PostgreSQL Write-Ahead Logging Manager
// Enforces:
// 1. Fast sequential binary log append (wal.log)
// 2. Monotonically advancing Log Sequence Numbers (LSN)
// 3. Write-Ahead Rule (wal.flushed_lsn >= page.pd_lsn)
// 4. Checkpoint records and Full-Page Images (FPI) for torn-page healing
// 5. Fast crash recovery starting from last checkpoint LSN
class WALManager {
public:
    explicit WALManager(const std::string& wal_path, BufferPoolManager* bpm = nullptr);
    ~WALManager();

    // Factory method
    static std::unique_ptr<WALManager> open(const std::string& wal_path, BufferPoolManager* bpm = nullptr);

    // Logging operations
    lsn_t log_insert(tx_id_t tx_id, page_id_t page_id, slot_id_t slot_id, const HeapTuple& tuple);
    lsn_t log_update(tx_id_t tx_id, const CTID& old_ctid, const CTID& new_ctid, const HeapTuple& new_tuple);
    lsn_t log_commit(tx_id_t tx_id);
    lsn_t log_abort(tx_id_t tx_id);
    lsn_t log_checkpoint();
    lsn_t log_fpi(page_id_t page_id, const void* page_data);
    lsn_t log_toast_insert(tx_id_t tx_id, uint64_t toast_id, uint32_t chunk_seq, page_id_t page_id, slot_id_t slot_id, const void* chunk_data, size_t chunk_len);

    // Make every record up to target_lsn durable. This is a real fsync, not a
    // userspace buffer flush: it is the barrier that COMMIT and dirty-page
    // eviction both depend on.
    void flush(lsn_t target_lsn);
    void flush();

    // Named for the caller's intent: "do not proceed until the log covering this
    // page LSN is on disk". Cheap when already satisfied.
    void flush_up_to(lsn_t page_lsn) {
        if (page_lsn > flushed_lsn_) {
            flush(page_lsn);
        }
    }

    // Has this page already been given a full-page image since the last
    // checkpoint? Torn-page protection only needs the first write after a
    // checkpoint to carry the whole image.
    bool needs_fpi(page_id_t page_id) const {
        return fpi_written_pages_.find(page_id) == fpi_written_pages_.end();
    }

    // LSN inspection
    lsn_t current_lsn() const { return current_lsn_; }
    lsn_t flushed_lsn() const { return flushed_lsn_; }
    lsn_t checkpoint_lsn() const { return checkpoint_lsn_; }

    // Buffer pool wiring
    void set_bpm(BufferPoolManager* bpm) { bpm_ = bpm; }
    BufferPoolManager* bpm() const { return bpm_; }

    // Crash Recovery: Scans WAL from checkpoint_lsn, verifies CRCs, restores FPI baselines,
    // and replays all committed operations onto the heap and toast relations
    size_t recover(HeapFile& heap, TransactionManager& tm, ToastManager* toast = nullptr);

    // Truncate the log. Used after a checkpoint proves every prior record is
    // reflected on disk, so recovery need never look further back.
    void reset_after_checkpoint();

private:
    std::string wal_path_;
    BufferPoolManager* bpm_{nullptr};
    File file_;
    lsn_t current_lsn_{0};
    lsn_t flushed_lsn_{0};
    lsn_t prev_lsn_{0};
    lsn_t checkpoint_lsn_{0};
    std::unordered_set<page_id_t> fpi_written_pages_;

    lsn_t append_record(WALRecordType type, tx_id_t tx_id, page_id_t page_id, slot_id_t slot_id, const void* data, size_t len);
};

} // namespace pg
