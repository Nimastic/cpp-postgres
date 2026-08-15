#include "pg/heap.h"
#include "pg/tx.h"
#include "pg/mvcc.h"
#include <iostream>
#include <vector>
#include <cassert>
#include <filesystem>

namespace fs = std::filesystem;

void run_mvcc_tests() {
    const std::string db_name = "test_mvcc.db";
    if (fs::exists(db_name)) {
        fs::remove(db_name);
    }

    {
        pg::TransactionManager tm;
        auto heap = pg::HeapFile::open(db_name);

        // =========================================================================
        // THE EXACT LECTURE SCENARIO:
        // Tx1 inserts (100, $10) and commits.
        // Tx2 starts and freezes its snapshot.
        // Tx3 updates item 100 to $20 and commits.
        // Tx4 starts and takes a new snapshot.
        // Tx2 MUST see $10. Tx4 MUST see $20.
        // =========================================================================
        std::cout << "\n--- REPRODUCING HUSSEIN NASSER'S MVCC LECTURE SCENARIO ---" << std::endl;

        std::cout << "[Step 1] Tx 1: Inserting (100, $10) and committing..." << std::endl;
        pg::tx_id_t tx1 = tm.begin_transaction();
        pg::CTID ctid_v1 = heap->insert({100, 10}, tx1);
        tm.commit(tx1);
        std::cout << " -> Tx 1 (id=" << tx1 << ") inserted item 100 at CTID " 
                  << ctid_v1.to_string() << " and COMMITTED.\n";
        assert(ctid_v1 == pg::CTID(0, 1));

        std::cout << "[Step 2] Tx 2: Starting long-running transaction and taking snapshot..." << std::endl;
        pg::tx_id_t tx2 = tm.begin_transaction();
        pg::Snapshot snap2 = tm.take_snapshot(tx2);
        std::cout << " -> Tx 2 (id=" << tx2 << ") snapshot taken: [xmin=" 
                  << snap2.xmin << ", xmax=" << snap2.xmax << "].\n";

        std::cout << "[Step 3] Tx 3: Updating item 100 from $10 to $20 and committing..." << std::endl;
        pg::tx_id_t tx3 = tm.begin_transaction();
        pg::CTID ctid_v2 = heap->update(ctid_v1, {100, 20}, tx3);
        tm.commit(tx3);
        std::cout << " -> Tx 3 (id=" << tx3 << ") updated CTID " << ctid_v1.to_string() 
                  << " -> new CTID " << ctid_v2.to_string() << " and COMMITTED.\n";
        assert(ctid_v2 == pg::CTID(0, 2));

        std::cout << "[Step 4] Tx 4: Starting new transaction after commit..." << std::endl;
        pg::tx_id_t tx4 = tm.begin_transaction();
        pg::Snapshot snap4 = tm.take_snapshot(tx4);
        std::cout << " -> Tx 4 (id=" << tx4 << ") snapshot taken: [xmin=" 
                  << snap4.xmin << ", xmax=" << snap4.xmax << "].\n";

        std::cout << "\n[Step 5] Verifying Physical On-Disk Heap State..." << std::endl;
        auto physical_tuples = heap->seq_scan();
        std::cout << " -> Total physical tuples on disk: " << physical_tuples.size() << std::endl;
        assert(physical_tuples.size() == 2);

        auto tuple_v1 = heap->get(ctid_v1);
        auto tuple_v2 = heap->get(ctid_v2);
        assert(tuple_v1.has_value() && tuple_v2.has_value());

        std::cout << "   Slot 1: xmin=" << tuple_v1->header.xmin 
                  << ", xmax=" << tuple_v1->header.xmax 
                  << ", price=$" << tuple_v1->data.price 
                  << ", t_ctid=" << tuple_v1->header.t_ctid.to_string() << std::endl;

        std::cout << "   Slot 2: xmin=" << tuple_v2->header.xmin 
                  << ", xmax=" << tuple_v2->header.xmax 
                  << ", price=$" << tuple_v2->data.price 
                  << ", t_ctid=" << tuple_v2->header.t_ctid.to_string() << std::endl;

        assert(tuple_v1->header.xmin == tx1 && tuple_v1->header.xmax == tx3);
        assert(tuple_v2->header.xmin == tx3 && tuple_v2->header.xmax == 0);

        std::cout << "\n[Step 6] Verifying MVCC Visibility Across Snapshots..." << std::endl;
        
        // Long-running Tx 2 query
        auto visible_tx2 = heap->seq_scan(snap2, tm);
        assert(visible_tx2.size() == 1);
        assert(visible_tx2[0].second.data.item_id == 100);
        assert(visible_tx2[0].second.data.price == 10);
        std::cout << " -> Tx 2 (Old Snapshot) sees exactly 1 row: Price = $" 
                  << visible_tx2[0].second.data.price << " [VERIFIED $10]!\n";

        // New Tx 4 query
        auto visible_tx4 = heap->seq_scan(snap4, tm);
        assert(visible_tx4.size() == 1);
        assert(visible_tx4[0].second.data.item_id == 100);
        assert(visible_tx4[0].second.data.price == 20);
        std::cout << " -> Tx 4 (New Snapshot) sees exactly 1 row: Price = $" 
                  << visible_tx4[0].second.data.price << " [VERIFIED $20]!\n";

        tm.commit(tx2);
        tm.commit(tx4);

        // =========================================================================
        // TEST: ABORTED TRANSACTION ROLLBACK VISIBILITY
        // =========================================================================
        std::cout << "\n[Test 2] Verifying Aborted Transaction Rollback..." << std::endl;
        pg::tx_id_t tx5 = tm.begin_transaction();
        pg::CTID ctid_aborted = heap->insert({999, 999}, tx5);
        tm.abort(tx5); // Abort tx5!
        std::cout << " -> Tx 5 inserted item 999 and was ABORTED.\n";

        pg::tx_id_t tx6 = tm.begin_transaction();
        pg::Snapshot snap6 = tm.take_snapshot(tx6);
        auto visible_tx6 = heap->seq_scan(snap6, tm);

        for (const auto& [ctid, t] : visible_tx6) {
            assert(t.data.item_id != 999); // Aborted row MUST NOT be visible
        }
        std::cout << " -> Aborted transaction insert is completely invisible to subsequent snapshots!\n";
        tm.commit(tx6);

        // =========================================================================
        // TEST: UNCOMMITTED CONCURRENT TRANSACTION VISIBILITY
        // =========================================================================
        std::cout << "\n[Test 3] Verifying Uncommitted Transaction Isolation..." << std::endl;
        pg::tx_id_t tx7 = tm.begin_transaction();
        pg::CTID ctid_uncommitted = heap->insert({888, 888}, tx7); // In-progress, uncommitted

        pg::tx_id_t tx8 = tm.begin_transaction();
        pg::Snapshot snap8 = tm.take_snapshot(tx8);
        auto visible_tx8 = heap->seq_scan(snap8, tm);

        for (const auto& [ctid, t] : visible_tx8) {
            assert(t.data.item_id != 888); // Uncommitted row MUST NOT be visible to other transactions
        }
        std::cout << " -> In-progress uncommitted row is invisible to concurrent transactions!\n";

        tm.commit(tx7);
        pg::tx_id_t tx9 = tm.begin_transaction();
        pg::Snapshot snap9 = tm.take_snapshot(tx9);
        auto visible_tx9 = heap->seq_scan(snap9, tm);
        bool found_888 = false;
        for (const auto& [ctid, t] : visible_tx9) {
            if (t.data.item_id == 888) found_888 = true;
        }
        assert(found_888);
        std::cout << " -> After commit, row 888 becomes visible to new transactions.\n";
        tm.commit(tx8);
        tm.commit(tx9);
    }

    fs::remove(db_name);
    std::cout << "\n>>> ITEM 4 (MVCC & TRANSACTION VISIBILITY) TESTS PASSED SUCCESSFULLY! <<<\n" << std::endl;
}

int main() {
    try {
        run_mvcc_tests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "MVCC test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
