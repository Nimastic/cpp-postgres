#include "pg/disk_btree.h"
#include <iostream>
#include <vector>
#include <cassert>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

void run_disk_btree_tests() {
    const std::string btree_db = "test_disk_btree.db";

    if (fs::exists(btree_db)) fs::remove(btree_db);

    std::cout << "\n--- REPRODUCING POSTGRESQL ON-DISK B-TREE INDEX WITH PAGE SPLITS ---" << std::endl;

    // =========================================================================
    // TEST 1: Basic Insertion & Point Lookup
    // =========================================================================
    std::cout << "[Step 1] Inserting 10 keys into DiskBTree..." << std::endl;
    {
        auto tree = pg::DiskBTree::open(btree_db);

        tree->insert_entry(100, pg::CTID(0, 1));
        tree->insert_entry(200, pg::CTID(0, 2));
        tree->insert_entry(50,  pg::CTID(0, 3));
        tree->insert_entry(300, pg::CTID(0, 4));
        tree->insert_entry(150, pg::CTID(0, 5));

        auto res100 = tree->find_entries(100);
        assert(res100.size() == 1);
        assert(res100[0] == pg::CTID(0, 1));

        auto res50 = tree->find_entries(50);
        assert(res50.size() == 1);
        assert(res50[0] == pg::CTID(0, 3));

        auto res999 = tree->find_entries(999);
        assert(res999.empty());

        std::cout << " -> Point lookups verified for single-page tree.\n";
    }

    // =========================================================================
    // TEST 2: Triggering Leaf and Internal Node Splits (500 Keys)
    // =========================================================================
    std::cout << "\n[Step 2] Inserting 500 keys to trigger node splits and tree growth..." << std::endl;
    {
        auto tree = pg::DiskBTree::open(btree_db);

        for (int i = 1; i <= 500; ++i) {
            tree->insert_entry(i * 10, pg::CTID(i / 100, (i % 100) + 1));
        }

        std::cout << " -> Total 8KB B-Tree pages allocated: " << tree->num_pages() << "\n";
        assert(tree->num_pages() >= 5); // 500 keys / 64 max keys = multiple splits!

        // Verify point queries across split leaf nodes
        for (int i = 1; i <= 500; i += 25) {
            int key = i * 10;
            auto res = tree->find_entries(key);
            assert(!res.empty());
            assert(res[0] == pg::CTID(i / 100, (i % 100) + 1));
        }
        std::cout << " -> Point queries across all split nodes verified successfully.\n";
    }

    // =========================================================================
    // TEST 3: Range Scans Traversing Sibling Linked List
    // =========================================================================
    std::cout << "\n[Step 3] Testing Range Scan [2000 .. 2500] across multiple leaf pages..." << std::endl;
    {
        auto tree = pg::DiskBTree::open(btree_db);

        auto range_res = tree->range_scan(2000, 2500);
        std::cout << " -> Range scan returned " << range_res.size() << " entries.\n";
        assert(range_res.size() == 51); // 2000, 2010, ..., 2500

        // Verify sorted order
        for (size_t i = 0; i < range_res.size(); ++i) {
            assert(range_res[i].first == static_cast<pg::index_key_t>(2000 + (i * 10)));
        }
        std::cout << " -> Range scan ordering and sibling traversal verified!\n";
    }

    // =========================================================================
    // TEST 4: On-Disk Persistence across Reopen (Zero Heap Scan)
    // =========================================================================
    std::cout << "\n[Step 4] Reopening DiskBTree from disk and validating persistent tree..." << std::endl;
    {
        auto tree = pg::DiskBTree::open(btree_db);

        auto res = tree->find_entries(3500); // Key 3500 (i = 350)
        assert(!res.empty());
        assert(res[0] == pg::CTID(350 / 100, (350 % 100) + 1));
        std::cout << " -> Key 3500 fetched directly from disk B-Tree index without scanning heap!\n";
    }

    // =========================================================================
    // TEST 5: Multi-Version Duplicate Keys (MVCC Candidate Dereferencing)
    // =========================================================================
    std::cout << "\n[Step 5] Testing multi-version duplicate keys for MVCC..." << std::endl;
    {
        auto tree = pg::DiskBTree::open(btree_db);

        // Insert duplicate key 7777 pointing to 2 different CTIDs (simulating non-HOT update)
        tree->insert_entry(7777, pg::CTID(10, 1));
        tree->insert_entry(7777, pg::CTID(10, 2));

        auto candidates = tree->find_entries(7777);
        assert(candidates.size() == 2);
        assert(candidates[0] == pg::CTID(10, 1));
        assert(candidates[1] == pg::CTID(10, 2));
        std::cout << " -> Multi-version candidate accumulation verified (2 CTIDs returned for key 7777).\n";
    }

    fs::remove(btree_db);
    std::cout << "\n>>> ITEM 14 (DISK-RESIDENT B-TREE WITH PAGE SPLITS) TESTS PASSED SUCCESSFULLY! <<<\n" << std::endl;
}

int main() {
    try {
        run_disk_btree_tests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "DiskBTree test failed: " << e.what() << std::endl;
        return 1;
    }
}
