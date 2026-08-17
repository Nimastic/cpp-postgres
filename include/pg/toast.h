#pragma once

#include "pg/constants.h"
#include "pg/pager.h"
#include "pg/page.h"
#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <map>
#include <stdexcept>
#include <cstring>

namespace pg {

// TOAST Thresholds (PostgreSQL convention: 1/4 page size)
constexpr size_t TOAST_TUPLE_THRESHOLD = 2048; // Max inline payload size (2KB)
constexpr size_t TOAST_CHUNK_SIZE      = 2048; // Size of each individual chunk

enum class ToastStorageType : uint8_t {
    INLINE = 0,     // Small payload stored directly in tuple
    OUT_OF_LINE = 1 // Oversized payload split into 2KB chunks in TOAST table
};

#pragma pack(push, 1)
// 18-byte pointer stored in main heap tuple when attribute is TOASTed
struct ToastPointer {
    uint64_t toast_id{0};       // Unique 64-bit ID identifying this toasted value
    uint32_t raw_size{0};       // Full uncompressed byte size (e.g. 20,480 bytes)
    uint32_t chunk_count{0};    // Total number of 2KB chunks in TOAST table
    uint16_t flags{0};          // Storage flags
};
#pragma pack(pop)

static_assert(sizeof(ToastPointer) == 18, "ToastPointer must be exactly 18 bytes");

// An individual 2KB chunk in the auxiliary TOAST table
struct ToastChunk {
    uint64_t toast_id{0};
    uint32_t chunk_seq{0};
    std::vector<uint8_t> data;
};

// Represents a flexible attribute value (either inline bytes or a ToastPointer)
struct ToastValue {
    ToastStorageType type{ToastStorageType::INLINE};
    std::vector<uint8_t> inline_data; // Populated when type == INLINE
    ToastPointer pointer{};           // Populated when type == OUT_OF_LINE

    bool is_inline() const { return type == ToastStorageType::INLINE; }
    size_t size() const {
        return is_inline() ? inline_data.size() : pointer.raw_size;
    }
};

// Auxiliary TOAST Table Manager
// Manages:
// 1. Automatic inline vs out-of-line thresholding (<=2KB vs >2KB)
// 2. Slicing oversized payloads into 2KB chunks with (toast_id, chunk_seq)
// 3. Storing chunks across dedicated 8KB TOAST pages
// 4. Transparent multi-chunk reassembly upon read
class ToastManager {
public:
    explicit ToastManager(std::unique_ptr<Pager> toast_pager);
    ~ToastManager() = default;

    // Factory method
    static std::unique_ptr<ToastManager> open(const std::string& toast_filepath);

    // Ingests an arbitrary payload:
    // If len <= 2048: returns inline ToastValue directly.
    // If len > 2048: slices into 2KB chunks, writes to TOAST table, and returns ToastPointer.
    ToastValue store(const void* data, size_t len);
    ToastValue store_string(const std::string& text);

    // Reassembles the full original payload:
    // If inline: returns inline bytes.
    // If out-of-line: fetches all chunks (0..N-1), glues them together, and returns full buffer.
    std::vector<uint8_t> fetch(const ToastValue& val);
    std::string fetch_string(const ToastValue& val);

    // Delete chunks for a toasted value
    bool delete_value(uint64_t toast_id);

    // TOAST table metrics
    size_t total_chunks() const { return chunk_index_.size(); }
    size_t num_pages() const { return pager_->num_pages(); }

private:
    std::unique_ptr<Pager> pager_;
    uint64_t next_toast_id_{1};

    // Fast memory mapping: (toast_id, chunk_seq) -> chunk data
    // In disk persistence, chunks are packed into 8KB TOAST pages
    std::map<std::pair<uint64_t, uint32_t>, std::vector<uint8_t>> chunk_index_;

    void flush_chunk_to_page(uint64_t toast_id, uint32_t chunk_seq, const void* data, size_t len);
};

} // namespace pg
