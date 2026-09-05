#pragma once

#include "pg/constants.h"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <optional>
#include <string>
#include <sstream>
#include <cstring>

namespace pg {

enum class ItemFlags : uint8_t {
    UNUSED = 0,   // Slot is empty / unused
    NORMAL = 1,   // Points to live tuple on this page
    REDIRECT = 2, // HOT redirect to another slot
    DEAD = 3      // Marked dead by vacuum
};

#pragma pack(push, 1)
// 4-byte line pointer. Same size and same four states as PostgreSQL ItemIdData,
// but a different bit split: PostgreSQL uses lp_off:15, lp_flags:2, lp_len:15.
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

// 18-byte page header. Simplified PostgreSQL PageHeaderData: the six fields below
// match in order and width, but PostgreSQL is 24 bytes and also carries
// pd_pagesize_version (2B) and pd_prune_xid (4B).
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

// A view over 8192 bytes of page memory that this object does not own.
//
// Making Page a view is what lets a caller work directly inside a pinned buffer
// frame, the way PostgreSQL's BufferGetPage() hands back a pointer into shared
// buffers. The previous owning design forced a copy out of the frame and a copy
// back in, which meant no page stayed pinned across a read-modify-write and
// concurrent writers silently clobbered each other.
class Page {
public:
    explicit Page(void* raw_data) : data_(static_cast<uint8_t*>(raw_data)) {}

    // Initialize this memory as a fresh blank slotted page
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

    // Insert tuple byte payload; returns 1-based slot_id, or INVALID_SLOT_ID if full.
    // Only UNUSED line pointers are recycled. A DEAD pointer may still be
    // referenced by an index entry, so reusing it would let an index lookup
    // resolve to an unrelated row; DEAD becomes UNUSED only after VACUUM has
    // cleaned the indexes (see Vacuum::run).
    slot_id_t insert_tuple(const void* data, size_t len);

    // Place a tuple at one specific slot, extending the line pointer array if
    // needed. WAL redo must reproduce the exact physical layout that was logged,
    // so it cannot let the page choose a slot for it.
    bool insert_tuple_at(slot_id_t slot_id, const void* data, size_t len);

    // Get pointer to tuple bytes for a valid 1-based slot_id
    const uint8_t* get_tuple_ptr(slot_id_t slot_id, size_t* out_len = nullptr) const;
    uint8_t* get_tuple_ptr_mut(slot_id_t slot_id, size_t* out_len = nullptr);
    std::optional<std::vector<uint8_t>> get_tuple(slot_id_t slot_id) const;

    // Defragment page: shifts all surviving live (NORMAL) tuples towards page end,
    // compacts dead space, updates line pointer offsets, and expands pd_upper.
    // Never renumbers slots: CTIDs held by indexes must stay valid.
    void defragment();

    // WAL bookkeeping. PostgreSQL calls this PageSetLSN, under the buffer's
    // exclusive content lock, right after emitting the record for the change.
    void set_lsn(uint64_t lsn) { header().pd_lsn = lsn; }
    uint64_t lsn() const { return header().pd_lsn; }

    // Direct access to the underlying 8KB buffer
    const uint8_t* data() const { return data_; }
    uint8_t* data() { return data_; }

    // Visual page dump representation
    std::string dump() const;

private:
    uint8_t* data_;

    LinePointer* line_pointers_internal();
    const LinePointer* line_pointers_internal() const;
};

// Owns 8192 bytes of page memory and hands out a Page view over it. Used where a
// page genuinely needs its own storage: scratch space during recovery, a freshly
// formatted page before it is handed to the buffer pool, test fixtures.
class PageBuffer {
public:
    PageBuffer() { view_.init(); }

    PageBuffer(const PageBuffer& other) { std::memcpy(data_, other.data_, PAGE_SIZE); }
    PageBuffer& operator=(const PageBuffer& other) {
        if (this != &other) std::memcpy(data_, other.data_, PAGE_SIZE);
        return *this;
    }

    Page& operator*() { return view_; }
    Page* operator->() { return &view_; }
    const Page& operator*() const { return view_; }
    const Page* operator->() const { return &view_; }

    Page view() { return Page(data_); }
    uint8_t* data() { return data_; }
    const uint8_t* data() const { return data_; }

private:
    alignas(8) uint8_t data_[PAGE_SIZE]{};
    Page view_{data_};
};

} // namespace pg
