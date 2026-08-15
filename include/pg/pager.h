#pragma once

#include "pg/constants.h"
#include <fstream>
#include <string>
#include <memory>
#include <stdexcept>

namespace pg {

class Pager {
public:
    explicit Pager(const std::string& filepath);
    ~Pager();

    // Disable copy construction and assignment
    Pager(const Pager&) = delete;
    Pager& operator=(const Pager&) = delete;

    // Move operations
    Pager(Pager&& other) noexcept;
    Pager& operator=(Pager&& other) noexcept;

    // Factory method
    static std::unique_ptr<Pager> open(const std::string& filepath);

    // Allocate a new empty 8KB page at the end of the file and return its page_id
    page_id_t allocate_page();

    // Read 8192 bytes from page_id into out_buffer
    void read_page(page_id_t page_id, void* out_buffer);

    // Write 8192 bytes from in_buffer to page_id
    void write_page(page_id_t page_id, const void* in_buffer);

    // Total count of pages in file
    size_t num_pages() const;

    // Flush file stream
    void flush();

    // Explicit close
    void close();

    // Status check
    bool is_open() const;

    // Get filepath
    const std::string& filepath() const { return filepath_; }

private:
    std::string filepath_;
    mutable std::fstream stream_;
    size_t num_pages_{0};

    void update_num_pages();
};

} // namespace pg
