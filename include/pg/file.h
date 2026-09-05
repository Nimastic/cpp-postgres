#pragma once

#include <string>
#include <cstdint>
#include <cstddef>
#include <stdexcept>

namespace pg {

// A raw file descriptor wrapper with an explicit durability barrier.
//
// std::fstream cannot express durability: its flush() only drains the userspace
// buffer into the OS page cache, and the handle it wraps is not reachable, so
// there is no way to ask the kernel to put the bytes on stable storage. Every
// WAL implementation needs that barrier, so the storage layer owns a descriptor
// directly instead.
//
// Positioned reads and writes also mean no shared seek cursor and no sticky
// stream state: a read that hits end-of-file cannot poison a later write the way
// eofbit does on a std::fstream.
class File {
public:
    File() = default;
    ~File();

    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;

    // Open for read/write, creating the file if it does not exist.
    static File open(const std::string& path);

    bool is_open() const { return fd_ >= 0; }
    const std::string& path() const { return path_; }

    uint64_t size() const;

    // Positioned I/O. Both throw on short transfers rather than reporting them,
    // because a partial page read or write is never recoverable at this layer.
    void read_at(uint64_t offset, void* buffer, size_t len) const;
    void write_at(uint64_t offset, const void* buffer, size_t len);

    // Durability barrier: block until the kernel reports the data is on stable
    // storage. fdatasync/FlushFileBuffers, not a userspace flush.
    void sync();

    void close();

private:
    int fd_{-1};
    std::string path_;
};

} // namespace pg
