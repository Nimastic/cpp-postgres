#pragma once

#include "pg/constants.h"
#include "pg/pager.h"
#include "pg/tx.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>

namespace pg {

// Number of transaction status slots per byte (2 bits each -> 4 tx per byte)
constexpr size_t CLOG_TXS_PER_BYTE = 4;

// Number of transactions recorded on a single 8KB CLOG page
// 8192 bytes * 4 tx/byte = 32,768 transactions per 8KB page
constexpr size_t CLOG_TXS_PER_PAGE = PAGE_SIZE * CLOG_TXS_PER_BYTE;

// 2-bit CLOG status masks
constexpr uint8_t CLOG_STATUS_MASK = 0x03; // 0b11

// Number of 8KB frames in the SLRU shared buffer pool
// 32 buffers * 32,768 tx/page = 1,048,576 active transactions resident in RAM (256 KB footprint)
constexpr size_t CLOG_SLRU_BUFFERS = 32;

// Individual SLRU buffer frame
struct SlruFrame {
    page_id_t page_id{INVALID_PAGE_ID};
    bool valid{false};
    bool dirty{false};
    uint64_t lru_counter{0};
    uint8_t data[PAGE_SIZE]{};
};

// PostgreSQL CLOG (Commit Log / pg_xact) Manager with SLRU Shared Memory Cache
// Enforces:
// 1. 2-bit-per-transaction persistent bitmap layout on 8KB disk pages
// 2. 32,768 transactions per 8KB page
// 3. O(1) in-memory status reads and writes via SLRU buffer cache without synchronous disk I/O
// 4. Deferred dirty frame writeback and fsync at checkpoints and shutdown
class CLogManager {
public:
    explicit CLogManager(std::unique_ptr<Pager> pager, size_t num_buffers = CLOG_SLRU_BUFFERS);
    ~CLogManager();

    // Disable copy
    CLogManager(const CLogManager&) = delete;
    CLogManager& operator=(const CLogManager&) = delete;

    // Factory method
    static std::unique_ptr<CLogManager> open(const std::string& filepath, size_t num_buffers = CLOG_SLRU_BUFFERS);

    // Record the status of a transaction in the in-memory SLRU cache
    void set_status(tx_id_t tx_id, TransactionStatus status);

    // Retrieve the status of a transaction from the SLRU cache or disk
    TransactionStatus get_status(tx_id_t tx_id) const;

    // Flush all dirty SLRU frames to disk and force durable sync
    void flush();

    // Diagnostics & Metrics
    size_t num_pages() const { return pager_->num_pages(); }
    size_t num_buffers() const { return frames_.size(); }
    size_t dirty_frames() const;
    size_t cache_hits() const { return cache_hits_; }
    size_t cache_misses() const { return cache_misses_; }
    Pager& pager() { return *pager_; }

private:
    std::unique_ptr<Pager> pager_;
    mutable std::vector<SlruFrame> frames_;
    mutable uint64_t lru_clock_{0};
    mutable size_t cache_hits_{0};
    mutable size_t cache_misses_{0};

    void ensure_page_exists(page_id_t target_page_id);
    size_t find_or_load_frame(page_id_t page_id) const;
    size_t victim_frame() const;
    void write_frame(size_t frame_idx) const;
};

} // namespace pg
