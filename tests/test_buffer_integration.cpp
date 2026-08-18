#include "pg/engine.h"
#include "pg/buffer_pool.h"
#include <iostream>
#include <cassert>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

void run_buffer_integration_tests() {
    const std::string db_prefix = "test_bpm_int";

    if (fs::exists(db_prefix + "_heap.db"))  fs::remove(db_prefix + "_heap.db");
    if (fs::exists(db_prefix + "_wal.log"))  fs::remove(db_prefix + "_wal.log");
    if (fs::exists(db_prefix + "_toast.db")) fs::remove(db_prefix + "_toast.db");

    std::cout << "\n--- BUFFER POOL INTEGRATION (ALL HEAP I/O THROUGH SHARED_BUFFERS) ---" << std::endl;

    // =========================================================================
    // TEST 1: All heap inserts go through buffer pool
    // =========================================================================
    {
        pg::Engine engine(db_prefix);

        std::cout << "[Step 1] Inserting 50 rows through buffer-pool-integrated heap..." << std::endl;
        for (int i = 1; i <= 50; ++i) {
            engine.execute("INSERT INTO items VALUES (" + std::to_string(i * 100) + ", " + std::to_string(i * 10) + ");");
        }

        // Verify buffer pool has resident pages
        size_t resident = engine.bpm().resident_pages();
        std::cout << " -> Buffer pool has " << resident << " resident pages after 50 inserts.\n";
        assert(resident > 0);

        // Verify reads go through buffer pool (cache hits)
        std::cout << "[Step 2] Reading item 100 via B-Tree index (should hit buffer pool)..." << std::endl;
        std::string sel = engine.execute("SELECT * FROM items WHERE item_id = 100;");
        std::cout << sel;
        assert(sel.find("$   10") != std::string::npos);

        // Verify sequential scan through buffer pool
        std::cout << "[Step 3] Sequential scan of all 50 rows through buffer pool..." << std::endl;
        std::string all = engine.execute("SELECT * FROM items;");
        assert(all.find("50 rows") != std::string::npos);
        std::cout << " -> Sequential scan returned all 50 rows via buffer pool.\n";

        // Verify DUMP PAGE still works through buffer pool
        std::cout << "[Step 4] Page dump through buffer pool..." << std::endl;
        std::string dump = engine.execute("DUMP PAGE 0;");
        assert(dump.find("PAGE 0 LAYOUT DUMP") != std::string::npos);
        std::cout << " -> Page dump successful through buffer pool integration.\n";

        // Verify STATUS reports buffer pool metrics
        std::string status = engine.execute("STATUS;");
        assert(status.find("resident in RAM") != std::string::npos);
        std::cout << " -> STATUS report includes buffer pool metrics.\n";
    }

    // =========================================================================
    // TEST 2: Data survives engine restart (buffer pool flush to disk)
    // =========================================================================
    std::cout << "\n[Step 5] Restarting engine from disk files..." << std::endl;
    {
        pg::Engine engine(db_prefix);
        engine.recover(); // Replay WAL to reconstruct committed tx status

        std::string sel = engine.execute("SELECT * FROM items WHERE item_id = 2500;");
        std::cout << sel;
        assert(sel.find("$  250") != std::string::npos);
        std::cout << " -> Data persisted to disk via buffer pool writeback and survived restart!\n";

        // Verify all 50 rows survived
        std::string all = engine.execute("SELECT * FROM items;");
        assert(all.find("50 rows") != std::string::npos);
        std::cout << " -> All 50 rows verified after engine restart.\n";
    }

    // =========================================================================
    // TEST 3: HOT updates through buffer pool
    // =========================================================================
    std::cout << "\n[Step 6] Testing HOT update through buffer pool..." << std::endl;
    {
        pg::Engine engine(db_prefix);
        engine.recover();

        std::string upd = engine.execute("UPDATE items SET price = 999 WHERE item_id = 100;");
        std::cout << upd;
        assert(upd.find("UPDATE") != std::string::npos);

        std::string sel = engine.execute("SELECT * FROM items WHERE item_id = 100;");
        std::cout << sel;
        assert(sel.find("$  999") != std::string::npos);
        std::cout << " -> HOT update verified through buffer pool integration.\n";
    }

    // =========================================================================
    // TEST 4: VACUUM through buffer pool
    // =========================================================================
    std::cout << "\n[Step 7] Running VACUUM through buffer pool integration..." << std::endl;
    {
        pg::Engine engine(db_prefix);
        engine.recover();

        std::string vac = engine.execute("VACUUM;");
        std::cout << vac;
        assert(vac.find("Garbage collection complete") != std::string::npos);
        std::cout << " -> VACUUM executed through buffer pool integration.\n";
    }

    if (fs::exists(db_prefix + "_heap.db"))  fs::remove(db_prefix + "_heap.db");
    if (fs::exists(db_prefix + "_wal.log"))  fs::remove(db_prefix + "_wal.log");
    if (fs::exists(db_prefix + "_toast.db")) fs::remove(db_prefix + "_toast.db");

    std::cout << "\n>>> ITEM 12 (BUFFER POOL INTEGRATION) TESTS PASSED SUCCESSFULLY! <<<\n" << std::endl;
}

int main() {
    try {
        run_buffer_integration_tests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Buffer integration test failed: " << e.what() << std::endl;
        return 1;
    }
}
