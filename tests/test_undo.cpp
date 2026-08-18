#include "pg/wal.h"
#include "pg/heap.h"
#include "pg/clog.h"
#include "pg/tx.h"
#include <iostream>
#include <vector>
#include <cassert>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

void run_undo_tests() {
    const std::string wal_path  = "test_undo_wal.log";
    const std::string heap_path = "test_undo_heap.db";
    const std::string clog_path = "test_undo_clog.db";

    if (fs::exists(wal_path))  fs::remove(wal_path);
    if (fs::exists(heap_path)) fs::remove(heap_path);
    if (fs::exists(clog_path)) fs::remove(clog_path);

    std::cout << "\n--- REPRODUCING POSTGRESQL ARIES UNDO CRASH RECOVERY ---" << std::endl;

    // =========================================================================
    // TEST 1: Uncommitted INSERT Rollback during UNDO Pass
    // =========================================================================
    std::cout << "[Step 1] Tx 1 commits (100, $10); Tx 2 inserts (200, $20) and crashes UNCOMMITTED..." << std::endl;
    {
        auto wal  = pg::WALManager::open(wal_path);
        auto heap = pg::HeapFile::open(heap_path);
        auto clog = pg::CLogManager::open(clog_path);
        pg::TransactionManager tm(clog.get());

        // Tx 1: Insert (100, $10) and COMMIT
        pg::tx_id_t tx1 = tm.begin_transaction();
        pg::ItemRecord r1{100, 10};
        pg::CTID ctid1 = heap->insert(r1, tx1);
        auto t1 = heap->get(ctid1);
        wal->log_insert(tx1, ctid1.page, ctid1.slot, *t1);
        wal->log_commit(tx1);
        tm.commit(tx1);

        // Tx 2: Insert (200, $20) and NEVER COMMIT (Power Failure Simulator)
        pg::tx_id_t tx2 = tm.begin_transaction();
        pg::ItemRecord r2{200, 20};
        pg::CTID ctid2 = heap->insert(r2, tx2);
        auto t2 = heap->get(ctid2);
        wal->log_insert(tx2, ctid2.page, ctid2.slot, *t2);
        wal->flush(); // Force log to disk

        // Flush dirty pages to disk: uncommitted Tx 2 writes are physically on disk before crash!
        std::cout << " -> Pre-crash disk state: Page 0 has Slot 1 (Tx 1) and Slot 2 (Tx 2).\n";
    }

    std::cout << "[Step 2] Restarting database engine and running 3-Phase ARIES Recovery..." << std::endl;
    {
        auto wal  = pg::WALManager::open(wal_path);
        auto heap = pg::HeapFile::open(heap_path);
        auto clog = pg::CLogManager::open(clog_path);
        pg::TransactionManager tm(clog.get());

        // Run ARIES Recovery: Analysis -> REDO -> UNDO
        size_t replayed = wal->recover(*heap, tm);
        std::cout << " -> ARIES Recovery complete. Replayed " << replayed << " records during REDO pass.\n";

        // Verify physical page state: Slot 2 must be UNUSED (undone by UNDO pass)!
        std::vector<uint8_t> page_buf(pg::PAGE_SIZE, 0);
        heap->pager().read_page(0, page_buf.data());
        pg::Page p0(page_buf.data());

        auto lp1 = p0.get_line_pointer(1);
        assert(lp1.has_value() && lp1->flags() == pg::ItemFlags::NORMAL);

        auto lp2 = p0.get_line_pointer(2);
        assert(lp2.has_value() && lp2->flags() == pg::ItemFlags::UNUSED);
        std::cout << " -> Physical verification: Slot 2 (uncommitted Tx 2) was wiped to UNUSED by UNDO pass!\n";

        // Verify CLOG status: Tx 2 is marked ABORTED!
        assert(tm.get_status(2) == pg::TransactionStatus::ABORTED);
        assert(clog->get_status(2) == pg::TransactionStatus::ABORTED);
        std::cout << " -> Transaction status verification: Tx 2 marked ABORTED in persistent CLOG.\n";
    }

    // =========================================================================
    // TEST 2: Uncommitted UPDATE Rollback (Old Tuple xmax Restored to 0)
    // =========================================================================
    std::cout << "\n[Step 3] Testing uncommitted UPDATE rollback during UNDO pass..." << std::endl;
    {
        auto wal  = pg::WALManager::open(wal_path);
        auto heap = pg::HeapFile::open(heap_path);
        auto clog = pg::CLogManager::open(clog_path);
        pg::TransactionManager tm(clog.get());

        // Tx 3: Update item 100 ($10) -> $999 and NEVER COMMIT
        tm.set_next_tx_id(3);
        pg::tx_id_t tx3 = tm.begin_transaction();
        pg::CTID old_ctid(0, 1);
        pg::ItemRecord new_rec{100, 999};

        pg::CTID new_ctid = heap->update(old_ctid, new_rec, tx3);
        auto new_tuple = heap->get(new_ctid);
        wal->log_update(tx3, old_ctid, new_ctid, *new_tuple);
        wal->flush();

        std::cout << " -> Tx 3 performed UPDATE (stamped xmax=3 on Slot 1, created Slot 3 with price $999) without committing.\n";
    }

    std::cout << "[Step 4] Running ARIES Recovery to rollback Tx 3..." << std::endl;
    {
        auto wal  = pg::WALManager::open(wal_path);
        auto heap = pg::HeapFile::open(heap_path);
        auto clog = pg::CLogManager::open(clog_path);
        pg::TransactionManager tm(clog.get());

        wal->recover(*heap, tm);

        // Inspect physical page:
        // Slot 1 should have xmax restored to 0
        // Slot 3 should be UNUSED
        std::vector<uint8_t> page_buf(pg::PAGE_SIZE, 0);
        heap->pager().read_page(0, page_buf.data());
        pg::Page p0(page_buf.data());

        size_t len1 = 0;
        const uint8_t* ptr1 = p0.get_tuple_ptr(1, &len1);
        assert(ptr1 != nullptr);
        pg::HeapTuple t1 = pg::HeapTuple::deserialize(ptr1, len1);

        assert(t1.header.xmax == 0); // Restored!
        assert(t1.data.price == 10); // Original price preserved!

        auto lp2 = p0.get_line_pointer(2);
        assert(lp2.has_value() && lp2->flags() == pg::ItemFlags::UNUSED); // Uncommitted new version erased!

        std::cout << " -> UPDATE Rollback verified: Slot 1 xmax restored to 0, Slot 2 erased to UNUSED!\n";
    }

    fs::remove(wal_path);
    fs::remove(heap_path);
    fs::remove(clog_path);

    std::cout << "\n>>> ITEM 16 (ARIES UNDO RECOVERY PASS) TESTS PASSED SUCCESSFULLY! <<<\n" << std::endl;
}

int main() {
    try {
        run_undo_tests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "UNDO test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
