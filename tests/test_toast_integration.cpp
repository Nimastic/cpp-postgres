#include "pg/toast.h"
#include "pg/heap.h"
#include "pg/engine.h"
#include "pg/tuple.h"
#include <iostream>
#include <vector>
#include <cassert>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

void run_toast_integration_tests() {
    const std::string db_prefix = "test_toast_int_engine";
    const std::string wal_file  = db_prefix + "_wal.log";
    const std::string heap_file = db_prefix + "_heap.db";
    const std::string clog_file = db_prefix + "_clog.db";
    const std::string toast_file= db_prefix + "_toast.db";

    if (fs::exists(wal_file))   fs::remove(wal_file);
    if (fs::exists(heap_file))  fs::remove(heap_file);
    if (fs::exists(clog_file))  fs::remove(clog_file);
    if (fs::exists(toast_file)) fs::remove(toast_file);

    std::cout << "\n--- REPRODUCING POSTGRESQL TOAST + HEAP + INDEX INTEGRATION ---" << std::endl;

    // =========================================================================
    // TEST 1: Inline Document Storage (< 2048 bytes)
    // =========================================================================
    std::cout << "[Step 1] Inserting small inline document (<2KB)..." << std::endl;
    {
        pg::Engine engine(db_prefix);

        std::string small_doc = "Standard PostgreSQL product specifications (under 2KB).";
        std::string out = engine.insert_item_with_doc(100, 10, small_doc);
        std::cout << out;
        assert(out.find("INLINE in tuple") != std::string::npos);

        std::string sel = engine.select_doc_by_id(100);
        std::cout << sel;
        assert(sel.find("INLINE ATTRIBUTE") != std::string::npos);
        std::cout << " -> Verified: Small payload stored inline without auxiliary TOAST table write.\n";
    }

    // =========================================================================
    // TEST 2: Oversized Document Storage (> 2048 bytes -> Chunked TOAST Table)
    // =========================================================================
    std::cout << "\n[Step 2] Inserting oversized document (10,000 bytes -> 5 chunks of 2KB)..." << std::endl;
    std::string large_doc;
    for (int i = 0; i < 1000; ++i) {
        large_doc += "0123456789"; // 10,000 bytes
    }
    assert(large_doc.size() == 10000);

    {
        pg::Engine engine(db_prefix);

        std::string out = engine.insert_item_with_doc(200, 20, large_doc);
        std::cout << out;
        assert(out.find("OUT-OF-LINE in TOAST table") != std::string::npos);
        assert(out.find("5 chunks of 2KB") != std::string::npos);

        // Verify TOAST table metrics
        assert(engine.toast().total_chunks() == 5);
        std::cout << " -> TOAST manager confirmed 5 auxiliary 2KB chunks stored in toast relation.\n";

        // Query by B-Tree Index: verify HEAP_HASEXTERNAL flag
        std::string sel = engine.select_doc_by_id(200);
        std::cout << sel;
        assert(sel.find("TOASTED ATTRIBUTE PRESENT") != std::string::npos);
        std::cout << " -> Verified: Heap tuple header has HEAP_HASEXTERNAL infomask bit flag set.\n";
    }

    // =========================================================================
    // TEST 3: SQL REPL 3-Argument INSERT & Cross-Restart TOAST Persistence
    // =========================================================================
    std::cout << "\n[Step 3] Testing SQL REPL INSERT with document..." << std::endl;
    {
        pg::Engine engine(db_prefix);

        std::string repl_out = engine.execute("INSERT INTO items VALUES (300, 30, 'Documentation payload via REPL');");
        std::cout << repl_out;
        assert(repl_out.find("INSERT (WITH TOAST)") != std::string::npos);

        // Query via Index Scan
        std::string q_out = engine.execute("SELECT * FROM items WHERE item_id = 300;");
        std::cout << q_out;
        assert(q_out.find("1 row returned") != std::string::npos);
    }

    std::cout << "[Step 4] Restarting engine and verifying TOAST persistence from disk..." << std::endl;
    {
        pg::Engine engine(db_prefix);

        std::string sel100 = engine.execute("SELECT * FROM items WHERE item_id = 100;");
        assert(sel100.find("1 row returned") != std::string::npos);

        std::string sel200 = engine.execute("SELECT * FROM items WHERE item_id = 200;");
        assert(sel200.find("1 row returned") != std::string::npos);

        std::string sel300 = engine.execute("SELECT * FROM items WHERE item_id = 300;");
        assert(sel300.find("1 row returned") != std::string::npos);

        std::string all = engine.execute("SELECT * FROM items;");
        std::cout << all;
        assert(all.find("3 rows returned") != std::string::npos);
        assert(engine.toast().total_chunks() == 5);

        // Fetch back 10,000-byte document across restart
        pg::ToastValue tv;
        tv.type = pg::ToastStorageType::OUT_OF_LINE;
        tv.pointer.toast_id = 1;
        tv.pointer.raw_size = 10001;
        tv.pointer.chunk_count = 5;
        std::string recovered_doc = engine.toast().fetch_string(tv);
        assert(recovered_doc == large_doc);
        std::cout << " -> All rows and 10,000-byte TOASTed doc verified surviving restart across disk files!\n";
    }

    // =========================================================================
    // TEST 5: Crash Recovery Replay of TOAST Chunks from WAL
    // =========================================================================
    std::cout << "\n[Step 5] Testing TOAST WAL logging and ARIES crash recovery replay..." << std::endl;
    const std::string crash_prefix = "test_toast_crash_engine";
    const std::string c_wal_file   = crash_prefix + "_wal.log";
    const std::string c_heap_file  = crash_prefix + "_heap.db";
    const std::string c_clog_file  = crash_prefix + "_clog.db";
    const std::string c_toast_file = crash_prefix + "_toast.db";
    const std::string c_index_file = crash_prefix + "_index.db";
    const std::string c_ctrl_file  = crash_prefix + "_control.db";

    for (const auto& f : {c_wal_file, c_heap_file, c_clog_file, c_toast_file, c_index_file, c_ctrl_file}) {
        if (fs::exists(f)) fs::remove(f);
    }

    std::string huge_doc(20480, 'X'); // 20KB payload -> 11 chunks (with null terminator 20481)
    {
        pg::Engine engine(crash_prefix);
        engine.execute("BEGIN;");
        std::string ins = engine.insert_item_with_doc(500, 50, huge_doc);
        std::cout << ins;
        assert(ins.find("11 chunks of 2KB") != std::string::npos);
        engine.execute("COMMIT;");
        assert(engine.toast().total_chunks() == 11);
    }

    // Simulate sudden crash before TOAST pages were synced to disk:
    // Mark control file as unclean (needs recovery) and delete the toast relation file.
    {
        pg::ControlFile ctrl(c_ctrl_file);
        ctrl.mark_in_production();
    }
    if (fs::exists(c_toast_file)) {
        fs::remove(c_toast_file);
    }

    // Restart engine: Startup recovery will see unclean control file,
    // invoke wal_->recover(*heap_, tm_, toast_.get()),
    // and replay all 11 TOAST_INSERT records from WAL into toast relation!
    {
        pg::Engine recovered_engine(crash_prefix);
        assert(recovered_engine.recovered_at_startup());
        assert(recovered_engine.toast().total_chunks() == 11);

        pg::ToastValue tv;
        tv.type = pg::ToastStorageType::OUT_OF_LINE;
        tv.pointer.toast_id = 1;
        tv.pointer.raw_size = 20481;
        tv.pointer.chunk_count = 11;
        std::string replayed_doc = recovered_engine.toast().fetch_string(tv);
        assert(replayed_doc == huge_doc);
        std::cout << " -> Verified: All 11 TOAST chunks replayed from WAL and 20KB doc recovered byte-for-byte!\n";
    }

    for (const auto& f : {c_wal_file, c_heap_file, c_clog_file, c_toast_file, c_index_file, c_ctrl_file}) {
        if (fs::exists(f)) fs::remove(f);
    }

    fs::remove(wal_file);
    fs::remove(heap_file);
    fs::remove(clog_file);
    fs::remove(toast_file);
    if (fs::exists(db_prefix + "_index.db"))   fs::remove(db_prefix + "_index.db");
    if (fs::exists(db_prefix + "_control.db")) fs::remove(db_prefix + "_control.db");

    std::cout << "\n>>> ITEM 17 (TOAST HEAP + INDEX INTEGRATION & ARIES CRASH RECOVERY) TESTS PASSED SUCCESSFULLY! <<<\n" << std::endl;
}

int main() {
    try {
        run_toast_integration_tests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "TOAST integration test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
