#include "pg/clog.h"
#include <iostream>
#include <vector>
#include <stdexcept>

namespace pg {

CLogManager::CLogManager(std::unique_ptr<Pager> pager) : pager_(std::move(pager)) {
    if (pager_ && pager_->num_pages() == 0) {
        // Initialize Page 0 with zeroes (all transactions start as IN_PROGRESS = 0b00)
        page_id_t pid = pager_->allocate_page();
        std::vector<uint8_t> blank_page(PAGE_SIZE, 0);
        pager_->write_page(pid, blank_page.data());
    }
}

std::unique_ptr<CLogManager> CLogManager::open(const std::string& filepath) {
    auto pager = Pager::open(filepath);
    return std::make_unique<CLogManager>(std::move(pager));
}

void CLogManager::ensure_page_exists(page_id_t target_page_id) {
    while (pager_->num_pages() <= target_page_id) {
        page_id_t new_pid = pager_->allocate_page();
        std::vector<uint8_t> blank_page(PAGE_SIZE, 0);
        pager_->write_page(new_pid, blank_page.data());
    }
}

void CLogManager::set_status(tx_id_t tx_id, TransactionStatus status) {
    page_id_t page_id = static_cast<page_id_t>(tx_id / CLOG_TXS_PER_PAGE);
    size_t tx_within_page = tx_id % CLOG_TXS_PER_PAGE;
    size_t byte_offset = tx_within_page / CLOG_TXS_PER_BYTE;
    size_t bit_shift = (tx_within_page % CLOG_TXS_PER_BYTE) * 2;

    ensure_page_exists(page_id);

    std::vector<uint8_t> page_buf(PAGE_SIZE, 0);
    pager_->read_page(page_id, page_buf.data());

    uint8_t status_bits = static_cast<uint8_t>(status) & CLOG_STATUS_MASK;
    page_buf[byte_offset] &= ~(CLOG_STATUS_MASK << bit_shift); // Clear 2 bits
    page_buf[byte_offset] |= (status_bits << bit_shift);       // Set 2 bits

    pager_->write_page(page_id, page_buf.data());
}

TransactionStatus CLogManager::get_status(tx_id_t tx_id) const {
    page_id_t page_id = static_cast<page_id_t>(tx_id / CLOG_TXS_PER_PAGE);
    if (page_id >= pager_->num_pages()) {
        return TransactionStatus::IN_PROGRESS; // If page doesn't exist yet, tx is IN_PROGRESS (0b00)
    }

    size_t tx_within_page = tx_id % CLOG_TXS_PER_PAGE;
    size_t byte_offset = tx_within_page / CLOG_TXS_PER_BYTE;
    size_t bit_shift = (tx_within_page % CLOG_TXS_PER_BYTE) * 2;

    std::vector<uint8_t> page_buf(PAGE_SIZE, 0);
    pager_->read_page(page_id, page_buf.data());

    uint8_t status_bits = (page_buf[byte_offset] >> bit_shift) & CLOG_STATUS_MASK;
    return static_cast<TransactionStatus>(status_bits);
}

} // namespace pg
