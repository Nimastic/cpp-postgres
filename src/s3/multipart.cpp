#include "s3/multipart.h"
#include "s3/crc64.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <stdexcept>

namespace s3 {

static std::string to_hex_string(uint64_t val) {
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << val;
    return oss.str();
}

std::string MultipartManager::initiate_upload(const std::string& bucket, const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t seq = next_upload_seq_++;
    std::string upload_id = "mp-" + bucket + "-" + std::to_string(seq);

    MultipartUploadSession session;
    session.upload_id = upload_id;
    session.bucket = bucket;
    session.key = key;
    session.initiated_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    sessions_[upload_id] = std::move(session);
    return upload_id;
}

PartInfo MultipartManager::upload_part(const std::string& upload_id, uint32_t part_number,
                                       const void* data, size_t len, ChunkStore& store) {
    if (part_number < 1 || part_number > 10000) {
        throw std::invalid_argument("Part number must be between 1 and 10000");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(upload_id);
    if (it == sessions_.end()) {
        throw std::runtime_error("Multipart upload not found: " + upload_id);
    }

    PartInfo info;
    info.part_number = part_number;
    info.size = len;
    info.crc64 = crc64_nvme(data, len);
    info.etag = "\"" + to_hex_string(info.crc64) + "\"";

    // Slice part data into chunks
    const char* bytes = static_cast<const char*>(data);
    size_t chunk_cap = store.chunk_payload_capacity();
    size_t offset = 0;

    if (len == 0) {
        // Special case: 0-length part
        chunk_id_t cid = store.append_chunk(nullptr, 0);
        info.extents.push_back(Extent{
            .chunk_id = cid,
            .offset_in_chunk = 0,
            .length = 0
        });
    } else {
        while (offset < len) {
            size_t slice = std::min(chunk_cap, len - offset);
            chunk_id_t cid = store.append_chunk(bytes + offset, slice);
            info.extents.push_back(Extent{
                .chunk_id = cid,
                .offset_in_chunk = 0,
                .length = static_cast<uint32_t>(slice)
            });
            offset += slice;
        }
    }

    it->second.parts[part_number] = info;
    return info;
}

ObjectMetadata MultipartManager::complete_upload(const std::string& upload_id,
                                                 const std::vector<PartInfo>& expected_parts,
                                                 ExtentIndex& index) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(upload_id);
    if (it == sessions_.end()) {
        throw std::runtime_error("Multipart upload not found: " + upload_id);
    }

    auto& session = it->second;
    std::vector<PartInfo> parts_to_assemble;

    if (!expected_parts.empty()) {
        // Strict verification of provided part list
        uint32_t last_part_num = 0;
        for (const auto& ep : expected_parts) {
            if (ep.part_number <= last_part_num) {
                throw std::invalid_argument("Parts must be specified in ascending order");
            }
            auto pit = session.parts.find(ep.part_number);
            if (pit == session.parts.end()) {
                throw std::runtime_error("Missing part number: " + std::to_string(ep.part_number));
            }
            if (!ep.etag.empty() && ep.etag != pit->second.etag) {
                throw std::runtime_error("ETag mismatch for part: " + std::to_string(ep.part_number));
            }
            parts_to_assemble.push_back(pit->second);
            last_part_num = ep.part_number;
        }
    } else {
        // Auto-assemble all uploaded parts sorted by part_number
        std::vector<uint32_t> part_nums;
        for (const auto& [num, _] : session.parts) {
            part_nums.push_back(num);
        }
        std::sort(part_nums.begin(), part_nums.end());
        for (uint32_t num : part_nums) {
            parts_to_assemble.push_back(session.parts[num]);
        }
    }

    if (parts_to_assemble.empty()) {
        throw std::runtime_error("Cannot complete empty multipart upload");
    }

    ObjectMetadata meta;
    meta.bucket = session.bucket;
    meta.key = session.key;
    meta.created_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    meta.is_tombstone = false;

    uint64_t composite_crc = 0;
    for (const auto& p : parts_to_assemble) {
        meta.size += p.size;
        for (const auto& ext : p.extents) {
            meta.extents.push_back(ext);
        }
        composite_crc ^= p.crc64;
    }

    // Standard AWS multipart ETag format: "<composite_hash>-<num_parts>"
    meta.etag = "\"" + to_hex_string(composite_crc) + "-" + std::to_string(parts_to_assemble.size()) + "\"";

    index.put_object(meta);
    sessions_.erase(it);
    return meta;
}

bool MultipartManager::abort_upload(const std::string& upload_id, ChunkStore& store) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(upload_id);
    if (it == sessions_.end()) {
        return false;
    }

    for (const auto& [part_num, part] : it->second.parts) {
        for (const auto& ext : part.extents) {
            store.mark_tombstone(ext.chunk_id);
        }
    }

    sessions_.erase(it);
    return true;
}

std::vector<PartInfo> MultipartManager::list_parts(const std::string& upload_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(upload_id);
    if (it == sessions_.end()) {
        return {};
    }

    std::vector<PartInfo> parts;
    for (const auto& [num, part] : it->second.parts) {
        parts.push_back(part);
    }

    std::sort(parts.begin(), parts.end(), [](const PartInfo& a, const PartInfo& b) {
        return a.part_number < b.part_number;
    });

    return parts;
}

bool MultipartManager::has_upload(const std::string& upload_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.find(upload_id) != sessions_.end();
}

size_t MultipartManager::active_uploads_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

} // namespace s3
