#include "pg/file.h"

#include <cerrno>
#include <cstring>
#include <utility>

#ifdef _WIN32
  #include <io.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #define PG_OPEN      ::_open
  #define PG_CLOSE     ::_close
  #define PG_READ      ::_read
  #define PG_WRITE     ::_write
  #define PG_LSEEK     ::_lseeki64
  #define PG_SYNC      ::_commit
  #define PG_OPEN_MODE (_O_RDWR | _O_CREAT | _O_BINARY)
  #define PG_PERMS     (_S_IREAD | _S_IWRITE)
#else
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #define PG_OPEN      ::open
  #define PG_CLOSE     ::close
  #define PG_READ      ::read
  #define PG_WRITE     ::write
  #define PG_LSEEK     ::lseek
  #define PG_OPEN_MODE (O_RDWR | O_CREAT)
  #define PG_PERMS     0644
  static int PG_SYNC(int fd) {
    #if defined(__APPLE__)
      return ::fsync(fd);   // fdatasync exists but does not flush the drive cache on macOS
    #else
      return ::fdatasync(fd);
    #endif
  }
#endif

namespace pg {

namespace {

std::string errno_text() {
    return std::string(std::strerror(errno));
}

} // namespace

File::~File() {
    close();
}

File::File(File&& other) noexcept : fd_(other.fd_), path_(std::move(other.path_)) {
    other.fd_ = -1;
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        path_ = std::move(other.path_);
        other.fd_ = -1;
    }
    return *this;
}

File File::open(const std::string& path) {
    File f;
    f.fd_ = PG_OPEN(path.c_str(), PG_OPEN_MODE, PG_PERMS);
    if (f.fd_ < 0) {
        throw std::runtime_error("File: cannot open " + path + ": " + errno_text());
    }
    f.path_ = path;
    return f;
}

uint64_t File::size() const {
    if (!is_open()) {
        return 0;
    }
    int64_t end = PG_LSEEK(fd_, 0, SEEK_END);
    if (end < 0) {
        throw std::runtime_error("File: cannot size " + path_ + ": " + errno_text());
    }
    return static_cast<uint64_t>(end);
}

void File::read_at(uint64_t offset, void* buffer, size_t len) const {
    if (!is_open()) {
        throw std::runtime_error("File: read on closed file " + path_);
    }
    if (PG_LSEEK(fd_, static_cast<int64_t>(offset), SEEK_SET) < 0) {
        throw std::runtime_error("File: seek failed on " + path_ + ": " + errno_text());
    }

    auto* out = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    while (done < len) {
        auto n = PG_READ(fd_, out + done, static_cast<unsigned>(len - done));
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("File: read failed on " + path_ + ": " + errno_text());
        }
        if (n == 0) {
            throw std::runtime_error("File: short read on " + path_ + " (wanted " +
                                     std::to_string(len) + ", got " + std::to_string(done) + ")");
        }
        done += static_cast<size_t>(n);
    }
}

void File::write_at(uint64_t offset, const void* buffer, size_t len) {
    if (!is_open()) {
        throw std::runtime_error("File: write on closed file " + path_);
    }
    if (PG_LSEEK(fd_, static_cast<int64_t>(offset), SEEK_SET) < 0) {
        throw std::runtime_error("File: seek failed on " + path_ + ": " + errno_text());
    }

    const auto* in = static_cast<const uint8_t*>(buffer);
    size_t done = 0;
    while (done < len) {
        auto n = PG_WRITE(fd_, in + done, static_cast<unsigned>(len - done));
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            throw std::runtime_error("File: write failed on " + path_ + ": " + errno_text());
        }
        done += static_cast<size_t>(n);
    }
}

void File::sync() {
    if (!is_open()) {
        return;
    }
    if (PG_SYNC(fd_) != 0) {
        throw std::runtime_error("File: sync failed on " + path_ + ": " + errno_text());
    }
}

void File::close() {
    if (fd_ >= 0) {
        PG_CLOSE(fd_);
        fd_ = -1;
    }
}

} // namespace pg
