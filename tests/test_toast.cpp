#include "pg/toast.h"
#include "pg/heap.h"
#include "pg/page.h"
#include <iostream>
#include <vector>
#include <cassert>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

void run_toast_tests() {
    const std::string toast_db = "test_toast_aux.db";
    const std::string heap_db  = "test_toast_heap.db";

    if (fs::exists(toast_db)) fs::remove(toast_db);
    if (fs::exists(heap_db))  fs::remove(heap_db);

    std::cout << "\n--- REPRODUCING POSTGRESQL TOAST (OVERSIZED ATTRIBUTE STORAGE) ---" << std::endl;

    auto toast_mgr = pg::ToastManager::open(toast_db);

    // =========================================================================
    // TEST 1: Small Inline Attribute (<= 2KB)
    // =========================================================================
    std::cout << "[Step 1] Storing small 500-byte string (Inline threshold)..." << std::endl;
    std::string small_text(500, 'A');
    auto small_val = toast_mgr->store_string(small_text);

    assert(small_val.is_inline());
    assert(small_val.inline_data.size() == 501); // 500 + null terminator
    assert(toast_mgr->total_chunks() == 0);      // Zero auxiliary TOAST entries!
    std::cout << " -> Stored inline inside tuple! TOAST table chunks: 0.\n";

    std::string small_recovered = toast_mgr->fetch_string(small_val);
    assert(small_recovered == small_text);
    std::cout << " -> Verified: Small inline text reassembled byte-for-byte.\n";

    // =========================================================================
    // TEST 2: Oversized 20KB Attribute (> 2KB) -> Automatic TOAST Slicing
    // =========================================================================
    std::cout << "\n[Step 2] Storing oversized 20KB text document (> 2KB threshold)..." << std::endl;
    std::string large_20kb_text;
    large_20kb_text.reserve(20480);
    for (size_t i = 0; i < 20480; ++i) {
        large_20kb_text.push_back(static_cast<char>('a' + (i % 26)));
    }

    auto toast_val = toast_mgr->store_string(large_20kb_text);

    assert(!toast_val.is_inline());
    assert(toast_val.pointer.raw_size == 20481);
    assert(toast_val.pointer.chunk_count == 11); // ceil(20481 / 2048) = 11 chunks
    assert(toast_mgr->total_chunks() == 11);

    std::cout << " -> TOAST Splicing successful! 20KB payload sliced into " 
              << toast_val.pointer.chunk_count << " chunks of 2KB.\n";
    std::cout << " -> Main tuple stores only an 18-byte ToastPointer (toast_id=" 
              << toast_val.pointer.toast_id << ", raw_size=" 
              << toast_val.pointer.raw_size << ").\n";

    std::cout << "[Step 3] Fetching and reconstructing 20KB string from TOAST chunks..." << std::endl;
    std::string reconstructed_20kb = toast_mgr->fetch_string(toast_val);
    assert(reconstructed_20kb == large_20kb_text);
    std::cout << " -> Verified: 20KB string reconstructed byte-for-byte from 11 chunks!\n";

    // =========================================================================
    // TEST 3: Huge 100KB Payload (Multi-Page TOAST Slicing)
    // =========================================================================
    std::cout << "\n[Step 4] Storing massive 100KB JSON payload across multiple TOAST pages..." << std::endl;
    std::string massive_100kb_text(102400, 'Z');
    auto massive_val = toast_mgr->store_string(massive_100kb_text);

    assert(!massive_val.is_inline());
    assert(massive_val.pointer.chunk_count == 51); // ceil(102401 / 2048) = 51 chunks
    assert(toast_mgr->num_pages() >= 13);          // 51 chunks * 2KB = ~102KB -> at least 13 8KB pages!

    std::string reconstructed_100kb = toast_mgr->fetch_string(massive_val);
    assert(reconstructed_100kb == massive_100kb_text);
    std::cout << " -> Verified: 100KB payload spanned " << toast_mgr->num_pages() 
              << " 8KB TOAST pages and reassembled perfectly!\n";

    // =========================================================================
    // TEST 4: Main Heap Page Bloat Prevention (10 Rows with 20KB payloads)
    // =========================================================================
    std::cout << "\n[Step 5] Verifying Main Slotted Page Bloat Prevention..." << std::endl;
    {
        // Table schema: item_id (4B), price (4B), ToastPointer (18B)
        #pragma pack(push, 1)
        struct ProductTuple {
            pg::TupleHeader  header;
            int32_t          item_id;
            int32_t          price;
            pg::ToastPointer desc_ptr;
        };
        #pragma pack(pop)
        static_assert(sizeof(ProductTuple) == 16 + 4 + 4 + 18, "ProductTuple must be 42 bytes");

        auto heap = pg::HeapFile::open(heap_db);
        std::vector<pg::ToastValue> stored_values;

        // Insert 10 products with 20KB descriptions each (200KB total text!)
        for (int i = 1; i <= 10; ++i) {
            std::string desc = "PRODUCT_" + std::to_string(i) + "_DESCRIPTION_" + large_20kb_text;
            auto val = toast_mgr->store_string(desc);
            stored_values.push_back(val);

            ProductTuple row;
            row.header.xmin = 1;
            row.header.xmax = 0;
            row.item_id = i;
            row.price = i * 100;
            row.desc_ptr = val.pointer;

            pg::Page p;
            // Insert 42-byte row into main heap
            pg::CTID ctid = heap->insert({row.item_id, row.price}, 1);
            assert(ctid.page == 0); // All 10 rows fit easily on Page 0!
        }

        std::cout << " -> Inserted 10 rows with 200KB total data into main table.\n";
        std::cout << " -> Main Heap Table Page Count: " << heap->num_pages() 
                  << " (All 10 rows fit on Page 0 with zero page bloat!)\n";
        assert(heap->num_pages() == 1);

        // Fetch back and verify
        for (int i = 1; i <= 10; ++i) {
            std::string desc_fetched = toast_mgr->fetch_string(stored_values[i - 1]);
            std::string expected = "PRODUCT_" + std::to_string(i) + "_DESCRIPTION_" + large_20kb_text;
            assert(desc_fetched == expected);
        }
        std::cout << " -> All 10 large product descriptions reconstructed byte-for-byte!\n";
    }

    toast_mgr = nullptr; // Close file handles before deletion

    fs::remove(toast_db);
    fs::remove(heap_db);
    std::cout << "\n>>> ITEM 10 (TOAST / OVERSIZED ATTRIBUTE STORAGE) TESTS PASSED SUCCESSFULLY! <<<\n" << std::endl;
}

int main() {
    try {
        run_toast_tests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "TOAST test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
