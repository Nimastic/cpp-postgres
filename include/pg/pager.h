#pragma once

#include "pg/constants.h"
#include "pg/file.h"
#include <string>
#include <memory>
#include <stdexcept>

namespace pg {

// Maps a relation file onto a linear array of fixed 8KB pages.
//
// This is the storage-manager layer (PostgreSQL's smgr/md.c). It knows nothing
// about page contents; it moves whole blocks and nothing else.
class Pager {
public:
    explicit Pager(const std::string& filepath);
    ~Pager() = default;

    Pager(const Pager&) = delete;
    Pager& operator=(const Pager&) = delete;
    Pager(Pager&&) noexcept = default;
    Pager& operator=(Pager&&) noexcept = default;

    static std::unique_ptr<Pager> open(const std::string& filepath);

    // Extend the relation by one zero-filled page and return its page_id.
    page_id_t allocate_page();

    void read_page(page_id_t page_id, void* out_buffer);
    void write_page(page_id_t page_id, const void* in_buffer);

    size_t num_pages() const { return num_pages_; }

    // Durability barrier for this relation. Checkpoints call this; ordinary page
    // writes do not, because the WAL is what makes them recoverable.
    void sync();

    // Retained for source compatibility. Positioned writes reach the OS
    // immediately, so this is a no-op; call sync() when you need durability.
    void flush() {}

    void close();
    bool is_open() const { return file_.is_open(); }
    const std::string& filepath() const { return filepath_; }

private:
    std::string filepath_;
    File file_;
    size_t num_pages_{0};

    void update_num_pages();
};

} // namespace pg
