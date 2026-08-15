#include "pg/heap.h"
#include <iostream>
#include <vector>
#include <cassert>
#include <filesystem>

namespace fs = std::filesystem;

void run_heap_tests() {
    const std::string db_name = "test_items_heap.db";
    if (fs::exists(db_name)) {
        fs::remove(db_name);
    }

    std::cout << "[Test 1] Opening HeapFile and verifying Page 0 initialization..." << std::endl;
    {
        auto heap = pg::HeapFile::open(db_name);
        assert(heap->num_pages() == 1);
        std::cout << " -> HeapFile initialized with 1 page.\n";

        std::cout << "[Test 2] Inserting (100, $10) and (200, $5) from video lecture..." << std::endl;
        pg::CTID ctid1 = heap->insert({100, 10});
        pg::CTID ctid2 = heap->insert({200, 5});

        std::cout << " -> Inserted item 100 at CTID: " << ctid1.to_string() << std::endl;
        std::cout << " -> Inserted item 200 at CTID: " << ctid2.to_string() << std::endl;

        assert(ctid1 == pg::CTID(0, 1));
        assert(ctid2 == pg::CTID(0, 2));

        std::cout << "[Test 3] Fetching tuples directly by CTID..." << std::endl;
        auto t1 = heap->get(ctid1);
        assert(t1.has_value());
        assert(t1->data.item_id == 100);
        assert(t1->data.price == 10);
        assert(t1->header.xmin == 0);
        assert(t1->header.xmax == 0);
        assert(t1->header.t_ctid == ctid1);

        auto t2 = heap->get(ctid2);
        assert(t2.has_value());
        assert(t2->data.item_id == 200);
        assert(t2->data.price == 5);
        assert(t2->header.t_ctid == ctid2);
        std::cout << " -> Fetched and verified both tuples by CTID successfully.\n";

        std::cout << "[Test 4] Running Sequential Heap Scan..." << std::endl;
        auto scan_results = heap->seq_scan();
        assert(scan_results.size() == 2);
        assert(scan_results[0].first == ctid1);
        assert(scan_results[0].second.data.item_id == 100);
        assert(scan_results[1].first == ctid2);
        assert(scan_results[1].second.data.item_id == 200);
        std::cout << " -> Sequential scan returned " << scan_results.size() << " tuples in physical order.\n";

        std::cout << "[Test 5] Inserting 500 rows to trigger multi-page allocation..." << std::endl;
        // Each tuple takes 24B payload + 4B line pointer = 28B.
        // Page 0 can hold ~291 tuples. 500 rows will spill over to Page 1.
        for (int i = 1; i <= 500; ++i) {
            pg::CTID ctid = heap->insert({1000 + i, i * 2});
            if (i == 300) {
                std::cout << " -> Row 300 landed on: " << ctid.to_string() << std::endl;
                assert(ctid.page >= 1); // Confirms multi-page expansion
            }
        }

        assert(heap->num_pages() >= 2);
        std::cout << " -> Total pages allocated: " << heap->num_pages() << std::endl;

        auto full_scan = heap->seq_scan();
        assert(full_scan.size() == 502); // 2 initial + 500 bulk
        std::cout << " -> Sequential scan across multiple pages returned all " << full_scan.size() << " tuples!\n";
    }

    std::cout << "[Test 6] Re-opening HeapFile to verify disk persistence..." << std::endl;
    {
        auto heap = pg::HeapFile::open(db_name);
        assert(heap->num_pages() >= 2);

        auto t1 = heap->get(pg::CTID(0, 1));
        assert(t1.has_value());
        assert(t1->data.item_id == 100);
        assert(t1->data.price == 10);

        auto full_scan = heap->seq_scan();
        assert(full_scan.size() == 502);
        std::cout << " -> Disk persistence verified! All 502 tuples reloaded successfully.\n";
    }

    fs::remove(db_name);
    std::cout << "\n>>> ITEM 3 (TUPLES & HEAP CTIDS) TESTS PASSED SUCCESSFULLY! <<<\n" << std::endl;
}

int main() {
    try {
        run_heap_tests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Heap test failed: " << e.what() << std::endl;
        return 1;
    }
}
