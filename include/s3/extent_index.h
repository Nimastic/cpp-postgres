#pragma once

#include "s3/types.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <optional>
#include <shared_mutex>
#include <mutex>
#include <set>

namespace s3 {

struct ResolvedSlice {
    chunk_id_t chunk_id;
    uint32_t offset_in_chunk;
    uint32_t length;
    uint64_t object_offset;
};

class ExtentIndex {
public:
    ExtentIndex() = default;
    ~ExtentIndex() = default;

    // Register or overwrite object metadata
    void put_object(const ObjectMetadata& meta);

    // Retrieve active object metadata (or specific version if version_id != 0)
    std::optional<ObjectMetadata> get_object(const std::string& bucket, const std::string& key, uint64_t version_id = 0) const;

    // Delete object (inserts tombstone version if versioning enabled, or deletes)
    bool delete_object(const std::string& bucket, const std::string& key, bool soft_delete = false);

    // List objects in a bucket with optional prefix
    std::vector<ObjectMetadata> list_objects(const std::string& bucket, const std::string& prefix = "") const;

    // Range query translation: maps [byte_offset, byte_offset + byte_len) to physical chunk slices
    std::vector<ResolvedSlice> resolve_range(const std::string& bucket, const std::string& key, uint64_t byte_offset, uint64_t byte_len) const;

    // Return set of all chunk IDs currently referenced by active objects
    std::set<chunk_id_t> get_referenced_chunks() const;

    // Count of total active objects
    size_t size() const;

private:
    // Key format: bucket + "/" + key
    static std::string make_index_key(const std::string& bucket, const std::string& key) {
        return bucket + "/" + key;
    }

    mutable std::shared_mutex mutex_;
    // Maps "bucket/key" -> sorted map of version_id -> ObjectMetadata
    std::unordered_map<std::string, std::map<uint64_t, ObjectMetadata>> objects_;
};

} // namespace s3
