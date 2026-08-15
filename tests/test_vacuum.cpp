#include "pg/heap.h"
#include "pg/tx.h"
#include "pg/vacuum.h"
#include <iostream>
#include <vector>
#include <cassert>
#include <filesystem>

namespace fs = std::filesystem;

void run_vacuum_tests() {
    const std::string db_name = "test_vacuum.db";
    if (fs::exists(db_name)) {
        fs::remove(db_name);
    }

    {
        pg::TransactionManager tm;
        auto heap = pg::HeapFile::open(db_name);

        std::cout << "\n--- REPRODUCING POSTGRESQL VACUUM & BLOAT PREVENTION ---" << std::endl;

        std::cout << "[Step 1] Tx 1: Inserting (100, $10) and committing..." << std::endl;
        pg::tx_id_t tx1 = tm.begin_transaction();
        pg::CTID ctid1 = heap->insert({100, 10}, tx1);
        tm.commit(tx1);
        assert(ctid1 == pg::CTID(0, 1));

        std::cout << "[Step 2] Tx 2: Starting long-running transaction (Snapshot Pinning)..." << std::endl;
        pg::tx_id_t tx2 = tm.begin_transaction();
        pg::Snapshot snap2 = tm.take_snapshot(tx2);
        std::cout << " -> Tx 2 active. tm.oldest_active_xmin() = " << tm.oldest_active_xmin() << std::endl;
        assert(tm.oldest_active_xmin() == tx2);

        std::cout << "[Step 3] Tx 3: Updating item 100 to $20 and committing..." << std::endl;
        pg::tx_id_t tx3 = tm.begin_transaction();
        pg::CTID ctid2 = heap->update(ctid1, {100, 20}, tx3);
        tm.commit(tx3);
        assert(ctid2 == pg::CTID(0, 2));

        // Physical state: Slot 1 has $10 (xmax=3), Slot 2 has $20 (xmax=0)
        auto raw_tuples_before = heap->seq_scan();
        assert(raw_tuples_before.size() == 2);
        std::cout << " -> Page 0 currently has 2 physical tuples.\n";

        std::cout << "\n[Step 4] Running VACUUM while Tx 2 is still open..." << std::endl;
        auto stats1 = pg::Vacuum::run(*heap, tm);
        std::cout << " -> Vacuum scanned " << stats1.pages_scanned 
                  << " pages, reclaimed " << stats1.dead_tuples_reclaimed << " dead tuples.\n";
        assert(stats1.dead_tuples_reclaimed == 0); // MUST NOT reclaim because Tx 2 pins the snapshot!

        auto snap2_check = heap->seq_scan(snap2, tm);
        assert(snap2_check.size() == 1 && snap2_check[0].second.data.price == 10);
        std::cout << " -> Verified: Slot 1 ($10) was safely preserved for Tx 2!\n";

        std::cout << "\n[Step 5] Committing Tx 2 and running VACUUM again..." << std::endl;
        tm.commit(tx2);
        std::cout << " -> Tx 2 committed. tm.oldest_active_xmin() advanced to " 
                  << tm.oldest_active_xmin() << std::endl;

        auto stats2 = pg::Vacuum::run(*heap, tm);
        std::cout << " -> Vacuum scanned " << stats2.pages_scanned 
                  << " pages, reclaimed " << stats2.dead_tuples_reclaimed 
                  << " dead tuples (" << stats2.bytes_reclaimed << " bytes).\n";
        assert(stats2.dead_tuples_reclaimed == 1);
        assert(stats2.bytes_reclaimed == sizeof(pg::HeapTuple));

        // Physical state: Slot 1 is marked DEAD, Slot 2 is compacted
        auto raw_tuples_after_vacuum = heap->seq_scan();
        assert(raw_tuples_after_vacuum.size() == 1); // Only Slot 2 is live!
        assert(raw_tuples_after_vacuum[0].first == pg::CTID(0, 2));
        assert(raw_tuples_after_vacuum[0].second.data.price == 20);
        std::cout << " -> Verified: Slot 1 dead tuple was reclaimed and Slot 2 remains intact!\n";

        std::cout << "\n[Step 6] Inserting a new row (300, $99) and verifying hole reuse..." << std::endl;
        pg::tx_id_t tx4 = tm.begin_transaction();
        pg::CTID ctid3 = heap->insert({300, 99}, tx4);
        tm.commit(tx4);

        std::cout << " -> New row (300, $99) landed at CTID: " << ctid3.to_string() << std::endl;
        assert(ctid3 == pg::CTID(0, 1)); // Successfully reused Slot 1 hole on Page 0!
        assert(heap->num_pages() == 1);  // File did not expand!

        pg::tx_id_t tx5 = tm.begin_transaction();
        pg::Snapshot snap5 = tm.take_snapshot(tx5);
        auto final_scan = heap->seq_scan(snap5, tm);
        assert(final_scan.size() == 2);
        std::cout << " -> Final table contains " << final_scan.size() << " live rows:\n";
        for (const auto& [ctid, t] : final_scan) {
            std::cout << "    CTID " << ctid.to_string() 
                      << " -> item_id=" << t.data.item_id 
                      << ", price=$" << t.data.price << std::endl;
        }
        tm.commit(tx5);

        // =====================================================================
        // TEST: ABORTED TUPLE CLEANUP
        // =====================================================================
        std::cout << "\n[Test 2] Inserting in aborted transaction and running VACUUM..." << std::endl;
        pg::tx_id_t tx6 = tm.begin_transaction();
        pg::CTID aborted_ctid = heap->insert({500, 500}, tx6);
        tm.abort(tx6);
        std::cout << " -> Tx 6 inserted at " << aborted_ctid.to_string() << " and ABORTED.\n";

        auto stats_abort = pg::Vacuum::run(*heap, tm);
        assert(stats_abort.dead_tuples_reclaimed == 1);
        std::cout << " -> VACUUM successfully identified and purged aborted transaction tuple!\n";
    }

    fs::remove(db_name);
    std::cout << "\n>>> ITEM 5 (VACUUM & GARBAGE COLLECTION) TESTS PASSED SUCCESSFULLY! <<<\n" << std::endl;
}

int main() {
    try {
        run_vacuum_tests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Vacuum test failed: " << e.what() << std::endl;
        return 1;
    }
}
