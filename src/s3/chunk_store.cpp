#include "s3/chunk_store.h"
#include <iostream>
#include <filesystem>
#include <vector>
#include <algorithm>

namespace fs = std::filesystem;

namespace s3 {

std::unique_ptr<ChunkStore> ChunkStore::open(const std::string& path, size_t chunk_size) {
    return std::unique_ptr<ChunkStore>(new ChunkStore(path, chunk_size));
}

ChunkStore::ChunkStore(std::string path, size_t chunk_payload_size)
    : path_(std::move(path)), chunk_payload_size_(chunk_payload_size) {
    if (!fs::exists(path_)) {
        std::ofstream create_file(path_, std::ios::binary | std::ios::out);
        create_file.close();
    }

    file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_.is_open()) {
        throw std::runtime_error("Failed to open S3 ChunkStore at: " + path_);
    }

    file_.seekg(0, std::ios::end);
    size_t file_len = static_cast<size_t>(file_.tellg());
    num_chunks_ = file_len / slot_size();
}

ChunkStore::~ChunkStore() {
    close();
}

void ChunkStore::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open()) {
        file_.flush();
    }
}

void ChunkStore::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

chunk_id_t ChunkStore::append_chunk(const void* data, size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    chunk_id_t id = num_chunks_;
    uint64_t crc = crc64_nvme(data, len);

    if (len > chunk_payload_size_) {
        throw std::runtime_error("Chunk payload exceeds slot capacity");
    }

    ChunkHeader hdr;
    hdr.magic = S3_CHUNK_MAGIC;
    hdr.version = 1;
    hdr.flags = CHUNK_NORMAL;
    hdr.chunk_id = id;
    hdr.payload_len = static_cast<uint32_t>(len);
    hdr.crc64 = crc;

    uint64_t offset = get_slot_offset(id);
    file_.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    file_.write(reinterpret_cast<const char*>(&hdr), sizeof(ChunkHeader));
    if (len > 0) {
        file_.write(static_cast<const char*>(data), static_cast<std::streamsize>(len));
    }

    // Zero-fill the remainder of the slot for clean disk layout
    size_t remainder = chunk_payload_size_ - len;
    if (remainder > 0) {
        std::vector<char> zeros(std::min<size_t>(remainder, 64 * 1024), 0);
        size_t written = 0;
        while (written < remainder) {
            size_t batch = std::min(zeros.size(), remainder - written);
            file_.write(zeros.data(), static_cast<std::streamsize>(batch));
            written += batch;
        }
    }

    file_.flush();
    num_chunks_++;
    return id;
}

uint64_t ChunkStore::write_chunk(chunk_id_t id, const void* data, size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (len > chunk_payload_size_) {
        throw std::runtime_error("Chunk payload exceeds slot capacity");
    }

    uint64_t crc = crc64_nvme(data, len);

    ChunkHeader hdr;
    hdr.magic = S3_CHUNK_MAGIC;
    hdr.version = 1;
    hdr.flags = CHUNK_NORMAL;
    hdr.chunk_id = id;
    hdr.payload_len = static_cast<uint32_t>(len);
    hdr.crc64 = crc;

    uint64_t offset = get_slot_offset(id);
    file_.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    file_.write(reinterpret_cast<const char*>(&hdr), sizeof(ChunkHeader));
    if (len > 0) {
        file_.write(static_cast<const char*>(data), static_cast<std::streamsize>(len));
    }

    file_.flush();
    if (id >= num_chunks_) {
        num_chunks_ = id + 1;
    }
    return crc;
}

size_t ChunkStore::read_chunk(chunk_id_t id, void* buffer, size_t max_len, ChunkHeader* out_hdr) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (id >= num_chunks_) {
        throw std::out_of_range("Chunk ID out of range");
    }

    ChunkHeader hdr;
    uint64_t offset = get_slot_offset(id);
    file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    file_.read(reinterpret_cast<char*>(&hdr), sizeof(ChunkHeader));

    if (hdr.magic != S3_CHUNK_MAGIC) {
        throw std::runtime_error("Corrupted chunk header magic");
    }

    if (out_hdr) {
        *out_hdr = hdr;
    }

    size_t to_read = std::min<size_t>(max_len, hdr.payload_len);
    if (to_read > 0) {
        file_.read(static_cast<char*>(buffer), static_cast<std::streamsize>(to_read));
    }

    return to_read;
}

bool ChunkStore::read_header(chunk_id_t id, ChunkHeader& out_hdr) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (id >= num_chunks_) return false;

    uint64_t offset = get_slot_offset(id);
    file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    file_.read(reinterpret_cast<char*>(&out_hdr), sizeof(ChunkHeader));
    return out_hdr.magic == S3_CHUNK_MAGIC;
}

bool ChunkStore::verify_chunk(chunk_id_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (id >= num_chunks_) return false;

    ChunkHeader hdr;
    uint64_t offset = get_slot_offset(id);
    file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    file_.read(reinterpret_cast<char*>(&hdr), sizeof(ChunkHeader));

    if (hdr.magic != S3_CHUNK_MAGIC) return false;
    if (hdr.flags & CHUNK_TOMBSTONE) return true; // Tombstone chunks are considered verified

    std::vector<uint8_t> payload(hdr.payload_len);
    if (hdr.payload_len > 0) {
        file_.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(hdr.payload_len));
    }

    uint64_t actual_crc = crc64_nvme(payload.data(), hdr.payload_len);
    return actual_crc == hdr.crc64;
}

bool ChunkStore::mark_tombstone(chunk_id_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (id >= num_chunks_) return false;

    ChunkHeader hdr;
    uint64_t offset = get_slot_offset(id);
    file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    file_.read(reinterpret_cast<char*>(&hdr), sizeof(ChunkHeader));

    if (hdr.magic != S3_CHUNK_MAGIC) return false;

    hdr.flags |= CHUNK_TOMBSTONE;
    file_.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    file_.write(reinterpret_cast<const char*>(&hdr), sizeof(ChunkHeader));
    file_.flush();
    return true;
}

size_t ChunkStore::scrub(std::function<void(chunk_id_t id, bool ok)> callback) {
    size_t corrupted = 0;
    for (chunk_id_t i = 0; i < num_chunks_; ++i) {
        bool ok = verify_chunk(i);
        if (!ok) corrupted++;
        if (callback) {
            callback(i, ok);
        }
    }
    return corrupted;
}

bool ChunkStore::corrupt_chunk_payload(chunk_id_t id, size_t byte_offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (id >= num_chunks_) return false;

    uint64_t offset = get_slot_offset(id) + sizeof(ChunkHeader) + byte_offset;
    file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    char b = 0;
    file_.read(&b, 1);
    b ^= 0xFF; // Flip all bits in the byte to guarantee CRC mismatch

    file_.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    file_.write(&b, 1);
    file_.flush();
    return true;
}

} // namespace s3
