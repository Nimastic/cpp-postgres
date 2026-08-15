#include "pg/pager.h"
#include <iostream>
#include <vector>
#include <cstring>

namespace pg {

Pager::Pager(const std::string& filepath) : filepath_(filepath) {
    // Try to open existing file for read + write binary
    stream_.open(filepath_, std::ios::in | std::ios::out | std::ios::binary);

    if (!stream_.is_open()) {
        // File does not exist yet; create empty file first
        std::ofstream create_file(filepath_, std::ios::out | std::ios::binary);
        if (!create_file.is_open()) {
            throw std::runtime_error("Pager: Failed to create database file: " + filepath_);
        }
        create_file.close();

        // Re-open with read + write binary
        stream_.open(filepath_, std::ios::in | std::ios::out | std::ios::binary);
        if (!stream_.is_open()) {
            throw std::runtime_error("Pager: Failed to open database file: " + filepath_);
        }
    }

    update_num_pages();
}

Pager::~Pager() {
    close();
}

Pager::Pager(Pager&& other) noexcept
    : filepath_(std::move(other.filepath_)),
      stream_(std::move(other.stream_)),
      num_pages_(other.num_pages_) {
    other.num_pages_ = 0;
}

Pager& Pager::operator=(Pager&& other) noexcept {
    if (this != &other) {
        close();
        filepath_ = std::move(other.filepath_);
        stream_ = std::move(other.stream_);
        num_pages_ = other.num_pages_;
        other.num_pages_ = 0;
    }
    return *this;
}

std::unique_ptr<Pager> Pager::open(const std::string& filepath) {
    return std::make_unique<Pager>(filepath);
}

void Pager::update_num_pages() {
    if (!stream_.is_open()) {
        num_pages_ = 0;
        return;
    }
    stream_.clear();
    stream_.seekg(0, std::ios::end);
    auto file_size = stream_.tellg();
    if (file_size < 0) {
        num_pages_ = 0;
        return;
    }

    if (static_cast<size_t>(file_size) % PAGE_SIZE != 0) {
        throw std::runtime_error("Pager: Corrupted database file (size is not a multiple of 8KB): " + filepath_);
    }

    num_pages_ = static_cast<size_t>(file_size) / PAGE_SIZE;
}

page_id_t Pager::allocate_page() {
    if (!is_open()) {
        throw std::runtime_error("Pager: Cannot allocate page, file is not open.");
    }

    page_id_t new_page_id = static_cast<page_id_t>(num_pages_);
    stream_.clear();
    stream_.seekp(static_cast<std::streamoff>(new_page_id) * PAGE_SIZE, std::ios::beg);

    std::vector<char> zero_buffer(PAGE_SIZE, 0);
    stream_.write(zero_buffer.data(), PAGE_SIZE);
    stream_.flush();

    if (!stream_.good()) {
        throw std::runtime_error("Pager: Failed to write zero buffer for page allocation.");
    }

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

    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(page_id) * PAGE_SIZE, std::ios::beg);
    stream_.read(reinterpret_cast<char*>(out_buffer), PAGE_SIZE);

    if (static_cast<size_t>(stream_.gcount()) != PAGE_SIZE) {
        throw std::runtime_error("Pager: Incomplete read for page_id " + std::to_string(page_id));
    }
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

    stream_.clear();
    stream_.seekp(static_cast<std::streamoff>(page_id) * PAGE_SIZE, std::ios::beg);
    stream_.write(reinterpret_cast<const char*>(in_buffer), PAGE_SIZE);
    stream_.flush();

    if (!stream_.good()) {
        throw std::runtime_error("Pager: Write failed for page_id " + std::to_string(page_id));
    }
}

size_t Pager::num_pages() const {
    return num_pages_;
}

void Pager::flush() {
    if (is_open()) {
        stream_.flush();
    }
}

void Pager::close() {
    if (stream_.is_open()) {
        stream_.flush();
        stream_.close();
    }
}

bool Pager::is_open() const {
    return stream_.is_open();
}

} // namespace pg
