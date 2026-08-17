#include "pg/heap.h"
#include "pg/tx.h"
#include "pg/mvcc.h"
#include "pg/btree.h"
#include <iostream>
#include <vector>
#include <cassert>
#include <filesystem>

namespace fs = std::filesystem;

void run_index_tests() {
    const std::string db_name = "test_index.db";
    if (fs::exists(db_name)) {
        fs::remove(db_name);
    }

    {
        pg::TransactionManager tm;
        pg::BTreeIndex index;
        auto heap = pg::HeapFile::open(db_name);

        std::cout << "\n--- REPRODUCING B-TREE INDEX (KEY -> CTID) AND MVCC INTEGRATION ---" << std::endl;

        // =====================================================================
        // STEP 1: Insert item 100 ($10) and index it
        // =====================================================================
        std::cout << "[Step 1] Tx 1: Inserting (100, $10) into heap and index..." << std::endl;
        pg::tx_id_t tx1 = tm.begin_transaction();
        pg::CTID ctid1 = heap->insert({100, 10}, tx1);
        index.insert_entry(100, ctid1);
        tm.commit(tx1);
        assert(ctid1 == pg::CTID(0, 1));
        std::cout << " -> Inserted into heap at " << ctid1.to_string() 
                  << " and registered in B-Tree index.\n";

        // =====================================================================
        // STEP 2: Start long-running transaction Tx 2
        // =====================================================================
        std::cout << "[Step 2] Tx 2: Starting transaction and freezing snapshot..." << std::endl;
        pg::tx_id_t tx2 = tm.begin_transaction();
        pg::Snapshot snap2 = tm.take_snapshot(tx2);

        // =====================================================================
        // STEP 3: Tx 3 updates item 100 to $20 and adds second index entry
        // =====================================================================
        std::cout << "[Step 3] Tx 3: Updating item 100 to $20..." << std::endl;
        pg::tx_id_t tx3 = tm.begin_transaction();
        pg::CTID ctid2 = heap->update(ctid1, {100, 20}, tx3);
        // Non-HOT update: Postgres must add a new index entry pointing to the new version!
        index.insert_entry(100, ctid2);
        tm.commit(tx3);
        assert(ctid2 == pg::CTID(0, 2));
        std::cout << " -> Updated in heap at " << ctid2.to_string() 
                  << " and registered second entry in B-Tree index.\n";

        // =====================================================================
        // STEP 4: Start Tx 4 after update commit
        // =====================================================================
        std::cout << "[Step 4] Tx 4: Starting new transaction after commit..." << std::endl;
        pg::tx_id_t tx4 = tm.begin_transaction();
        pg::Snapshot snap4 = tm.take_snapshot(tx4);

        // =====================================================================
        // STEP 5: Verify B-Tree Index Multi-Version Contents
        // =====================================================================
        std::cout << "\n[Step 5] Inspecting B-Tree Index Structure..." << std::endl;
        std::cout << index.dump();
        auto candidate_ctids = index.find_entries(100);
        assert(candidate_ctids.size() == 2);
        assert(candidate_ctids[0] == pg::CTID(0, 1));
        assert(candidate_ctids[1] == pg::CTID(0, 2));
        std::cout << " -> B-Tree holds 2 candidate CTIDs for key 100: (0, 1) and (0, 2)!\n";

        // =====================================================================
        // STEP 6: Execute Index Point Queries and MVCC Evaluation
        // =====================================================================
        std::cout << "\n[Step 6] Executing Index Lookups with Snapshot Isolation..." << std::endl;

        // Query via Tx 2 Snapshot (Must return $10)
        auto res_tx2 = pg::index_lookup(index, *heap, 100, snap2, tm);
        assert(res_tx2.has_value());
        assert(res_tx2->first == pg::CTID(0, 1));
        assert(res_tx2->second.data.price == 10);
        std::cout << " -> Tx 2 Index Lookup returned CTID " << res_tx2->first.to_string() 
                  << " with Price = $" << res_tx2->second.data.price << " [VERIFIED $10]!\n";

        // Query via Tx 4 Snapshot (Must return $20)
        auto res_tx4 = pg::index_lookup(index, *heap, 100, snap4, tm);
        assert(res_tx4.has_value());
        assert(res_tx4->first == pg::CTID(0, 2));
        assert(res_tx4->second.data.price == 20);
        std::cout << " -> Tx 4 Index Lookup returned CTID " << res_tx4->first.to_string() 
                  << " with Price = $" << res_tx4->second.data.price << " [VERIFIED $20]!\n";

        tm.commit(tx2);
        tm.commit(tx4);

        // =====================================================================
        // TEST 2: Index Lookups across Large Multi-Page Heap (500 items)
        // =====================================================================
        std::cout << "\n[Test 2] Populating 500 items across multiple pages..." << std::endl;
        pg::tx_id_t bulk_tx = tm.begin_transaction();
        for (int i = 1; i <= 500; ++i) {
            int32_t item_id = 1000 + i;
            int32_t price = i * 2;
            pg::CTID ctid = heap->insert({item_id, price}, bulk_tx);
            index.insert_entry(item_id, ctid);
        }
        tm.commit(bulk_tx);

        std::cout << " -> Total Heap Pages: " << heap->num_pages() 
                  << " | Total Index Entries: " << index.num_entries() << std::endl;
        assert(heap->num_pages() >= 2);

        // Direct Point Lookup via Index for item_id = 1350
        pg::tx_id_t query_tx = tm.begin_transaction();
        pg::Snapshot snap_query = tm.take_snapshot(query_tx);

        auto lookup_res = pg::index_lookup(index, *heap, 1350, snap_query, tm);
        assert(lookup_res.has_value());
        assert(lookup_res->second.data.item_id == 1350);
        assert(lookup_res->second.data.price == 700); // 350 * 2
        std::cout << " -> Successfully fetched item 1350 at CTID " 
                  << lookup_res->first.to_string() << " (Price = $" 
                  << lookup_res->second.data.price << ") via Index!\n";

        // Non-existent key lookup
        auto missing_res = pg::index_lookup(index, *heap, 9999, snap_query, tm);
        assert(!missing_res.has_value());
        std::cout << " -> Non-existent key 9999 correctly returned nullopt.\n";

        // =====================================================================
        // TEST 3: Index Range Scan
        // =====================================================================
        std::cout << "\n[Test 3] Testing Index Range Scan [1100 .. 1105]..." << std::endl;
        auto range_results = index.range_scan(1100, 1105);
        assert(range_results.size() == 6);
        for (const auto& [k, ctid] : range_results) {
            std::cout << "   Key [" << k << "] -> CTID " << ctid.to_string() << std::endl;
        }
        tm.commit(query_tx);
    }

    fs::remove(db_name);
    std::cout << "\n>>> ITEM 6 (B-TREE INDEX & KEY->CTID MAPPING) TESTS PASSED SUCCESSFULLY! <<<\n" << std::endl;
}

int main() {
    try {
        run_index_tests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Index test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
