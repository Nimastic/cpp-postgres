#include "pg/clog.h"
#include "pg/tx.h"
#include "pg/engine.h"
#include <iostream>
#include <cassert>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

void run_clog_tests() {
    const std::string clog_db   = "test_clog_raw.db";
    const std::string db_prefix = "test_clog_engine";

    if (fs::exists(clog_db)) fs::remove(clog_db);
    if (fs::exists(db_prefix + "_heap.db"))  fs::remove(db_prefix + "_heap.db");
    if (fs::exists(db_prefix + "_wal.log"))  fs::remove(db_prefix + "_wal.log");
    if (fs::exists(db_prefix + "_toast.db")) fs::remove(db_prefix + "_toast.db");
    if (fs::exists(db_prefix + "_clog.db"))  fs::remove(db_prefix + "_clog.db");

    std::cout << "\n--- REPRODUCING POSTGRESQL CLOG (COMMIT STATUS BITMAP ON DISK) ---" << std::endl;

    // =========================================================================
    // TEST 1: 2-bit Status Bitmap Manipulation & On-Disk Persistence
    // =========================================================================
    std::cout << "[Step 1] Setting 2-bit statuses for Transactions 1..8..." << std::endl;
    {
        auto clog = pg::CLogManager::open(clog_db);

        clog->set_status(1, pg::TransactionStatus::COMMITTED);    // 0b01
        clog->set_status(2, pg::TransactionStatus::ABORTED);      // 0b10
        clog->set_status(3, pg::TransactionStatus::COMMITTED);    // 0b01
        clog->set_status(4, pg::TransactionStatus::IN_PROGRESS);  // 0b00
        clog->set_status(5, pg::TransactionStatus::ABORTED);      // 0b10
        clog->set_status(6, pg::TransactionStatus::COMMITTED);    // 0b01
        clog->set_status(7, pg::TransactionStatus::SUB_COMMITTED);// 0b11
        clog->set_status(8, pg::TransactionStatus::COMMITTED);    // 0b01

        assert(clog->get_status(1) == pg::TransactionStatus::COMMITTED);
        assert(clog->get_status(2) == pg::TransactionStatus::ABORTED);
        assert(clog->get_status(3) == pg::TransactionStatus::COMMITTED);
        assert(clog->get_status(4) == pg::TransactionStatus::IN_PROGRESS);
        assert(clog->get_status(5) == pg::TransactionStatus::ABORTED);
        assert(clog->get_status(6) == pg::TransactionStatus::COMMITTED);
        assert(clog->get_status(7) == pg::TransactionStatus::SUB_COMMITTED);
        assert(clog->get_status(8) == pg::TransactionStatus::COMMITTED);
        std::cout << " -> All 8 transaction statuses verified in memory.\n";
    }

    std::cout << "[Step 2] Reopening CLOG from disk to verify binary persistence..." << std::endl;
    {
        auto clog = pg::CLogManager::open(clog_db);

        assert(clog->get_status(1) == pg::TransactionStatus::COMMITTED);
        assert(clog->get_status(2) == pg::TransactionStatus::ABORTED);
        assert(clog->get_status(3) == pg::TransactionStatus::COMMITTED);
        assert(clog->get_status(4) == pg::TransactionStatus::IN_PROGRESS);
        assert(clog->get_status(5) == pg::TransactionStatus::ABORTED);
        assert(clog->get_status(6) == pg::TransactionStatus::COMMITTED);
        assert(clog->get_status(7) == pg::TransactionStatus::SUB_COMMITTED);
        assert(clog->get_status(8) == pg::TransactionStatus::COMMITTED);

        // Uninitialized transaction ID defaults to IN_PROGRESS (0b00)
        assert(clog->get_status(999) == pg::TransactionStatus::IN_PROGRESS);
        std::cout << " -> All 8 transaction statuses reloaded and verified from disk!\n";
    }

    // =========================================================================
    // TEST 2: Multi-Page CLOG Scaling (> 32,768 transactions per 8KB page)
    // =========================================================================
    std::cout << "\n[Step 3] Testing multi-page CLOG page scaling (> 32,768 Tx)..." << std::endl;
    {
        auto clog = pg::CLogManager::open(clog_db);

        // Page 0: covers tx 0 .. 32,767
        // Page 1: covers tx 32,768 .. 65,535
        // Page 2: covers tx 65,536 .. 98,303
        clog->set_status(35000, pg::TransactionStatus::COMMITTED);
        clog->set_status(70000, pg::TransactionStatus::ABORTED);

        assert(clog->num_pages() == 3);
        assert(clog->get_status(35000) == pg::TransactionStatus::COMMITTED);
        assert(clog->get_status(70000) == pg::TransactionStatus::ABORTED);
        std::cout << " -> Multi-page scaling verified! 3 CLOG pages allocated for 70,000 transactions.\n";
    }

    // =========================================================================
    // TEST 3: Engine MVCC Visibility using Persistent CLOG (Zero WAL Replay)
    // =========================================================================
    std::cout << "\n[Step 4] Testing Engine transaction persistence via CLOG..." << std::endl;
    {
        pg::Engine engine(db_prefix);

        // Tx 1: Insert item 100 ($10) and COMMIT
        engine.execute("BEGIN;");
        engine.execute("INSERT INTO items VALUES (100, 10);");
        engine.execute("COMMIT;");

        // Tx 2: Insert item 999 ($999) and ABORT
        engine.execute("BEGIN;");
        engine.execute("INSERT INTO items VALUES (999, 999);");
        engine.execute("ROLLBACK;");

        // Tx 3: Insert item 200 ($5) and COMMIT
        engine.execute("BEGIN;");
        engine.execute("INSERT INTO items VALUES (200, 5);");
        engine.execute("COMMIT;");

        std::cout << " -> Committed Tx 1 and Tx 3, Aborted Tx 2.\n";
    }

    std::cout << "[Step 5] Restarting Engine WITHOUT calling recover() (Pure CLOG test)..." << std::endl;
    {
        pg::Engine engine(db_prefix);

        // Even WITHOUT calling engine.recover(), CLOG tells the engine that Tx 1 and Tx 3 committed,
        // while Tx 2 aborted!
        std::string sel1 = engine.execute("SELECT * FROM items WHERE item_id = 100;");
        std::cout << sel1;
        assert(sel1.find("$   10") != std::string::npos);

        std::string sel3 = engine.execute("SELECT * FROM items WHERE item_id = 200;");
        std::cout << sel3;
        assert(sel3.find("$    5") != std::string::npos);

        std::string sel2 = engine.execute("SELECT * FROM items WHERE item_id = 999;");
        std::cout << sel2;
        assert(sel2.find("0 rows") != std::string::npos); // Aborted row is invisible purely via CLOG!
        std::cout << " -> MVCC Visibility verified purely via on-disk CLOG bitmap without WAL replay!\n";
    }

    // =========================================================================
    // TEST 4: SLRU Cache Hits, Misses, Dirty Frames & LRU Eviction Writeback
    // =========================================================================
    std::cout << "\n[Step 6] Testing SLRU shared buffer pool (Hits, Misses, LRU Eviction)..." << std::endl;
    const std::string slru_db = "test_clog_slru.db";
    if (fs::exists(slru_db)) fs::remove(slru_db);

    {
        // Open with only 4 SLRU frames to test eviction under pressure
        auto clog = pg::CLogManager::open(slru_db, 4);
        assert(clog->num_buffers() == 4);

        // Access 4 different pages:
        // tx 0 -> page 0
        // tx 32,768 -> page 1
        // tx 65,536 -> page 2
        // tx 98,304 -> page 3
        clog->set_status(0, pg::TransactionStatus::COMMITTED);
        clog->set_status(32768, pg::TransactionStatus::ABORTED);
        clog->set_status(65536, pg::TransactionStatus::COMMITTED);
        clog->set_status(98304, pg::TransactionStatus::COMMITTED);

        assert(clog->cache_misses() == 4);
        assert(clog->dirty_frames() == 4);

        // Read all 4 pages repeatedly -> 100% cache hits
        for (int i = 0; i < 10; ++i) {
            assert(clog->get_status(0) == pg::TransactionStatus::COMMITTED);
            assert(clog->get_status(32768) == pg::TransactionStatus::ABORTED);
            assert(clog->get_status(65536) == pg::TransactionStatus::COMMITTED);
            assert(clog->get_status(98304) == pg::TransactionStatus::COMMITTED);
        }
        assert(clog->cache_hits() == 40);
        assert(clog->cache_misses() == 4);
        std::cout << " -> SLRU cache hit ratio verified: 40 hits / 4 misses on 4 resident pages.\n";

        // Access page 4 (tx 131,072) and page 5 (tx 163,840) -> forces eviction of 2 dirty frames
        clog->set_status(131072, pg::TransactionStatus::COMMITTED);
        clog->set_status(163840, pg::TransactionStatus::SUB_COMMITTED);

        assert(clog->cache_misses() == 6);
        std::cout << " -> Evicted dirty frames successfully flushed during victim selection.\n";

        // Flush remaining dirty frames
        assert(clog->dirty_frames() > 0);
        clog->flush();
        assert(clog->dirty_frames() == 0);
        std::cout << " -> CLogManager::flush() cleanly flushed all resident dirty frames.\n";
    }

    // Re-read all pages from disk to ensure evicted pages and flushed pages are durable
    {
        auto clog = pg::CLogManager::open(slru_db, 4);
        assert(clog->get_status(0) == pg::TransactionStatus::COMMITTED);
        assert(clog->get_status(32768) == pg::TransactionStatus::ABORTED);
        assert(clog->get_status(65536) == pg::TransactionStatus::COMMITTED);
        assert(clog->get_status(98304) == pg::TransactionStatus::COMMITTED);
        assert(clog->get_status(131072) == pg::TransactionStatus::COMMITTED);
        assert(clog->get_status(163840) == pg::TransactionStatus::SUB_COMMITTED);
        std::cout << " -> All 6 pages persisted and verified correctly across SLRU evictions!\n";
    }

    // High throughput test: 10,000 commits in memory
    {
        auto clog = pg::CLogManager::open(slru_db, 32);
        for (pg::tx_id_t tx = 1000; tx < 11000; ++tx) {
            clog->set_status(tx, pg::TransactionStatus::COMMITTED);
        }
        // Since 1000..10999 is all within Page 0 (0..32767), this should result in exactly 1 miss and 9999 hits!
        assert(clog->cache_misses() == 1);
        assert(clog->cache_hits() == 9999);
        std::cout << " -> High throughput test passed: 10,000 commits achieved with 1 page miss and 9,999 cache hits!\n";
    }

    fs::remove(slru_db);
    fs::remove(clog_db);
    fs::remove(db_prefix + "_heap.db");
    fs::remove(db_prefix + "_wal.log");
    fs::remove(db_prefix + "_toast.db");
    fs::remove(db_prefix + "_clog.db");

    std::cout << "\n>>> ITEM 22 / FINDING 2.8 (CLOG SLRU CACHE) TESTS PASSED SUCCESSFULLY! <<<\n" << std::endl;
}

int main() {
    try {
        run_clog_tests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "CLOG test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
