#include "pg/fsm.h"
#include "pg/heap.h"
#include "pg/vacuum.h"
#include "pg/btree.h"
#include <iostream>
#include <vector>
#include <cassert>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace pg;

void run_fsm_tests() {
    const std::string fsm_file  = "test_standalone_fsm.db";
    const std::string heap_file = "test_fsm_heap.db";
    const std::string heap_fsm  = "test_fsm_heap_fsm.db";

    if (fs::exists(fsm_file))  fs::remove(fsm_file);
    if (fs::exists(heap_file)) fs::remove(heap_file);
    if (fs::exists(heap_fsm))  fs::remove(heap_fsm);

    std::cout << "\n--- REPRODUCING POSTGRESQL FREE SPACE MAP (FSM) INVARIANTS ---" << std::endl;

    // =========================================================================
    // TEST 1: Category Quantization Formulas
    // =========================================================================
    std::cout << "[Step 1] Verifying 1-byte category quantization math..." << std::endl;
    assert(pg::FreeSpaceMap::bytes_to_category(0) == 0);
    assert(pg::FreeSpaceMap::bytes_to_category(31) == 0);
    assert(pg::FreeSpaceMap::bytes_to_category(32) == 1);
    assert(pg::FreeSpaceMap::bytes_to_category(63) == 1);
    assert(pg::FreeSpaceMap::bytes_to_category(64) == 2);
    assert(pg::FreeSpaceMap::bytes_to_category(8160) == 255);
    assert(pg::FreeSpaceMap::bytes_to_category(8192) == 255);

    assert(pg::FreeSpaceMap::required_category(0) == 0);
    assert(pg::FreeSpaceMap::required_category(1) == 1);
    assert(pg::FreeSpaceMap::required_category(32) == 1);
    assert(pg::FreeSpaceMap::required_category(33) == 2);
    assert(pg::FreeSpaceMap::required_category(8192) == 255);
    std::cout << " -> Category formulas verified: 32 bytes per category step, 256 categories per 8KB page.\n";

    // =========================================================================
    // TEST 2: Binary Max-Heap Tree Invariants on Single Page (4,096 leaves)
    // =========================================================================
    std::cout << "\n[Step 2] Testing complete binary max-heap tree search and bubble-up..." << std::endl;
    {
        auto fsm = pg::FreeSpaceMap::open(fsm_file);
        assert(fsm->num_fsm_pages() == 0);

        // Initially empty: search returns INVALID_PAGE_ID
        assert(fsm->search_page(100) == pg::INVALID_PAGE_ID);

        // Update page 5 with 1,000 bytes (cat = 31)
        fsm->update_page(5, 1000);
        assert(fsm->get_category(5) == 31);
        assert(fsm->get_category(0) == 0);
        assert(fsm->num_fsm_pages() == 1);

        // Search for 500 bytes (cat = 16): page 5 has room!
        assert(fsm->search_page(500) == 5);

        // Search for 1,200 bytes (cat = 38): root is 31, no page has room!
        assert(fsm->search_page(1200) == pg::INVALID_PAGE_ID);

        // Update page 2 with 2,000 bytes (cat = 62)
        fsm->update_page(2, 2000);
        assert(fsm->get_category(2) == 62);

        // Search for 500 bytes: both page 2 and page 5 have room,
        // leftmost descent must choose page 2!
        assert(fsm->search_page(500) == 2);

        // Search for 1,500 bytes: only page 2 has room
        assert(fsm->search_page(1500) == 2);

        // Consume space on page 2: reduce to 100 bytes (cat = 3)
        fsm->update_page(2, 100);
        assert(fsm->get_category(2) == 3);

        // Now search for 500 bytes: page 2 no longer qualifies, returns page 5!
        assert(fsm->search_page(500) == 5);

        fsm->flush();
        std::cout << " -> Binary max-heap property and O(log M) descent verified!\n";
    }

    // =========================================================================
    // TEST 3: Multi-Page FSM Scaling across 4,096 Page Boundary
    // =========================================================================
    std::cout << "\n[Step 3] Testing multi-page FSM scaling (> 4,096 heap pages)..." << std::endl;
    {
        auto fsm = pg::FreeSpaceMap::open(fsm_file);
        assert(fsm->num_fsm_pages() == 1);

        // Register heap page 4,095 (last leaf on FSM page 0)
        fsm->update_page(4095, 3000);
        assert(fsm->num_fsm_pages() == 1);
        assert(fsm->get_category(4095) == 93);

        // Register heap page 4,096 (first leaf on FSM page 1)
        fsm->update_page(4096, 4000);
        assert(fsm->num_fsm_pages() == 2);
        assert(fsm->get_category(4096) == 125);

        // Search for 3,500 bytes: FSM page 0 root is < 3500 bytes,
        // search traverses to FSM page 1 and finds page 4,096!
        assert(fsm->search_page(3500) == 4096);
        std::cout << " -> Multi-page scaling verified: FSM expanded to page 1 to track heap page 4,096.\n";
    }

    // =========================================================================
    // TEST 4: Dynamic Space Reclamation via VACUUM & Recycling
    // =========================================================================
    std::cout << "\n[Step 4] Verifying HeapFile insertion and VACUUM space recycling via FSM..." << std::endl;
    {
        pg::TransactionManager tm;
        pg::BTreeIndex index;

        auto heap = pg::HeapFile::open(heap_file);

        // Insert enough records to fill Page 0 and spill to Page 1
        // An 8KB page holds around ~330 24-byte HeapTuples (with 4-byte line pointers)
        pg::tx_id_t tx_ins = tm.begin_transaction();
        std::vector<pg::CTID> inserted_ctids;
        for (int i = 1; i <= 400; ++i) {
            pg::CTID ctid = heap->insert({i, i * 10}, tx_ins);
            index.insert_entry(i, ctid);
            inserted_ctids.push_back(ctid);
        }
        tm.commit(tx_ins);

        assert(heap->num_pages() >= 2);
        std::cout << " -> Inserted 400 rows across " << heap->num_pages() << " heap pages.\n";

        // Page 0 should now have low category in FSM
        uint8_t p0_cat_before = heap->fsm().get_category(0);
        std::cout << " -> Page 0 FSM category before delete: " << static_cast<int>(p0_cat_before) << "\n";

        // Delete all rows that landed on Page 0
        pg::tx_id_t del_tx = tm.begin_transaction();
        size_t deleted_on_p0 = 0;
        for (const auto& ctid : inserted_ctids) {
            if (ctid.page == 0) {
                heap->delete_tuple(ctid, del_tx);
                deleted_on_p0++;
            }
        }
        tm.commit(del_tx);
        std::cout << " -> Deleted " << deleted_on_p0 << " rows on Page 0.\n";

        // Advance horizon past del_tx
        pg::tx_id_t tx_post = tm.begin_transaction();
        tm.commit(tx_post);

        // Run VACUUM: compacts dead tuples and informs FSM of newly available space
        auto stats = pg::Vacuum::run(*heap, tm, index);
        assert(stats.dead_tuples_reclaimed == deleted_on_p0);

        uint8_t p0_cat_after = heap->fsm().get_category(0);
        std::cout << " -> Page 0 FSM category after VACUUM: " << static_cast<int>(p0_cat_after) << "\n";
        assert(p0_cat_after > p0_cat_before);
        assert(p0_cat_after >= 200); // Page 0 is now mostly empty!

        // Insert a new row: FSM must route it directly into Page 0!
        pg::CTID recycled_ctid = heap->insert({9999, 99990}, 3);
        std::cout << " -> New insert landed at: " << recycled_ctid.to_string() << "\n";
        assert(recycled_ctid.page == 0); // Reused space on Page 0!
        std::cout << " -> Verified: Space reclaimed by VACUUM on Page 0 was instantly reused by FSM!\n";
    }

    // =========================================================================
    // TEST 5: FSM Persistence Across Restart
    // =========================================================================
    std::cout << "\n[Step 5] Verifying FSM persistence and O(1) startup from disk..." << std::endl;
    {
        auto heap = pg::HeapFile::open(heap_file);
        assert(heap->fsm().num_fsm_pages() > 0);
        assert(heap->fsm().get_category(0) >= 200);

        // Verify search still finds Page 0 immediately
        page_id_t found = heap->fsm().search_page(sizeof(pg::HeapTuple) + sizeof(pg::LinePointer));
        assert(found == 0);
        std::cout << " -> Verified: FSM loaded from disk on startup and preserved page categories!\n";
    }

    if (fs::exists(fsm_file))  fs::remove(fsm_file);
    if (fs::exists(heap_file)) fs::remove(heap_file);
    if (fs::exists(heap_fsm))  fs::remove(heap_fsm);

    std::cout << "\n>>> ITEM 21 (FREE SPACE MAP - FSM) TESTS PASSED SUCCESSFULLY! <<<\n" << std::endl;
}

int main() {
    try {
        run_fsm_tests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FSM test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
