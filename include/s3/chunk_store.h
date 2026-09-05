#pragma once

#include "s3/types.h"
#include "s3/crc64.h"
#include <string>
#include <memory>
#include <fstream>
#include <mutex>
#include <functional>
#include <stdexcept>

namespace s3 {

class ChunkStore {
public:
    static std::unique_ptr<ChunkStore> open(const std::string& path, size_t chunk_size = DEFAULT_CHUNK_SIZE);
    ~ChunkStore();

    // Append a new chunk to the store, returning assigned chunk_id
    chunk_id_t append_chunk(const void* data, size_t len);

    // Overwrite or write at specific chunk_id
    uint64_t write_chunk(chunk_id_t id, const void* data, size_t len);

    // Read payload of chunk_id into user buffer
    size_t read_chunk(chunk_id_t id, void* buffer, size_t max_len, ChunkHeader* out_hdr = nullptr);

    // Read full chunk header
    bool read_header(chunk_id_t id, ChunkHeader& out_hdr);

    // Check header magic and verify CRC64-NVME over stored payload
    bool verify_chunk(chunk_id_t id);

    // Mark chunk as tombstone (soft delete)
    bool mark_tombstone(chunk_id_t id);

    // Background bit-rot scrubber: verifies all non-tombstone chunks
    // Invokes callback(chunk_id, is_ok) for each chunk. Returns number of corrupted chunks.
    size_t scrub(std::function<void(chunk_id_t id, bool ok)> callback = nullptr);

    // Adversarial testing tool: deliberately flip a byte in chunk payload to verify bit-rot detection
    bool corrupt_chunk_payload(chunk_id_t id, size_t byte_offset = 0);

    size_t num_chunks() const { return num_chunks_; }
    size_t chunk_payload_capacity() const { return chunk_payload_size_; }
    size_t slot_size() const { return sizeof(ChunkHeader) + chunk_payload_size_; }

    void flush();
    void close();

private:
    ChunkStore(std::string path, size_t chunk_payload_size);

    std::string path_;
    size_t chunk_payload_size_;
    size_t num_chunks_{0};
    mutable std::mutex mutex_;
    std::fstream file_;

    uint64_t get_slot_offset(chunk_id_t id) const {
        return id * slot_size();
    }
};

} // namespace s3
