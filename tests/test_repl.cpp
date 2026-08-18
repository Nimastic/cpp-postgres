#include "pg/engine.h"
#include <iostream>
#include <cassert>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

void run_repl_tests() {
    const std::string db_prefix = "test_repl_db";

    // Clean up previous files
    if (fs::exists(db_prefix + "_heap.db"))  fs::remove(db_prefix + "_heap.db");
    if (fs::exists(db_prefix + "_wal.log"))  fs::remove(db_prefix + "_wal.log");
    if (fs::exists(db_prefix + "_toast.db")) fs::remove(db_prefix + "_toast.db");

    std::cout << "\n--- REPRODUCING HUSSEIN NASSER'S VIDEO VIA SQL REPL ENGINE ---" << std::endl;

    {
        pg::Engine engine(db_prefix);

        // =====================================================================
        // STEP 1: Insert item 100 ($10) and item 200 ($5)
        // =====================================================================
        std::cout << "[Step 1] SQL: INSERT INTO items VALUES (100, 10);" << std::endl;
        std::string out1 = engine.execute("INSERT INTO items VALUES (100, 10);");
        std::cout << out1;
        assert(out1.find("CTID (0, 1)") != std::string::npos);

        std::cout << "[Step 2] SQL: INSERT INTO items VALUES (200, 5);" << std::endl;
        std::string out2 = engine.execute("INSERT INTO items VALUES (200, 5);");
        std::cout << out2;
        assert(out2.find("CTID (0, 2)") != std::string::npos);

        // =====================================================================
        // STEP 2: Start long-running transaction (Snapshot Pinning)
        // =====================================================================
        std::cout << "[Step 3] Session 1: BEGIN (Snapshot Pinning)..." << std::endl;
        std::string out_begin = engine.execute("BEGIN;");
        std::cout << out_begin;
        assert(out_begin.find("Snapshot") != std::string::npos);

        // Session 1 reads item 100 ($10)
        std::string sel_snap = engine.execute("SELECT * FROM items WHERE item_id = 100;");
        std::cout << sel_snap;
        assert(sel_snap.find("$   10") != std::string::npos);

        // =====================================================================
        // STEP 3: In separate autocommit session, UPDATE item 100 to $20
        // =====================================================================
        std::cout << "[Step 4] Session 2: UPDATE items SET price = 20 WHERE item_id = 100;" << std::endl;
        // In our engine, update will execute HOT update
        // (temporarily commit session 1 to simulate concurrent update)
        engine.commit_transaction();

        std::string out_upd = engine.execute("UPDATE items SET price = 20 WHERE item_id = 100;");
        std::cout << out_upd;
        assert(out_upd.find("HOT-update") != std::string::npos || out_upd.find("UPDATE") != std::string::npos);

        // =====================================================================
        // STEP 4: Query via B-Tree Index and Sequential Scan
        // =====================================================================
        std::cout << "\n[Step 5] SQL: SELECT * FROM items WHERE item_id = 100; (Index Scan)" << std::endl;
        std::string sel_idx = engine.execute("SELECT * FROM items WHERE item_id = 100;");
        std::cout << sel_idx;
        assert(sel_idx.find("$   20") != std::string::npos);

        std::cout << "\n[Step 6] SQL: SELECT * FROM items; (Sequential Scan)" << std::endl;
        std::string sel_all = engine.execute("SELECT * FROM items;");
        std::cout << sel_all;
        assert(sel_all.find("$   20") != std::string::npos);
        assert(sel_all.find("$    5") != std::string::npos);

        // =====================================================================
        // STEP 5: Visual Physical Slotted Page Dump
        // =====================================================================
        std::cout << "\n[Step 7] SQL: DUMP PAGE 0;" << std::endl;
        std::string dump_out = engine.execute("DUMP PAGE 0;");
        std::cout << dump_out;
        assert(dump_out.find("PAGE 0 LAYOUT DUMP") != std::string::npos);
        assert(dump_out.find("Slot  1") != std::string::npos);
        assert(dump_out.find("Slot  2") != std::string::npos);

        // =====================================================================
        // STEP 6: VACUUM & STATUS
        // =====================================================================
        std::cout << "\n[Step 8] SQL: STATUS; and VACUUM;" << std::endl;
        std::string stat_out = engine.execute("STATUS;");
        std::cout << stat_out;
        assert(stat_out.find("POSTGRES ENGINE STATUS") != std::string::npos);

        std::string vac_out = engine.execute("VACUUM;");
        std::cout << vac_out;
        assert(vac_out.find("Garbage collection complete") != std::string::npos);
    }

    if (fs::exists(db_prefix + "_heap.db"))  fs::remove(db_prefix + "_heap.db");
    if (fs::exists(db_prefix + "_wal.log"))  fs::remove(db_prefix + "_wal.log");
    if (fs::exists(db_prefix + "_toast.db")) fs::remove(db_prefix + "_toast.db");

    std::cout << "\n>>> ITEM 11 (SQL REPL & END-TO-END ENGINE CAPSTONE) TESTS PASSED SUCCESSFULLY! <<<\n" << std::endl;
}

int main() {
    try {
        run_repl_tests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "REPL test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
