#pragma once

#include "pg/constants.h"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <optional>
#include <string>
#include <sstream>

namespace pg {

enum class ItemFlags : uint8_t {
    UNUSED = 0,   // Slot is empty / unused
    NORMAL = 1,   // Points to live tuple on this page
    REDIRECT = 2, // HOT redirect to another slot
    DEAD = 3      // Marked dead by vacuum
};

#pragma pack(push, 1)
// 4-byte line pointer matching PostgreSQL ItemIdData layout
struct LinePointer {
    uint16_t lp_offset{0}; // Byte offset from start of page where tuple data begins
    uint16_t lp_len_flags{0}; // Lower 14 bits: tuple length; Upper 2 bits: ItemFlags

    ItemFlags flags() const {
        return static_cast<ItemFlags>((lp_len_flags >> 14) & 0x03);
    }

    uint16_t length() const {
        return lp_len_flags & 0x3FFF;
    }

    void set(uint16_t offset, uint16_t len, ItemFlags f) {
        lp_offset = offset;
        lp_len_flags = (len & 0x3FFF) | (static_cast<uint16_t>(f) << 14);
    }
};

// 18-byte page header matching PostgreSQL PageHeaderData
struct PageHeaderData {
    uint64_t pd_lsn{0};               // Log Sequence Number for WAL
    uint16_t pd_checksum{0};          // Page checksum
    uint16_t pd_flags{0};             // Status flags
    uint16_t pd_lower{0};             // Byte offset to start of free space (end of line pointers)
    uint16_t pd_upper{0};             // Byte offset to end of free space (start of youngest tuple)
    uint16_t pd_special{static_cast<uint16_t>(PAGE_SIZE)}; // Byte offset to special space
};
#pragma pack(pop)

static_assert(sizeof(LinePointer) == 4, "LinePointer must be exactly 4 bytes");
static_assert(sizeof(PageHeaderData) == 18, "PageHeaderData must be exactly 18 bytes");

class Page {
public:
    Page();
    explicit Page(const void* raw_data);

    // Initialize memory as a fresh blank slotted page
    void init();

    // Header access
    const PageHeaderData& header() const;
    PageHeaderData& header();

    // Slot & Line Pointer operations (1-based indexing: slot 1, 2, ...)
    size_t num_slots() const;
    std::optional<LinePointer> get_line_pointer(slot_id_t slot_id) const;
    bool set_line_pointer(slot_id_t slot_id, const LinePointer& lp);

    // Available free space (between pd_lower + sizeof(LinePointer) and pd_upper)
    size_t free_space() const;

    // Insert tuple byte payload; returns 1-based slot_id, or INVALID_SLOT_ID if full
    slot_id_t insert_tuple(const void* data, size_t len);

    // Get pointer to tuple bytes for a valid 1-based slot_id
    const uint8_t* get_tuple_ptr(slot_id_t slot_id, size_t* out_len = nullptr) const;
    std::optional<std::vector<uint8_t>> get_tuple(slot_id_t slot_id) const;

    // Direct access to underlying 8KB buffer
    const uint8_t* data() const { return data_; }
    uint8_t* data() { return data_; }

    // Visual page dump representation
    std::string dump() const;

private:
    uint8_t data_[PAGE_SIZE];

    LinePointer* line_pointers_internal();
    const LinePointer* line_pointers_internal() const;
};

} // namespace pg
