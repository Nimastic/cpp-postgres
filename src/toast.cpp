#include "pg/toast.h"
#include <iostream>

namespace pg {

ToastManager::ToastManager(std::unique_ptr<Pager> toast_pager) : pager_(std::move(toast_pager)) {
    if (pager_ && pager_->num_pages() == 0) {
        // Initialize Page 0 for TOAST table
        page_id_t pid = pager_->allocate_page();
        PageBuffer p0;
        pager_->write_page(pid, p0.data());
    }
}

std::unique_ptr<ToastManager> ToastManager::open(const std::string& toast_filepath) {
    auto pager = Pager::open(toast_filepath);
    return std::make_unique<ToastManager>(std::move(pager));
}

ToastValue ToastManager::store(const void* data, size_t len) {
    // Small payload (<= 2KB): Stored directly inline in standard HeapTuple
    if (len <= TOAST_TUPLE_THRESHOLD) {
        ToastValue val;
        val.type = ToastStorageType::INLINE;
        if (data != nullptr && len > 0) {
            val.inline_data.resize(len);
            std::memcpy(val.inline_data.data(), data, len);
        }
        return val;
    }

    // Large payload (> 2KB): Splice into 2KB chunks and store in auxiliary TOAST table
    uint64_t toast_id = next_toast_id_++;
    uint32_t chunk_count = static_cast<uint32_t>((len + TOAST_CHUNK_SIZE - 1) / TOAST_CHUNK_SIZE);

    const uint8_t* byte_ptr = static_cast<const uint8_t*>(data);
    size_t bytes_remaining = len;

    for (uint32_t seq = 0; seq < chunk_count; ++seq) {
        size_t current_chunk_len = std::min(bytes_remaining, TOAST_CHUNK_SIZE);
        std::vector<uint8_t> chunk_buf(byte_ptr, byte_ptr + current_chunk_len);

        chunk_index_[{toast_id, seq}] = chunk_buf;
        flush_chunk_to_page(toast_id, seq, byte_ptr, current_chunk_len);

        byte_ptr += current_chunk_len;
        bytes_remaining -= current_chunk_len;
    }

    // Return a tiny 18-byte ToastPointer to be placed in main heap tuple
    ToastValue val;
    val.type = ToastStorageType::OUT_OF_LINE;
    val.pointer.toast_id = toast_id;
    val.pointer.raw_size = static_cast<uint32_t>(len);
    val.pointer.chunk_count = chunk_count;
    val.pointer.flags = 0;

    return val;
}

ToastValue ToastManager::store_string(const std::string& text) {
    return store(text.data(), text.size() + 1); // include null terminator
}

std::vector<uint8_t> ToastManager::fetch(const ToastValue& val) {
    if (val.is_inline()) {
        return val.inline_data;
    }

    // Reconstruct full buffer from ordered chunks (0..N-1)
    std::vector<uint8_t> reconstructed;
    reconstructed.reserve(val.pointer.raw_size);

    for (uint32_t seq = 0; seq < val.pointer.chunk_count; ++seq) {
        auto it = chunk_index_.find({val.pointer.toast_id, seq});
        if (it == chunk_index_.end()) {
            throw std::runtime_error("ToastManager: Corrupt TOAST pointer - missing chunk " + 
                                     std::to_string(seq) + " for toast_id " + 
                                     std::to_string(val.pointer.toast_id));
        }
        reconstructed.insert(reconstructed.end(), it->second.begin(), it->second.end());
    }

    return reconstructed;
}

std::string ToastManager::fetch_string(const ToastValue& val) {
    auto bytes = fetch(val);
    if (bytes.empty()) {
        return "";
    }
    return std::string(reinterpret_cast<const char*>(bytes.data()));
}

bool ToastManager::delete_value(uint64_t toast_id) {
    bool erased = false;
    for (auto it = chunk_index_.begin(); it != chunk_index_.end(); ) {
        if (it->first.first == toast_id) {
            it = chunk_index_.erase(it);
            erased = true;
        } else {
            ++it;
        }
    }
    return erased;
}

void ToastManager::flush_chunk_to_page(uint64_t toast_id, uint32_t chunk_seq, const void* data, size_t len) {
    if (!pager_) return;

    page_id_t target_page_id = static_cast<page_id_t>(pager_->num_pages() - 1);
    std::vector<uint8_t> page_buffer(PAGE_SIZE, 0);
    pager_->read_page(target_page_id, page_buffer.data());

    Page page(page_buffer.data());
    slot_id_t slot = page.insert_tuple(data, len);

    if (slot != INVALID_SLOT_ID) {
        pager_->write_page(target_page_id, page.data());
        return;
    }

    // Current TOAST page is full -> allocate new 8KB TOAST page
    page_id_t new_pid = pager_->allocate_page();
    PageBuffer new_page;
    slot_id_t new_slot = new_page->insert_tuple(data, len);
    if (new_slot == INVALID_SLOT_ID) {
        throw std::runtime_error("ToastManager: Chunk larger than empty 8KB page");
    }

    pager_->write_page(new_pid, new_page.data());
}

} // namespace pg
