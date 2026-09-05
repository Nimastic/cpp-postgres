#pragma once

#include "s3/types.h"
#include "s3/chunk_store.h"
#include "s3/extent_index.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <optional>
#include <atomic>

namespace s3 {

struct PartInfo {
    uint32_t part_number{0};
    uint64_t size{0};
    std::string etag;
    uint64_t crc64{0};
    std::vector<Extent> extents;
};

struct MultipartUploadSession {
    std::string upload_id;
    std::string bucket;
    std::string key;
    int64_t initiated_at_ms{0};
    std::unordered_map<uint32_t, PartInfo> parts;
};

class MultipartManager {
public:
    MultipartManager() = default;
    ~MultipartManager() = default;

    // Initiate a multipart upload session, returns unique upload_id
    std::string initiate_upload(const std::string& bucket, const std::string& key);

    // Upload a part: slices data into ChunkStore, registers part extents and calculates CRC64
    PartInfo upload_part(const std::string& upload_id, uint32_t part_number,
                         const void* data, size_t len, ChunkStore& store);

    // Complete upload: validates parts in ascending order, merges extents, registers with ExtentIndex
    ObjectMetadata complete_upload(const std::string& upload_id,
                                   const std::vector<PartInfo>& expected_parts,
                                   ExtentIndex& index);

    // Abort upload: frees chunks by tombstoning them in ChunkStore, cleans up session
    bool abort_upload(const std::string& upload_id, ChunkStore& store);

    // List parts currently uploaded for an active session
    std::vector<PartInfo> list_parts(const std::string& upload_id) const;

    // Check if an upload session exists
    bool has_upload(const std::string& upload_id) const;

    // Number of active in-flight uploads
    size_t active_uploads_count() const;

private:
    mutable std::mutex mutex_;
    std::atomic<uint64_t> next_upload_seq_{1};
    std::unordered_map<std::string, MultipartUploadSession> sessions_;
};

} // namespace s3
