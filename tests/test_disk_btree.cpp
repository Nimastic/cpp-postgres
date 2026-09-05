#include "pg/disk_btree.h"
#include "pg/engine.h"
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

    // =========================================================================
    // TEST 6: Removal of Entries (VACUUM Phase 2 Index Pruning)
    // =========================================================================
    std::cout << "\n[Step 6] Testing entry removal (remove_entry) for VACUUM pruning..." << std::endl;
    {
        auto tree = pg::DiskBTree::open(btree_db);

        size_t initial_entries = tree->num_entries();
        size_t initial_unique = tree->num_unique_keys();
        std::cout << " -> Initial entries: " << initial_entries << ", unique keys: " << initial_unique << "\n";

        // Remove one of the duplicate entries for 7777
        bool removed1 = tree->remove_entry(7777, pg::CTID(10, 1));
        assert(removed1);
        auto candidates_after1 = tree->find_entries(7777);
        assert(candidates_after1.size() == 1);
        assert(candidates_after1[0] == pg::CTID(10, 2));
        assert(tree->num_entries() == initial_entries - 1);
        assert(tree->num_unique_keys() == initial_unique); // 7777 still has 1 entry

        // Remove the remaining entry for 7777
        bool removed2 = tree->remove_entry(7777, pg::CTID(10, 2));
        assert(removed2);
        auto candidates_after2 = tree->find_entries(7777);
        assert(candidates_after2.empty());
        assert(tree->num_entries() == initial_entries - 2);
        assert(tree->num_unique_keys() == initial_unique - 1); // 7777 completely removed

        // Non-existent removal
        bool removed_nonexistent = tree->remove_entry(99999, pg::CTID(0, 1));
        assert(!removed_nonexistent);

        std::cout << " -> remove_entry verified for duplicates, complete key removal, and non-existent keys.\n";
    }

    // =========================================================================
    // TEST 7: Tree Inspection and Diagnostic Dump
    // =========================================================================
    std::cout << "\n[Step 7] Testing diagnostic dump()..." << std::endl;
    {
        auto tree = pg::DiskBTree::open(btree_db);
        std::string dump_str = tree->dump();
        assert(!dump_str.empty());
        assert(dump_str.find("ON-DISK B-TREE INDEX DUMP") != std::string::npos);
        std::cout << " -> dump() format verified.\n";
    }

    fs::remove(btree_db);

    // =========================================================================
    // TEST 8: Engine Integration & On-Disk Index Persistence across Restart
    // =========================================================================
    std::cout << "\n[Step 8] Testing Engine integration with DiskBTree across restart..." << std::endl;
    const std::string engine_prefix = "test_eng_btree";
    auto cleanup_engine_files = [&]() {
        for (const auto& ext : {"_heap.db", "_wal.log", "_toast.db", "_clog.db", "_control.db", "_index.db"}) {
            std::string path = engine_prefix + ext;
            if (fs::exists(path)) fs::remove(path);
        }
    };
    cleanup_engine_files();
    {
        pg::Engine engine(engine_prefix);
        engine.execute("INSERT INTO items VALUES (100, 10);");
        engine.execute("INSERT INTO items VALUES (200, 20);");
        engine.execute("INSERT INTO items VALUES (300, 30);");

        assert(engine.index().num_entries() == 3);
        assert(engine.index().num_unique_keys() == 3);

        std::string res = engine.execute("SELECT * FROM items WHERE item_id = 200;");
        assert(res.find("20") != std::string::npos);
        assert(res.find("B-Tree Index Scan") != std::string::npos);
    }
    // Verify the on-disk index file was created and is non-empty
    assert(fs::exists(engine_prefix + "_index.db"));
    assert(fs::file_size(engine_prefix + "_index.db") >= 8192);

    // Re-open Engine: it opens the on-disk B-Tree index directly without heap rebuilding!
    {
        pg::Engine engine(engine_prefix);
        assert(engine.index().num_entries() == 3);
        assert(engine.index().num_unique_keys() == 3);

        std::string res = engine.execute("SELECT * FROM items WHERE item_id = 200;");
        assert(res.find("20") != std::string::npos);
        assert(res.find("B-Tree Index Scan") != std::string::npos);
        std::cout << " -> Engine restarted: on-disk B-Tree index preserved entries and served point query.\n";
    }
    cleanup_engine_files();

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
