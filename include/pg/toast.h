#pragma once

#include "pg/constants.h"
#include "pg/pager.h"
#include "pg/page.h"
#include "pg/tuple.h"
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

class WALManager;

#pragma pack(push, 1)
// 16-byte binary header stored before each chunk tuple on a TOAST slotted page
struct ToastChunkHeader {
    uint64_t toast_id{0};       // ID of the toasted attribute
    uint32_t chunk_seq{0};      // Chunk sequence number (0 .. chunk_count - 1)
    uint32_t data_len{0};       // Length of following chunk data bytes (<= 2048)
};
#pragma pack(pop)

static_assert(sizeof(ToastChunkHeader) == 16, "ToastChunkHeader must be exactly 16 bytes");

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
// 3. Storing chunks across dedicated 8KB TOAST pages with ToastChunkHeader
// 4. Transparent multi-chunk reassembly upon read
// 5. WAL logging for all chunk insertions
// 6. Cross-restart chunk index recovery from disk
class ToastManager {
public:
    explicit ToastManager(std::unique_ptr<Pager> toast_pager, WALManager* wal = nullptr);
    ~ToastManager() = default;

    // Factory method
    static std::unique_ptr<ToastManager> open(const std::string& toast_filepath, WALManager* wal = nullptr);

    // Ingests an arbitrary payload with optional transaction ID for WAL logging:
    // If len <= 2048: returns inline ToastValue directly.
    // If len > 2048: slices into 2KB chunks, writes to TOAST table, and returns ToastPointer.
    ToastValue store(const void* data, size_t len, tx_id_t tx_id = 0);
    ToastValue store_string(const std::string& text, tx_id_t tx_id = 0);

    // Reassembles the full original payload:
    // If inline: returns inline bytes.
    // If out-of-line: fetches all chunks (0..N-1), glues them together, and returns full buffer.
    std::vector<uint8_t> fetch(const ToastValue& val);
    std::string fetch_string(const ToastValue& val);

    // Delete chunks for a toasted value
    bool delete_value(uint64_t toast_id);

    // Replay chunk insertion during WAL crash recovery
    void replay_insert(uint64_t toast_id, uint32_t chunk_seq, page_id_t page_id, slot_id_t slot_id, const void* data, size_t len);

    // Wire WAL manager for crash-durable logging
    void set_wal(WALManager* wal) { wal_ = wal; }
    WALManager* wal() const { return wal_; }

    // Flush dirty pages and sync underlying pager file
    void flush();

    // TOAST table metrics
    size_t total_chunks() const { return chunk_index_.size(); }
    size_t num_pages() const { return pager_->num_pages(); }
    Pager& pager() { return *pager_; }

private:
    std::unique_ptr<Pager> pager_;
    WALManager* wal_{nullptr};
    uint64_t next_toast_id_{1};

    // Fast memory mapping: (toast_id, chunk_seq) -> chunk data
    // Restored on startup from slotted pages on disk
    std::map<std::pair<uint64_t, uint32_t>, std::vector<uint8_t>> chunk_index_;

    void scan_existing_pages();
    void flush_chunk_to_page(uint64_t toast_id, uint32_t chunk_seq, const void* data, size_t len, tx_id_t tx_id);
};

} // namespace pg
