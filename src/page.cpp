#include "pg/page.h"
#include <cstring>
#include <iomanip>
#include <sstream>

namespace pg {

void Page::init() {
    std::memset(data_, 0, PAGE_SIZE);
    auto* hdr = reinterpret_cast<PageHeaderData*>(data_);
    hdr->pd_lsn = 0;
    hdr->pd_checksum = 0;
    hdr->pd_flags = 0;
    hdr->pd_lower = static_cast<uint16_t>(sizeof(PageHeaderData));
    hdr->pd_upper = static_cast<uint16_t>(PAGE_SIZE);
    hdr->pd_special = static_cast<uint16_t>(PAGE_SIZE);
}

const PageHeaderData& Page::header() const {
    return *reinterpret_cast<const PageHeaderData*>(data_);
}

PageHeaderData& Page::header() {
    return *reinterpret_cast<PageHeaderData*>(data_);
}

LinePointer* Page::line_pointers_internal() {
    return reinterpret_cast<LinePointer*>(data_ + sizeof(PageHeaderData));
}

const LinePointer* Page::line_pointers_internal() const {
    return reinterpret_cast<const LinePointer*>(data_ + sizeof(PageHeaderData));
}

size_t Page::num_slots() const {
    const auto& hdr = header();
    if (hdr.pd_lower < sizeof(PageHeaderData)) {
        return 0;
    }
    return (hdr.pd_lower - sizeof(PageHeaderData)) / sizeof(LinePointer);
}

size_t Page::free_space() const {
    const auto& hdr = header();
    if (hdr.pd_upper <= hdr.pd_lower + sizeof(LinePointer)) {
        return 0;
    }
    return (hdr.pd_upper - hdr.pd_lower - sizeof(LinePointer));
}

slot_id_t Page::insert_tuple(const void* data, size_t len) {
    if (data == nullptr || len == 0 || len > PAGE_SIZE) {
        return INVALID_SLOT_ID;
    }

    auto& hdr = header();
    auto* lp_array = line_pointers_internal();
    size_t slots = num_slots();

    // Recycle an UNUSED line pointer if one exists.
    //
    // DEAD is deliberately excluded. A DEAD pointer may still be the target of an
    // index entry that VACUUM has not cleaned yet; handing that slot to a new
    // tuple would make the stale index entry resolve to an unrelated row. VACUUM
    // demotes DEAD to UNUSED only after every index has been vacuumed of that
    // TID, which is exactly PostgreSQL's rule.
    for (size_t i = 0; i < slots; ++i) {
        if (lp_array[i].flags() == ItemFlags::UNUSED) {
            // Reusable slot already exists; only need space for tuple payload
            if (hdr.pd_upper < hdr.pd_lower || (hdr.pd_upper - hdr.pd_lower) < len) {
                return INVALID_SLOT_ID; // Not enough free space
            }

            uint16_t new_upper = static_cast<uint16_t>(hdr.pd_upper - len);
            std::memcpy(data_ + new_upper, data, len);

            lp_array[i].set(new_upper, static_cast<uint16_t>(len), ItemFlags::NORMAL);
            hdr.pd_upper = new_upper;
            return static_cast<slot_id_t>(i + 1);
        }
    }

    // No reusable slot found; append new slot at pd_lower
    size_t space_needed = len + sizeof(LinePointer);
    if (hdr.pd_upper < hdr.pd_lower || (hdr.pd_upper - hdr.pd_lower) < space_needed) {
        return INVALID_SLOT_ID; // Not enough free space on this page
    }

    // Tuples grow downward from the top of the page towards pd_upper
    uint16_t new_upper = static_cast<uint16_t>(hdr.pd_upper - len);
    std::memcpy(data_ + new_upper, data, len);

    // Line pointers grow upward from pd_lower
    LinePointer lp;
    lp.set(new_upper, static_cast<uint16_t>(len), ItemFlags::NORMAL);

    size_t slot_index = (hdr.pd_lower - sizeof(PageHeaderData)) / sizeof(LinePointer);
    lp_array[slot_index] = lp;

    hdr.pd_upper = new_upper;
    hdr.pd_lower += static_cast<uint16_t>(sizeof(LinePointer));

    // Return 1-based slot ID (slot 1, 2, ...)
    return static_cast<slot_id_t>(slot_index + 1);
}

void Page::defragment() {
    auto& hdr = header();
    size_t slots = num_slots();
    if (slots == 0) {
        hdr.pd_upper = static_cast<uint16_t>(PAGE_SIZE);
        return;
    }

    // Temporary copy of page memory to read uncompacted tuples
    uint8_t temp_page[PAGE_SIZE];
    std::memcpy(temp_page, data_, PAGE_SIZE);

    uint16_t current_upper = static_cast<uint16_t>(PAGE_SIZE);
    auto* lp_array = line_pointers_internal();

    for (size_t i = 0; i < slots; ++i) {
        auto& lp = lp_array[i];
        if (lp.flags() == ItemFlags::NORMAL) {
            uint16_t len = lp.length();
            uint16_t old_offset = lp.lp_offset;
            uint16_t new_offset = current_upper - len;

            // Copy tuple payload into compacted position
            std::memcpy(data_ + new_offset, temp_page + old_offset, len);

            // Update line pointer with new offset
            lp.set(new_offset, len, ItemFlags::NORMAL);
            current_upper = new_offset;
        } else if (lp.flags() == ItemFlags::DEAD) {
            // Dead tuple keeps its slot but loses its storage. The slot number
            // stays valid so index entries pointing at it still resolve (to
            // nothing) rather than to some other row.
            lp.set(0, 0, ItemFlags::DEAD);
        } else if (lp.flags() == ItemFlags::REDIRECT) {
            // A HOT chain root: lp_offset holds the successor slot number, not a
            // byte offset, so it must survive compaction untouched.
        }
    }

    // Expand pd_upper to the new start of the youngest live tuple
    hdr.pd_upper = current_upper;
}

std::optional<LinePointer> Page::get_line_pointer(slot_id_t slot_id) const {
    if (slot_id == INVALID_SLOT_ID || slot_id > num_slots()) {
        return std::nullopt;
    }
    return line_pointers_internal()[slot_id - 1];
}

bool Page::set_line_pointer(slot_id_t slot_id, const LinePointer& lp) {
    if (slot_id == INVALID_SLOT_ID || slot_id > num_slots()) {
        return false;
    }
    line_pointers_internal()[slot_id - 1] = lp;
    return true;
}

const uint8_t* Page::get_tuple_ptr(slot_id_t slot_id, size_t* out_len) const {
    auto lp_opt = get_line_pointer(slot_id);
    if (!lp_opt.has_value()) {
        return nullptr;
    }

    const auto& lp = lp_opt.value();
    if (lp.flags() != ItemFlags::NORMAL) {
        return nullptr;
    }

    if (out_len != nullptr) {
        *out_len = lp.length();
    }
    return data_ + lp.lp_offset;
}

uint8_t* Page::get_tuple_ptr_mut(slot_id_t slot_id, size_t* out_len) {
    return const_cast<uint8_t*>(static_cast<const Page*>(this)->get_tuple_ptr(slot_id, out_len));
}

// Place a tuple at one specific slot. Used only by WAL redo, which must
// reproduce the physical layout that was logged rather than let the page pick.
bool Page::insert_tuple_at(slot_id_t slot_id, const void* data, size_t len) {
    if (data == nullptr || len == 0 || slot_id == INVALID_SLOT_ID) {
        return false;
    }

    auto& hdr = header();

    // Grow the line pointer array until it covers slot_id.
    while (num_slots() < slot_id) {
        if (hdr.pd_upper < hdr.pd_lower ||
            (hdr.pd_upper - hdr.pd_lower) < sizeof(LinePointer)) {
            return false;
        }
        size_t idx = (hdr.pd_lower - sizeof(PageHeaderData)) / sizeof(LinePointer);
        line_pointers_internal()[idx].set(0, 0, ItemFlags::UNUSED);
        hdr.pd_lower += static_cast<uint16_t>(sizeof(LinePointer));
    }

    auto& lp = line_pointers_internal()[slot_id - 1];
    if (lp.flags() == ItemFlags::NORMAL && lp.length() == len) {
        // Already present at this slot; redo is idempotent, so overwrite in place.
        std::memcpy(data_ + lp.lp_offset, data, len);
        return true;
    }

    if (hdr.pd_upper < hdr.pd_lower || (hdr.pd_upper - hdr.pd_lower) < len) {
        return false;
    }

    uint16_t new_upper = static_cast<uint16_t>(hdr.pd_upper - len);
    std::memcpy(data_ + new_upper, data, len);
    lp.set(new_upper, static_cast<uint16_t>(len), ItemFlags::NORMAL);
    hdr.pd_upper = new_upper;
    return true;
}

std::optional<std::vector<uint8_t>> Page::get_tuple(slot_id_t slot_id) const {
    size_t len = 0;
    const uint8_t* ptr = get_tuple_ptr(slot_id, &len);
    if (ptr == nullptr) {
        return std::nullopt;
    }
    return std::vector<uint8_t>(ptr, ptr + len);
}

std::string Page::dump() const {
    std::ostringstream ss;
    const auto& hdr = header();
    size_t slots = num_slots();

    ss << "====================== PAGE LAYOUT DUMP ======================\n";
    ss << "Header Size   : " << sizeof(PageHeaderData) << " bytes\n";
    ss << "pd_lower      : " << hdr.pd_lower << " (end of line pointers)\n";
    ss << "pd_upper      : " << hdr.pd_upper << " (start of youngest tuple)\n";
    ss << "Free Space    : " << (hdr.pd_upper > hdr.pd_lower ? hdr.pd_upper - hdr.pd_lower : 0) << " bytes\n";
    ss << "Slot Count    : " << slots << " items\n";
    ss << "--------------------------------------------------------------\n";
    ss << " [0.." << sizeof(PageHeaderData) - 1 << "] PageHeaderData (" << sizeof(PageHeaderData) << "B)\n";

    if (slots > 0) {
        ss << " [Line Pointers -> growing forward from offset " << sizeof(PageHeaderData) << " to " << hdr.pd_lower << "]\n";
        const auto* lp_array = line_pointers_internal();
        for (size_t i = 0; i < slots; ++i) {
            const auto& lp = lp_array[i];
            const char* flag_str = "UNUSED";
            switch (lp.flags()) {
                case ItemFlags::NORMAL:   flag_str = "NORMAL"; break;
                case ItemFlags::REDIRECT: flag_str = "REDIRECT"; break;
                case ItemFlags::DEAD:     flag_str = "DEAD"; break;
                default:                  flag_str = "UNKNOWN"; break;
            }
            ss << "   Slot " << std::setw(2) << (i + 1)
               << ": offset=" << std::setw(4) << lp.lp_offset
               << ", len=" << std::setw(4) << lp.length()
               << ", flags=" << flag_str << "\n";
        }
    }

    ss << " [" << hdr.pd_lower << ".." << hdr.pd_upper << "] Free Space Gap (" 
       << (hdr.pd_upper > hdr.pd_lower ? hdr.pd_upper - hdr.pd_lower : 0) << " bytes)\n";

    if (hdr.pd_upper < PAGE_SIZE) {
        ss << " [" << hdr.pd_upper << ".." << (PAGE_SIZE - 1) << "] Tuple Storage Area (<- growing backward)\n";
    }
    ss << "==============================================================\n";
    return ss.str();
}

} // namespace pg
