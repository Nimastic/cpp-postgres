#pragma once

#include "pg/constants.h"
#include <cstdint>
#include <vector>
#include <cstring>
#include <string>
#include <stdexcept>

namespace pg {

using tx_id_t = uint32_t;
constexpr tx_id_t INVALID_TX_ID = 0;

#pragma pack(push, 1)
// Current Tuple ID: physical address (page_id, slot_id)
struct CTID {
    page_id_t page{INVALID_PAGE_ID};
    slot_id_t slot{INVALID_SLOT_ID};

    constexpr CTID() = default;
    constexpr CTID(page_id_t p, slot_id_t s) : page(p), slot(s) {}

    bool is_valid() const {
        return page != INVALID_PAGE_ID && slot != INVALID_SLOT_ID;
    }

    bool operator==(const CTID& other) const {
        return page == other.page && slot == other.slot;
    }

    bool operator!=(const CTID& other) const {
        return !(*this == other);
    }

    std::string to_string() const {
        return "(" + std::to_string(page) + ", " + std::to_string(slot) + ")";
    }
};

// PostgreSQL Tuple Header (16 bytes)
struct TupleHeader {
    tx_id_t xmin{0};      // Transaction ID that created this tuple version
    tx_id_t xmax{0};      // Transaction ID that deleted or updated this tuple version
    CTID    t_ctid{};     // Points to self, or to newer tuple version on update
    uint16_t infomask{0}; // Status flags
};

// Infomask bit flags for HOT (Heap-Only Tuples) and TOAST
constexpr uint16_t HEAP_HASEXTERNAL  = 0x2000; // Tuple contains out-of-line TOASTed attributes
constexpr uint16_t HEAP_HOT_UPDATED  = 0x4000; // This tuple was HOT-updated; t_ctid points to successor on same page
constexpr uint16_t HEAP_ONLY_TUPLE   = 0x8000; // This tuple is a heap-only tuple (not indexed, reachable only via HOT chain)


// Items Table Schema Record (8 bytes)
struct ItemRecord {
    int32_t item_id{0};
    int32_t price{0};

    bool operator==(const ItemRecord& other) const {
        return item_id == other.item_id && price == other.price;
    }
};

// Full Heap Tuple layout stored on page (24 bytes)
struct HeapTuple {
    TupleHeader header;
    ItemRecord  data;

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buffer(sizeof(HeapTuple));
        std::memcpy(buffer.data(), this, sizeof(HeapTuple));
        return buffer;
    }

    static HeapTuple deserialize(const void* raw_data, size_t len) {
        if (len < sizeof(HeapTuple)) {
            throw std::runtime_error("HeapTuple: Buffer too small to deserialize tuple");
        }
        HeapTuple tuple;
        std::memcpy(&tuple, raw_data, sizeof(HeapTuple));
        return tuple;
    }
};
#pragma pack(pop)

static_assert(sizeof(CTID) == 6, "CTID must be exactly 6 bytes");
static_assert(sizeof(TupleHeader) == 16, "TupleHeader must be exactly 16 bytes");
static_assert(sizeof(ItemRecord) == 8, "ItemRecord must be exactly 8 bytes");
static_assert(sizeof(HeapTuple) == 24, "HeapTuple must be exactly 24 bytes");

} // namespace pg
