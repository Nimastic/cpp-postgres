#include "s3/extent_index.h"
#include <algorithm>

namespace s3 {

void ExtentIndex::put_object(const ObjectMetadata& meta) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    std::string ikey = make_index_key(meta.bucket, meta.key);
    uint64_t vid = meta.version_id;
    if (vid == 0) {
        // Auto-assign version ID monotonically if not provided
        auto& versions = objects_[ikey];
        vid = versions.empty() ? 1 : versions.rbegin()->first + 1;
    }
    ObjectMetadata copy = meta;
    copy.version_id = vid;
    if (copy.created_at_ms == 0) {
        copy.created_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    objects_[ikey][vid] = std::move(copy);
}

std::optional<ObjectMetadata> ExtentIndex::get_object(const std::string& bucket, const std::string& key, uint64_t version_id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::string ikey = make_index_key(bucket, key);
    auto it = objects_.find(ikey);
    if (it == objects_.end() || it->second.empty()) {
        return std::nullopt;
    }

    const auto& versions = it->second;
    if (version_id == 0) {
        // Return latest version
        const auto& latest = versions.rbegin()->second;
        if (latest.is_tombstone) {
            return std::nullopt;
        }
        return latest;
    }

    auto vit = versions.find(version_id);
    if (vit == versions.end() || vit->second.is_tombstone) {
        return std::nullopt;
    }
    return vit->second;
}

bool ExtentIndex::delete_object(const std::string& bucket, const std::string& key, bool soft_delete) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    std::string ikey = make_index_key(bucket, key);
    auto it = objects_.find(ikey);
    if (it == objects_.end() || it->second.empty()) {
        return false;
    }

    if (soft_delete) {
        // Append tombstone marker as new version
        auto& versions = it->second;
        uint64_t next_vid = versions.rbegin()->first + 1;
        ObjectMetadata tomb;
        tomb.bucket = bucket;
        tomb.key = key;
        tomb.version_id = next_vid;
        tomb.is_tombstone = true;
        tomb.created_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        versions[next_vid] = std::move(tomb);
        return true;
    } else {
        objects_.erase(it);
        return true;
    }
}

std::vector<ObjectMetadata> ExtentIndex::list_objects(const std::string& bucket, const std::string& prefix) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<ObjectMetadata> result;
    std::string bprefix = bucket + "/" + prefix;

    for (const auto& [ikey, versions] : objects_) {
        if (versions.empty()) continue;
        if (ikey.rfind(bprefix, 0) == 0) {
            const auto& latest = versions.rbegin()->second;
            if (!latest.is_tombstone) {
                result.push_back(latest);
            }
        }
    }

    std::sort(result.begin(), result.end(), [](const ObjectMetadata& a, const ObjectMetadata& b) {
        return a.key < b.key;
    });

    return result;
}

std::vector<ResolvedSlice> ExtentIndex::resolve_range(const std::string& bucket, const std::string& key, uint64_t byte_offset, uint64_t byte_len) const {
    auto obj = get_object(bucket, key);
    if (!obj.has_value()) {
        return {};
    }

    if (byte_offset >= obj->size || byte_len == 0) {
        return {};
    }

    uint64_t end_offset = std::min<uint64_t>(byte_offset + byte_len, obj->size);
    std::vector<ResolvedSlice> slices;

    uint64_t cur_obj_offset = 0;
    for (const auto& ext : obj->extents) {
        uint64_t ext_start = cur_obj_offset;
        uint64_t ext_end = cur_obj_offset + ext.length;

        // Check if query range intersects with this extent
        if (end_offset > ext_start && byte_offset < ext_end) {
            uint64_t slice_start = std::max(byte_offset, ext_start);
            uint64_t slice_end = std::min(end_offset, ext_end);
            uint32_t offset_in_extent = static_cast<uint32_t>(slice_start - ext_start);
            uint32_t slice_len = static_cast<uint32_t>(slice_end - slice_start);

            slices.push_back(ResolvedSlice{
                .chunk_id = ext.chunk_id,
                .offset_in_chunk = ext.offset_in_chunk + offset_in_extent,
                .length = slice_len,
                .object_offset = slice_start
            });
        }

        cur_obj_offset += ext.length;
    }

    return slices;
}

std::set<chunk_id_t> ExtentIndex::get_referenced_chunks() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::set<chunk_id_t> referenced;
    for (const auto& [ikey, versions] : objects_) {
        for (const auto& [vid, meta] : versions) {
            if (!meta.is_tombstone) {
                for (const auto& ext : meta.extents) {
                    referenced.insert(ext.chunk_id);
                }
            }
        }
    }
    return referenced;
}

size_t ExtentIndex::size() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    size_t count = 0;
    for (const auto& [ikey, versions] : objects_) {
        if (!versions.empty() && !versions.rbegin()->second.is_tombstone) {
            count++;
        }
    }
    return count;
}

} // namespace s3
