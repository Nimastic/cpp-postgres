#include "pg/pager.h"
#include <vector>

namespace pg {

Pager::Pager(const std::string& filepath)
    : filepath_(filepath), file_(File::open(filepath))
{
    update_num_pages();
}

std::unique_ptr<Pager> Pager::open(const std::string& filepath) {
    return std::make_unique<Pager>(filepath);
}

void Pager::update_num_pages() {
    if (!file_.is_open()) {
        num_pages_ = 0;
        return;
    }

    uint64_t file_size = file_.size();
    if (file_size % PAGE_SIZE != 0) {
        throw std::runtime_error("Pager: Corrupted database file (size is not a multiple of 8KB): " + filepath_);
    }
    num_pages_ = static_cast<size_t>(file_size / PAGE_SIZE);
}

page_id_t Pager::allocate_page() {
    if (!is_open()) {
        throw std::runtime_error("Pager: Cannot allocate page, file is not open.");
    }

    page_id_t new_page_id = static_cast<page_id_t>(num_pages_);
    std::vector<uint8_t> zero_buffer(PAGE_SIZE, 0);
    file_.write_at(static_cast<uint64_t>(new_page_id) * PAGE_SIZE, zero_buffer.data(), PAGE_SIZE);

    num_pages_++;
    return new_page_id;
}

void Pager::read_page(page_id_t page_id, void* out_buffer) {
    if (!is_open()) {
        throw std::runtime_error("Pager: Cannot read page, file is not open.");
    }
    if (page_id >= num_pages_) {
        throw std::out_of_range("Pager: Read out of bounds. Requested page_id " +
                                std::to_string(page_id) + " but total pages is " +
                                std::to_string(num_pages_));
    }

    file_.read_at(static_cast<uint64_t>(page_id) * PAGE_SIZE, out_buffer, PAGE_SIZE);
}

void Pager::write_page(page_id_t page_id, const void* in_buffer) {
    if (!is_open()) {
        throw std::runtime_error("Pager: Cannot write page, file is not open.");
    }
    if (page_id >= num_pages_) {
        throw std::out_of_range("Pager: Write out of bounds. Requested page_id " +
                                std::to_string(page_id) + " but total pages is " +
                                std::to_string(num_pages_));
    }

    file_.write_at(static_cast<uint64_t>(page_id) * PAGE_SIZE, in_buffer, PAGE_SIZE);
}

void Pager::sync() {
    file_.sync();
}

void Pager::close() {
    file_.close();
}

} // namespace pg
