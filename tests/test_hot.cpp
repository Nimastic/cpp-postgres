#include "pg/heap.h"
#include "pg/tx.h"
#include "pg/mvcc.h"
#include "pg/btree.h"
#include <iostream>
#include <vector>
#include <cassert>
#include <filesystem>

namespace fs = std::filesystem;

void run_hot_tests() {
    const std::string db_name = "test_hot.db";
    if (fs::exists(db_name)) {
        fs::remove(db_name);
    }

    {
        pg::TransactionManager tm;
        pg::BTreeIndex idx_item_id; // Index on item_id (the indexed column)
        auto heap = pg::HeapFile::open(db_name);

        std::cout << "\n--- REPRODUCING HOT (HEAP-ONLY TUPLES) ---" << std::endl;

        // =====================================================================
        // STEP 1: Insert (100, $10) and register in index
        // =====================================================================
        std::cout << "[Step 1] Tx 1: INSERT (100, $10)..." << std::endl;
        pg::tx_id_t tx1 = tm.begin_transaction();
        pg::CTID ctid1 = heap->insert({100, 10}, tx1);
        idx_item_id.insert_entry(100, ctid1);
        tm.commit(tx1);
        assert(ctid1 == pg::CTID(0, 1));
        std::cout << " -> Inserted at " << ctid1.to_string() 
                  << ". Index has " << idx_item_id.num_entries() << " entry.\n";

        // =====================================================================
        // STEP 2: Tx 2 starts and takes snapshot (long-running)
        // =====================================================================
        std::cout << "[Step 2] Tx 2: BEGIN (long-running snapshot)..." << std::endl;
        pg::tx_id_t tx2 = tm.begin_transaction();
        pg::Snapshot snap2 = tm.take_snapshot(tx2);

        // =====================================================================
        // STEP 3: HOT UPDATE - price changes from $10 to $20 (item_id unchanged!)
        // =====================================================================
        std::cout << "[Step 3] Tx 3: HOT UPDATE price $10 -> $20 (item_id unchanged!)..." << std::endl;
        pg::tx_id_t tx3 = tm.begin_transaction();
        // item_id = 100 is unchanged -> eligible for HOT
        auto hot_result = heap->hot_update(ctid1, {100, 20}, tx3);
        assert(hot_result.has_value());
        pg::CTID ctid2 = *hot_result;
        tm.commit(tx3);
        assert(ctid2.page == 0); // MUST be on same page!
        assert(ctid2 == pg::CTID(0, 2));

        // CRITICAL: NO index write! Index still has exactly 1 entry!
        std::cout << " -> HOT update placed new version at " << ctid2.to_string() 
                  << " on SAME page.\n";
        std::cout << " -> Index entries: " << idx_item_id.num_entries() 
                  << " (ZERO index writes!)\n";
        assert(idx_item_id.num_entries() == 1);

        // =====================================================================
        // STEP 4: Verify physical page state
        // =====================================================================
        std::cout << "\n[Step 4] Inspecting physical page state..." << std::endl;
        auto t1 = heap->get(ctid1);
        auto t2 = heap->get(ctid2);
        assert(t1.has_value() && t2.has_value());

        std::cout << "   Slot 1: xmin=" << t1->header.xmin 
                  << ", xmax=" << t1->header.xmax
                  << ", price=$" << t1->data.price
                  << ", t_ctid=" << t1->header.t_ctid.to_string()
                  << ", HOT_UPDATED=" << ((t1->header.infomask & pg::HEAP_HOT_UPDATED) ? "YES" : "NO")
                  << std::endl;
        std::cout << "   Slot 2: xmin=" << t2->header.xmin 
                  << ", xmax=" << t2->header.xmax
                  << ", price=$" << t2->data.price
                  << ", t_ctid=" << t2->header.t_ctid.to_string()
                  << ", HEAP_ONLY=" << ((t2->header.infomask & pg::HEAP_ONLY_TUPLE) ? "YES" : "NO")
                  << std::endl;

        assert(t1->header.infomask & pg::HEAP_HOT_UPDATED);
        assert(t2->header.infomask & pg::HEAP_ONLY_TUPLE);
        assert(t1->header.t_ctid == ctid2); // Chain: Slot 1 -> Slot 2

        // =====================================================================
        // STEP 5: Index lookup follows HOT chain correctly
        // =====================================================================
        std::cout << "\n[Step 5] Testing HOT-aware index lookups..." << std::endl;

        // Tx 2 (old snapshot) -> should follow chain and find $10 visible
        auto res_tx2 = pg::index_lookup(idx_item_id, *heap, 100, snap2, tm);
        assert(res_tx2.has_value());
        assert(res_tx2->second.data.price == 10);
        std::cout << " -> Tx 2 (Old Snapshot): index_lookup(100) -> Price = $" 
                  << res_tx2->second.data.price << " [VERIFIED $10]!\n";

        // Tx 4 (new snapshot after HOT commit) -> should follow chain and find $20 visible
        pg::tx_id_t tx4 = tm.begin_transaction();
        pg::Snapshot snap4 = tm.take_snapshot(tx4);
        auto res_tx4 = pg::index_lookup(idx_item_id, *heap, 100, snap4, tm);
        assert(res_tx4.has_value());
        assert(res_tx4->second.data.price == 20);
        std::cout << " -> Tx 4 (New Snapshot): index_lookup(100) -> Price = $" 
                  << res_tx4->second.data.price << " [VERIFIED $20]!\n";

        tm.commit(tx2);
        tm.commit(tx4);

        // =====================================================================
        // TEST 2: Multi-hop HOT chain ($10 -> $20 -> $30)
        // =====================================================================
        std::cout << "\n[Test 2] Multi-hop HOT chain: $20 -> $30..." << std::endl;
        pg::tx_id_t tx5 = tm.begin_transaction();
        auto hot_result2 = heap->hot_update(ctid2, {100, 30}, tx5);
        assert(hot_result2.has_value());
        pg::CTID ctid3 = *hot_result2;
        tm.commit(tx5);
        assert(ctid3.page == 0); // Still same page!
        assert(idx_item_id.num_entries() == 1); // STILL only 1 index entry!

        pg::tx_id_t tx6 = tm.begin_transaction();
        pg::Snapshot snap6 = tm.take_snapshot(tx6);
        auto res_chain = pg::index_lookup(idx_item_id, *heap, 100, snap6, tm);
        assert(res_chain.has_value());
        assert(res_chain->second.data.price == 30);
        std::cout << " -> 3-hop HOT chain: index_lookup(100) -> Price = $" 
                  << res_chain->second.data.price << " with ONLY 1 index entry!\n";
        tm.commit(tx6);

        // =====================================================================
        // TEST 3: Verify write amplification savings
        // =====================================================================
        std::cout << "\n[Test 3] Write amplification comparison..." << std::endl;
        std::cout << " -> Without HOT: 3 versions = 3 index entries (one per CTID).\n";
        std::cout << " -> With HOT:    3 versions = " << idx_item_id.num_entries() 
                  << " index entry (ZERO extra index writes)!\n";
        assert(idx_item_id.num_entries() == 1);
    }

    fs::remove(db_name);
    std::cout << "\n>>> ITEM 7 (HEAP-ONLY TUPLES / HOT) TESTS PASSED SUCCESSFULLY! <<<\n" << std::endl;
}

int main() {
    try {
        run_hot_tests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "HOT test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
