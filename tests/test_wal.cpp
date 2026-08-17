#include "pg/heap.h"
#include "pg/tx.h"
#include "pg/mvcc.h"
#include "pg/wal.h"
#include <iostream>
#include <vector>
#include <cassert>
#include <filesystem>

namespace fs = std::filesystem;

void run_wal_tests() {
    const std::string db_name = "test_wal_heap.db";
    const std::string wal_name = "test_wal.log";

    if (fs::exists(db_name)) fs::remove(db_name);
    if (fs::exists(wal_name)) fs::remove(wal_name);

    pg::tx_id_t tx1_id, tx2_id, tx3_id;

    // =========================================================================
    // STEP 1: Execute Transactions, Log to WAL, and Flush
    // =========================================================================
    std::cout << "\n--- REPRODUCING POSTGRESQL WAL LOGGING & CRASH RECOVERY ---" << std::endl;
    {
        pg::TransactionManager tm;
        auto heap = pg::HeapFile::open(db_name);
        auto wal = pg::WALManager::open(wal_name);

        std::cout << "[Step 1] Tx 1: Inserting (100, $10), logging to WAL, and committing..." << std::endl;
        tx1_id = tm.begin_transaction();
        pg::CTID ctid1 = heap->insert({100, 10}, tx1_id);
        wal->log_insert(tx1_id, ctid1.page, ctid1.slot, *heap->get(ctid1));
        wal->log_commit(tx1_id);
        tm.commit(tx1_id);
        assert(ctid1 == pg::CTID(0, 1));
        std::cout << " -> Tx 1 committed at LSN: " << wal->flushed_lsn() << std::endl;

        std::cout << "[Step 2] Tx 2: Updating item 100 to $20, logging to WAL, and committing..." << std::endl;
        tx2_id = tm.begin_transaction();
        pg::CTID ctid2 = heap->update(ctid1, {100, 20}, tx2_id);
        wal->log_update(tx2_id, ctid1, ctid2, *heap->get(ctid2));
        wal->log_commit(tx2_id);
        tm.commit(tx2_id);
        assert(ctid2 == pg::CTID(0, 2));
        std::cout << " -> Tx 2 committed at LSN: " << wal->flushed_lsn() << std::endl;

        std::cout << "[Step 3] Tx 3: Inserting (999, $999), logging to WAL, and ABORTING..." << std::endl;
        tx3_id = tm.begin_transaction();
        pg::CTID ctid3 = heap->insert({999, 999}, tx3_id);
        wal->log_insert(tx3_id, ctid3.page, ctid3.slot, *heap->get(ctid3));
        wal->log_abort(tx3_id);
        tm.abort(tx3_id);
        std::cout << " -> Tx 3 aborted at LSN: " << wal->flushed_lsn() << std::endl;

        wal->flush();
    }

    // =========================================================================
    // STEP 2: SIMULATE HARD CRASH (Wipe on-disk heap file completely!)
    // =========================================================================
    std::cout << "\n[Step 4] SIMULATING POWER OUTAGE / HARD CRASH..." << std::endl;
    fs::remove(db_name); // Heap table file destroyed!
    assert(!fs::exists(db_name));
    assert(fs::exists(wal_name)); // WAL file survived on durable disk!
    std::cout << " -> Table database file destroyed. WAL file intact.\n";

    // =========================================================================
    // STEP 3: POST-CRASH STARTUP & REDO REPLAY RECOVERY
    // =========================================================================
    std::cout << "\n[Step 5] Restarting database engine and running WAL REDO recovery..." << std::endl;
    {
        pg::TransactionManager recovered_tm;
        auto recovered_heap = pg::HeapFile::open(db_name); // Starts blank with fresh Page 0
        auto wal = pg::WALManager::open(wal_name);

        size_t replayed = wal->recover(*recovered_heap, recovered_tm);
        std::cout << " -> Replayed " << replayed << " log records during Redo recovery.\n";
        assert(replayed >= 2);

        // Verify transaction states were restored
        assert(recovered_tm.get_status(tx1_id) == pg::TransactionStatus::COMMITTED);
        assert(recovered_tm.get_status(tx2_id) == pg::TransactionStatus::COMMITTED);
        assert(recovered_tm.get_status(tx3_id) == pg::TransactionStatus::ABORTED);
        std::cout << " -> Transaction status log reconstructed perfectly!\n";

        // =====================================================================
        // STEP 4: VERIFY RECOVERED PHYSICAL AND MVCC STATE
        // =====================================================================
        std::cout << "\n[Step 6] Inspecting recovered heap and MVCC visibility..." << std::endl;

        auto t1 = recovered_heap->get(pg::CTID(0, 1));
        auto t2 = recovered_heap->get(pg::CTID(0, 2));
        assert(t1.has_value() && t2.has_value());

        std::cout << "   Slot 1: xmin=" << t1->header.xmin 
                  << ", xmax=" << t1->header.xmax 
                  << ", price=$" << t1->data.price << std::endl;
        std::cout << "   Slot 2: xmin=" << t2->header.xmin 
                  << ", xmax=" << t2->header.xmax 
                  << ", price=$" << t2->data.price << std::endl;

        assert(t1->header.xmin == tx1_id && t1->header.xmax == tx2_id && t1->data.price == 10);
        assert(t2->header.xmin == tx2_id && t2->header.xmax == 0 && t2->data.price == 20);

        // Query with new snapshot
        pg::tx_id_t query_tx = recovered_tm.begin_transaction();
        pg::Snapshot snap = recovered_tm.take_snapshot(query_tx);
        auto visible = recovered_heap->seq_scan(snap, recovered_tm);

        assert(visible.size() == 1);
        assert(visible[0].second.data.item_id == 100);
        assert(visible[0].second.data.price == 20); // Sees latest committed version!

        std::cout << " -> MVCC Scan on recovered database returns: item 100 with Price = $" 
                  << visible[0].second.data.price << " [VERIFIED $20]!\n";
        std::cout << " -> Aborted transaction row (999) is completely absent/invisible.\n";
    }

    fs::remove(db_name);
    fs::remove(wal_name);
    std::cout << "\n>>> ITEM 9 (WRITE-AHEAD LOGGING & REDO RECOVERY) TESTS PASSED SUCCESSFULLY! <<<\n" << std::endl;
}

int main() {
    try {
        run_wal_tests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "WAL test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
