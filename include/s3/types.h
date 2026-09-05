#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <chrono>

namespace s3 {

constexpr uint32_t S3_CHUNK_MAGIC = 0x53334348; // "S3CH"
constexpr size_t DEFAULT_CHUNK_SIZE = 4 * 1024 * 1024; // 4 MB

enum ChunkFlags : uint16_t {
    CHUNK_NORMAL    = 0x0001,
    CHUNK_TOMBSTONE = 0x0002,
    CHUNK_CORRUPTED = 0x0004
};

#pragma pack(push, 1)
struct ChunkHeader {
    uint32_t magic{S3_CHUNK_MAGIC};
    uint16_t version{1};
    uint16_t flags{CHUNK_NORMAL};
    uint64_t chunk_id{0};
    uint32_t payload_len{0};
    uint64_t crc64{0};
    uint32_t reserved{0};
};
#pragma pack(pop)

static_assert(sizeof(ChunkHeader) == 32, "ChunkHeader must be exactly 32 bytes");

using chunk_id_t = uint64_t;

struct Extent {
    chunk_id_t chunk_id{0};
    uint32_t offset_in_chunk{0};
    uint32_t length{0};
};

struct ObjectMetadata {
    std::string bucket;
    std::string key;
    uint64_t size{0};
    std::string etag;
    uint64_t version_id{1};
    int64_t created_at_ms{0};
    bool is_tombstone{false};
    std::vector<Extent> extents;
};

} // namespace s3
