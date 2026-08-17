#include "pg/buffer_pool.h"
#include "pg/pager.h"
#include "pg/page.h"
#include <iostream>
#include <vector>
#include <cassert>
#include <filesystem>
#include <cstring>

namespace fs = std::filesystem;

void run_buffer_pool_tests() {
    const std::string db_name = "test_bpm.db";
    if (fs::exists(db_name)) {
        fs::remove(db_name);
    }

    // Step 0: Prepare a database file with 4 distinct 8KB pages
    {
        auto pager = pg::Pager::open(db_name);
        for (pg::page_id_t pid = 0; pid < 4; ++pid) {
            pager->allocate_page();
            pg::Page p;
            // Write a distinct signature onto each page
            std::string sig = "PAGE_SIGNATURE_" + std::to_string(pid);
            p.insert_tuple(sig.data(), sig.size() + 1);
            pager->write_page(pid, p.data());
        }
        assert(pager->num_pages() == 4);
    }

    // =========================================================================
    // TEST 1: Cache Hits, Misses, and Fixed Pool Size (Pool Size = 3)
    // =========================================================================
    std::cout << "\n--- REPRODUCING POSTGRESQL SHARED BUFFERS (BUFFER POOL) ---" << std::endl;
    {
        auto pager = pg::Pager::open(db_name);
        pg::BufferPoolManager bpm(*pager, 3); // 3 frames in RAM

        std::cout << "[Step 1] Loading Pages 0, 1, 2 into 3-frame buffer pool..." << std::endl;
        pg::Page* p0 = bpm.fetch_page(0); // Cache miss -> Frame 0
        pg::Page* p1 = bpm.fetch_page(1); // Cache miss -> Frame 1
        pg::Page* p2 = bpm.fetch_page(2); // Cache miss -> Frame 2

        assert(bpm.resident_pages() == 3);
        assert(bpm.is_resident(0) && bpm.is_resident(1) && bpm.is_resident(2));
        assert(bpm.get_pin_count(0) == 1);
        std::cout << " -> Pages 0, 1, 2 loaded into RAM frames. Pool full.\n";

        std::cout << "[Step 2] Testing Cache Hit on resident Page 0..." << std::endl;
        pg::Page* p0_hit = bpm.fetch_page(0); // Cache Hit!
        assert(p0 == p0_hit); // Returned exact same memory address in RAM
        assert(bpm.get_pin_count(0) == 2);
        std::cout << " -> Cache Hit verified! Pin count on Page 0 incremented to 2.\n";

        // Unpin all pages
        bpm.unpin_page(0, false);
        bpm.unpin_page(0, false);
        bpm.unpin_page(1, false);
        bpm.unpin_page(2, false);
        assert(bpm.get_pin_count(0) == 0);
        assert(bpm.get_pin_count(1) == 0);
        assert(bpm.get_pin_count(2) == 0);
        std::cout << " -> All frames unpinned (pin_count = 0).\n";

        // =====================================================================
        // TEST 2: Clock-Sweep Eviction of Clean Page (Page 3 requested)
        // =====================================================================
        std::cout << "\n[Step 3] Fetching Page 3 to trigger Clock-Sweep eviction..." << std::endl;
        pg::Page* p3 = bpm.fetch_page(3); // Cache miss! Pool is full -> Clock sweep evicts victim
        assert(bpm.resident_pages() == 3);
        assert(bpm.is_resident(3));
        std::cout << " -> Page 3 loaded into RAM. Clean victim frame evicted silently without disk write.\n";
        bpm.unpin_page(3, false);

        // =====================================================================
        // TEST 3: Dirty Page Eviction with Automatic Disk Writeback
        // =====================================================================
        std::cout << "\n[Step 4] Modifying resident page and verifying dirty writeback upon eviction..." << std::endl;
        // Make sure Page 1 is resident and modify it
        pg::Page* p1_mod = bpm.fetch_page(1);
        std::string mod_sig = "MODIFIED_IN_SHARED_BUFFERS_PID1";
        p1_mod->insert_tuple(mod_sig.data(), mod_sig.size() + 1);
        bpm.unpin_page(1, true); // Mark DIRTY!
        assert(bpm.is_dirty(1));

        // Fetch other pages to rotate the clock hand and force eviction of dirty Page 1
        bpm.fetch_page(0); bpm.unpin_page(0, false);
        bpm.fetch_page(2); bpm.unpin_page(2, false);
        bpm.fetch_page(3); bpm.unpin_page(3, false);
        bpm.fetch_page(0); bpm.unpin_page(0, false);

        // Verify Page 1 was evicted from RAM
        assert(!bpm.is_resident(1));
        std::cout << " -> Dirty Page 1 was evicted by Clock Sweep and automatically written to disk.\n";
    }

    // Verify on disk that Page 1 was persisted
    {
        auto pager = pg::Pager::open(db_name);
        std::vector<uint8_t> disk_buf(pg::PAGE_SIZE, 0);
        pager->read_page(1, disk_buf.data());
        pg::Page p1_disk(disk_buf.data());
        
        // Check if modified tuple is present on disk
        bool found_mod = false;
        for (pg::slot_id_t s = 1; s <= p1_disk.num_slots(); ++s) {
            auto raw = p1_disk.get_tuple(s);
            if (raw.has_value()) {
                std::string content(reinterpret_cast<char*>(raw->data()));
                if (content.find("MODIFIED_IN_SHARED_BUFFERS") != std::string::npos) {
                    found_mod = true;
                }
            }
        }
        assert(found_mod);
        std::cout << " -> Disk file verified: Modified bytes persisted onto disk Page 1!\n";
    }

    // =========================================================================
    // TEST 4: Pinning Protection (Pinned pages CANNOT be evicted)
    // =========================================================================
    std::cout << "\n[Step 5] Testing Pinning Protection (Active pages cannot be evicted)..." << std::endl;
    {
        auto pager = pg::Pager::open(db_name);
        pg::BufferPoolManager bpm(*pager, 3);

        pg::Page* p0 = bpm.fetch_page(0); // pinned (pin_count = 1)
        pg::Page* p1 = bpm.fetch_page(1); // pinned (pin_count = 1)
        pg::Page* p2 = bpm.fetch_page(2); // unpin immediately
        bpm.unpin_page(2, false);

        // Fetch Page 3 -> Clock sweep MUST skip Page 0 and Page 1, and evict Page 2!
        pg::Page* p3 = bpm.fetch_page(3);
        assert(bpm.is_resident(0)); // Protected by pin!
        assert(bpm.is_resident(1)); // Protected by pin!
        assert(bpm.is_resident(3)); // Newly loaded
        assert(!bpm.is_resident(2)); // Evicted because pin_count == 0!

        std::cout << " -> Clock Sweep respected pins: Pinned Pages 0 & 1 preserved, unpinned Page 2 evicted!\n";

        bpm.unpin_page(0, false);
        bpm.unpin_page(1, false);
        bpm.unpin_page(3, false);
    }

    fs::remove(db_name);
    std::cout << "\n>>> ITEM 8 (SHARED BUFFERS / BUFFER POOL) TESTS PASSED SUCCESSFULLY! <<<\n" << std::endl;
}

int main() {
    try {
        run_buffer_pool_tests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Buffer pool test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
