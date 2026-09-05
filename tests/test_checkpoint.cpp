#include "pg/wal.h"
#include "pg/heap.h"
#include "pg/buffer_pool.h"
#include "pg/engine.h"
#include <iostream>
#include <vector>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

void run_checkpoint_tests() {
    const std::string db_prefix = "test_ckpt_engine";
    const std::string wal_file  = db_prefix + "_wal.log";
    const std::string heap_file = db_prefix + "_heap.db";
    const std::string clog_file = db_prefix + "_clog.db";
    const std::string toast_file= db_prefix + "_toast.db";

    if (fs::exists(wal_file))   fs::remove(wal_file);
    if (fs::exists(heap_file))  fs::remove(heap_file);
    if (fs::exists(clog_file))  fs::remove(clog_file);
    if (fs::exists(toast_file)) fs::remove(toast_file);

    std::cout << "\n--- REPRODUCING POSTGRESQL WAL CHECKPOINTS & FULL-PAGE IMAGES (FPI) ---" << std::endl;

    // =========================================================================
    // TEST 1: Checkpoint Skip Replay Optimization
    // =========================================================================
    std::cout << "[Step 1] Inserting 50 items and executing CHECKPOINT..." << std::endl;
    {
        pg::Engine engine(db_prefix);

        for (int i = 1; i <= 50; ++i) {
            engine.execute("INSERT INTO items VALUES (" + std::to_string(i * 100) + ", " + std::to_string(i * 10) + ");");
        }

        // Execute Checkpoint
        std::string ck_out = engine.execute("CHECKPOINT;");
        std::cout << ck_out;
        assert(ck_out.find("[CHECKPOINT]") != std::string::npos);

        // Insert 5 additional items AFTER checkpoint
        std::cout << "[Step 2] Inserting 5 post-checkpoint items (5100..5500)..." << std::endl;
        for (int i = 51; i <= 55; ++i) {
            engine.execute("INSERT INTO items VALUES (" + std::to_string(i * 100) + ", " + std::to_string(i * 10) + ");");
        }
    }

    std::cout << "[Step 3] Restarting database and running WAL recovery from checkpoint..." << std::endl;
    {
        pg::Engine engine(db_prefix);

        // Replay WAL: should only replay the 5 post-checkpoint operations!
        std::string rec_out = engine.execute("RECOVER;");
        std::cout << rec_out;
        // Recovery must start at the checkpoint, not at the head of the log, so
        // the 50 pre-checkpoint inserts are skipped entirely. The exact count is
        // no longer 5: full-page images are now actually emitted (the first
        // write to a page after a checkpoint carries one), so the tail of the
        // log holds an FPI as well as the 5 INSERT records.
        size_t pos = rec_out.find("replayed ");
        assert(pos != std::string::npos);
        int replayed = std::atoi(rec_out.c_str() + pos + 9);
        assert(replayed >= 5 && replayed <= 12);
        std::cout << " -> Verified: recovery replayed only " << replayed
                  << " records from the checkpoint, skipping all 50 before it.\n";

        // Verify all 55 items are visible
        std::string all = engine.execute("SELECT * FROM items;");
        assert(all.find("55 rows") != std::string::npos);
        std::cout << " -> All 55 items verified after fast checkpoint-assisted recovery.\n";
    }

    // =========================================================================
    // TEST 2: Full-Page Image (FPI) Torn-Page Healing
    // =========================================================================
    std::cout << "\n[Step 4] Testing Full-Page Image (FPI) Torn-Page Restoration..." << std::endl;
    const std::string fpi_wal_path  = "test_fpi_wal.log";
    const std::string fpi_heap_path = "test_fpi_heap.db";

    if (fs::exists(fpi_wal_path))  fs::remove(fpi_wal_path);
    if (fs::exists(fpi_heap_path)) fs::remove(fpi_heap_path);

    {
        auto wal  = pg::WALManager::open(fpi_wal_path);
        auto heap = pg::HeapFile::open(fpi_heap_path);

        // Populate Page 0 with 5 tuples
        for (int i = 1; i <= 5; ++i) {
            pg::ItemRecord rec{i * 100, i * 10};
            heap->insert(rec, 1);
        }

        // Read pristine Page 0. The inserts above are still sitting dirty in the
        // buffer pool, so the pool has to be flushed before the page can be read
        // straight off disk -- otherwise this captures an empty page and the FPI
        // records nothing.
        heap->bpm()->flush_all();
        std::vector<uint8_t> pristine_page(pg::PAGE_SIZE, 0);
        heap->pager().read_page(0, pristine_page.data());

        // Write FPI record to WAL
        pg::lsn_t fpi_lsn = wal->log_fpi(0, pristine_page.data());
        wal->flush(fpi_lsn);
        std::cout << " -> FPI record logged to WAL at LSN: " << fpi_lsn << "\n";

        // SIMULATE TORN PAGE / SECTOR CORRUPTION: Corrupt the second half of Page 0 on disk!
        std::vector<uint8_t> corrupted_page = pristine_page;
        std::memset(corrupted_page.data() + 4096, 0xDE, 4096); // Overwrite second 4KB with garbage
        heap->pager().write_page(0, corrupted_page.data());
        std::cout << " -> Simulated torn page on disk (corrupted 4096..8191 bytes with 0xDE).\n";
    }

    // Recover using FPI baseline
    {
        auto wal  = pg::WALManager::open(fpi_wal_path);
        auto heap = pg::HeapFile::open(fpi_heap_path);
        pg::TransactionManager tm;

        size_t replayed = wal->recover(*heap, tm);
        std::cout << " -> WAL recovery replayed " << replayed << " records (including FPI baseline).\n";

        // Read recovered Page 0 and verify torn-page is fully healed
        std::vector<uint8_t> healed_page(pg::PAGE_SIZE, 0);
        heap->pager().read_page(0, healed_page.data());
        pg::Page p0(healed_page.data());

        assert(p0.num_slots() == 5); // All 5 slots restored!
        for (pg::slot_id_t s = 1; s <= 5; ++s) {
            size_t len = 0;
            const uint8_t* ptr = p0.get_tuple_ptr(s, &len);
            assert(ptr != nullptr);
            pg::HeapTuple t = pg::HeapTuple::deserialize(ptr, len);
            assert(t.data.item_id == s * 100);
            assert(t.data.price == s * 10);
        }
        std::cout << " -> Torn page fully healed from FPI! All 5 tuples restored byte-for-byte.\n";
    }

    fs::remove(wal_file);
    fs::remove(heap_file);
    fs::remove(clog_file);
    fs::remove(toast_file);
    fs::remove(fpi_wal_path);
    fs::remove(fpi_heap_path);

    std::cout << "\n>>> ITEM 15 (WAL CHECKPOINTS & FULL-PAGE IMAGES) TESTS PASSED SUCCESSFULLY! <<<\n" << std::endl;
}

int main() {
    try {
        run_checkpoint_tests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Checkpoint test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
