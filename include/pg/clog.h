#pragma once

#include "pg/constants.h"
#include "pg/pager.h"
#include "pg/tx.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pg {

// Number of transaction status slots per byte (2 bits each -> 4 tx per byte)
constexpr size_t CLOG_TXS_PER_BYTE = 4;

// Number of transactions recorded on a single 8KB CLOG page
// 8192 bytes * 4 tx/byte = 32,768 transactions per 8KB page
constexpr size_t CLOG_TXS_PER_PAGE = PAGE_SIZE * CLOG_TXS_PER_BYTE;

// 2-bit CLOG status masks
constexpr uint8_t CLOG_STATUS_MASK = 0x03; // 0b11

// PostgreSQL CLOG (Commit Log / pg_xact) Manager
// Enforces:
// 1. 2-bit-per-transaction persistent bitmap layout on 8KB disk pages
// 2. 32,768 transactions per 8KB page
// 3. Instant persistent commit/abort status retrieval without WAL replay
class CLogManager {
public:
    explicit CLogManager(std::unique_ptr<Pager> pager);
    ~CLogManager() = default;

    // Disable copy
    CLogManager(const CLogManager&) = delete;
    CLogManager& operator=(const CLogManager&) = delete;

    // Factory method
    static std::unique_ptr<CLogManager> open(const std::string& filepath);

    // Record the status of a transaction in the on-disk 2-bit bitmap
    void set_status(tx_id_t tx_id, TransactionStatus status);

    // Retrieve the persistent status of a transaction from the on-disk 2-bit bitmap
    TransactionStatus get_status(tx_id_t tx_id) const;

    // Diagnostics & Metrics
    size_t num_pages() const { return pager_->num_pages(); }
    Pager& pager() { return *pager_; }

private:
    std::unique_ptr<Pager> pager_;

    void ensure_page_exists(page_id_t target_page_id);
};

} // namespace pg
