#include "pg/executor.h"
#include "pg/engine.h"
#include <iostream>
#include <cassert>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

void run_executor_tests() {
    const std::string db_prefix = "test_executor_db";

    auto cleanup = [&]() {
        if (fs::exists(db_prefix + "_heap.db"))    fs::remove(db_prefix + "_heap.db");
        if (fs::exists(db_prefix + "_wal.log"))    fs::remove(db_prefix + "_wal.log");
        if (fs::exists(db_prefix + "_toast.db"))   fs::remove(db_prefix + "_toast.db");
        if (fs::exists(db_prefix + "_clog.db"))    fs::remove(db_prefix + "_clog.db");
        if (fs::exists(db_prefix + "_control.db")) fs::remove(db_prefix + "_control.db");
        if (fs::exists(db_prefix + "_index.db"))   fs::remove(db_prefix + "_index.db");
        if (fs::exists(db_prefix + "_fsm.db"))     fs::remove(db_prefix + "_fsm.db");
    };

    cleanup();

    std::cout << "\n--- TESTING VOLCANO ITERATOR QUERY EXECUTION ENGINE (FINDING 2.5) ---" << std::endl;

    // =========================================================================
    // POPULATE DATASET: 500 items across multiple 8KB heap pages
    // =========================================================================
    std::cout << "[Step 1] Populating database with 500 items across multiple pages..." << std::endl;
    {
        pg::Engine engine(db_prefix);
        engine.execute("BEGIN;");
        for (int i = 1; i <= 500; ++i) {
            engine.insert_item(i, i * 10); // price: 10, 20, 30 ... 5000
        }
        engine.execute("COMMIT;");

        assert(engine.heap().num_pages() > 1);
        std::cout << " -> Populated 500 items across " << engine.heap().num_pages() 
                  << " pages (" << (engine.heap().num_pages() * 8) << " KB).\n";
    }

    // =========================================================================
    // TEST 1: Streaming SeqScan with O(1) Buffer Pin Invariant
    // =========================================================================
    std::cout << "\n[Step 2] Testing SeqScanNode with O(1) buffer pool pin invariant..." << std::endl;
    {
        pg::Engine engine(db_prefix);
        engine.execute("BEGIN;");
        auto& sess = engine.default_session();

        pg::SeqScanNode seq_scan(engine.heap(), *sess.snapshot, engine.tm());
        seq_scan.init();

        pg::TupleTableSlot slot;
        size_t count = 0;

        while (seq_scan.next(slot)) {
            assert(!slot.empty());
            count++;

            // CRITICAL INVARIANT: The executor must NEVER pin more than 1 frame in shared buffers!
            assert(engine.bpm().pinned_frames() <= 1);
        }
        seq_scan.end();

        assert(count == 500);
        // After scan completes, all pins MUST be released!
        assert(engine.bpm().pinned_frames() == 0);
        assert(seq_scan.pages_scanned() == engine.heap().num_pages());

        std::cout << " -> Streamed 500 tuples across " << seq_scan.pages_scanned() 
                  << " pages! Pinned frames <= 1 verified throughout entire scan.\n";
        engine.execute("COMMIT;");
    }

    // =========================================================================
    // TEST 2: Pipelined Early Termination via LimitNode
    // =========================================================================
    std::cout << "\n[Step 3] Testing LimitNode pipelined early termination (LIMIT 5)..." << std::endl;
    {
        pg::Engine engine(db_prefix);
        engine.execute("BEGIN;");
        auto& sess = engine.default_session();

        auto scan = std::make_unique<pg::SeqScanNode>(engine.heap(), *sess.snapshot, engine.tm());
        auto* scan_ptr = scan.get();

        pg::LimitNode limit_node(std::move(scan), 5, 0); // LIMIT 5
        limit_node.init();

        pg::TupleTableSlot slot;
        size_t count = 0;
        while (limit_node.next(slot)) {
            count++;
            assert(slot.tuple.data.item_id == static_cast<int32_t>(count));
        }
        limit_node.end();

        assert(count == 5);
        // CRITICAL INVARIANT: LimitNode halted early, reading ONLY Page 0!
        assert(scan_ptr->pages_scanned() == 1);
        assert(engine.bpm().pinned_frames() == 0);

        std::cout << " -> LIMIT 5 produced 5 tuples and halted immediately touching only 1 page (scanned " 
                  << scan_ptr->pages_scanned() << " of " << engine.heap().num_pages() << " pages)!\n";
        engine.execute("COMMIT;");
    }

    // =========================================================================
    // TEST 3: Pipelined Selection via FilterNode (Filter + Limit)
    // =========================================================================
    std::cout << "\n[Step 4] Testing FilterNode pipelining with LimitNode..." << std::endl;
    {
        pg::Engine engine(db_prefix);
        engine.execute("BEGIN;");
        auto& sess = engine.default_session();

        // Query: price > 4500 (items 451 .. 500) LIMIT 3
        auto scan = std::make_unique<pg::SeqScanNode>(engine.heap(), *sess.snapshot, engine.tm());
        auto filter = std::make_unique<pg::FilterNode>(
            std::move(scan),
            [](const pg::TupleTableSlot& s) { return s.tuple.data.price > 4500; },
            "price > 4500"
        );
        pg::LimitNode limit(std::move(filter), 3, 0);

        auto results = pg::ExecutionEngine::execute(limit);
        assert(results.size() == 3);
        assert(results[0].second.data.item_id == 451);
        assert(results[0].second.data.price == 4510);
        assert(results[1].second.data.item_id == 452);
        assert(results[2].second.data.item_id == 453);

        std::cout << " -> Filter (price > 4500) + Limit 3 returned items [451, 452, 453] in stream!\n";
        engine.execute("COMMIT;");
    }

    // =========================================================================
    // TEST 4: IndexScanNode with HOT Chain Traversal
    // =========================================================================
    std::cout << "\n[Step 5] Testing IndexScanNode with HOT update traversal..." << std::endl;
    {
        pg::Engine engine(db_prefix);

        // Update item 100 twice (HOT updates)
        engine.execute("BEGIN;");
        engine.execute("UPDATE items SET price = 9999 WHERE item_id = 100;");
        engine.execute("COMMIT;");

        engine.execute("BEGIN;");
        auto& sess = engine.default_session();

        pg::IndexScanNode idx_scan(engine.index(), engine.heap(), 100, *sess.snapshot, engine.tm());
        auto results = pg::ExecutionEngine::execute(idx_scan);

        assert(results.size() == 1);
        assert(results[0].second.data.item_id == 100);
        assert(results[0].second.data.price == 9999);

        std::cout << " -> IndexScanNode correctly traversed HOT chain and returned visible price $9999!\n";
        engine.execute("COMMIT;");
    }

    // =========================================================================
    // TEST 5: Engine SQL Integration, EXPLAIN and EXPLAIN ANALYZE
    // =========================================================================
    std::cout << "\n[Step 6] Testing Engine SQL parsing, EXPLAIN and EXPLAIN ANALYZE..." << std::endl;
    {
        pg::Engine engine(db_prefix);

        // 1. SELECT * FROM items LIMIT 2
        std::string res1 = engine.execute("SELECT * FROM items LIMIT 2;");
        assert(res1.find("(2 rows returned") != std::string::npos);

        // 2. SELECT * FROM items WHERE price > 4800
        std::string res2 = engine.execute("SELECT * FROM items WHERE price > 4800;");
        // items 481..500 (20 rows) + item 100 updated to 9999 (1 row) = 21 rows
        assert(res2.find("(21 rows returned") != std::string::npos);

        // 3. SELECT * FROM items WHERE price > 4800 LIMIT 3
        std::string res3 = engine.execute("SELECT * FROM items WHERE price > 4800 LIMIT 3;");
        assert(res3.find("(3 rows returned") != std::string::npos);

        // 4. EXPLAIN SELECT * FROM items WHERE price > 4800 LIMIT 3;
        std::string explain_out = engine.execute("EXPLAIN SELECT * FROM items WHERE price > 4800 LIMIT 3;");
        std::cout << "\n" << explain_out;
        assert(explain_out.find("QUERY PLAN:") != std::string::npos);
        assert(explain_out.find("->  Limit:") != std::string::npos);
        assert(explain_out.find("->  Filter: (price > 4800") != std::string::npos);
        assert(explain_out.find("->  Seq Scan on items") != std::string::npos);

        // 5. EXPLAIN ANALYZE SELECT * FROM items WHERE price > 4800 LIMIT 3;
        std::string analyze_out = engine.execute("EXPLAIN ANALYZE SELECT * FROM items WHERE price > 4800 LIMIT 3;");
        std::cout << analyze_out;
        assert(analyze_out.find("QUERY PLAN (ANALYZE:") != std::string::npos);
        assert(analyze_out.find("rows=3") != std::string::npos);

        // 6. EXPLAIN Index Scan
        std::string idx_explain = engine.execute("EXPLAIN SELECT * FROM items WHERE item_id = 100;");
        std::cout << idx_explain;
        assert(idx_explain.find("Index Scan using items_pkey on items (Key: 100") != std::string::npos);

        std::cout << " -> Engine SQL commands and EXPLAIN / EXPLAIN ANALYZE verified successfully!\n";
    }

    cleanup();

    std::cout << "\n>>> ITEM 23 / FINDING 2.5 (VOLCANO QUERY EXECUTOR) TESTS PASSED SUCCESSFULLY! <<<\n" << std::endl;
}

int main() {
    try {
        run_executor_tests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Executor test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
