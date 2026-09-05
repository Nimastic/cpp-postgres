#include "pg/engine.h"
#include "pg/executor.h"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

void run_joins_tests() {
    const std::string db_prefix = "test_joins_db";

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

    std::cout << "\n--- TESTING VOLCANO RELATIONAL JOINS (MILESTONE 24) ---" << std::endl;

    // =========================================================================
    // POPULATE DATASET: Items with intentional price groupings & duplicates
    // =========================================================================
    std::cout << "[Step 1] Populating database with test records..." << std::endl;
    {
        pg::Engine engine(db_prefix);
        engine.execute("BEGIN;");
        // Insert items:
        // Item 1: price 100
        // Item 2: price 200
        // Item 3: price 100 (duplicate price!)
        // Item 4: price 300
        // Item 5: price 200 (duplicate price!)
        engine.insert_item(1, 100);
        engine.insert_item(2, 200);
        engine.insert_item(3, 100);
        engine.insert_item(4, 300);
        engine.insert_item(5, 200);
        engine.execute("COMMIT;");
        std::cout << " -> Inserted 5 test items (prices: 100, 200, 100, 300, 200).\n";
    }

    // =========================================================================
    // TEST 1: Demand-Driven Nested-Loop Cross-Join (Cartesian Product)
    // =========================================================================
    std::cout << "\n[Step 2] Testing NestedLoopJoinNode cross-join (no predicate)..." << std::endl;
    {
        pg::Engine engine(db_prefix);
        engine.execute("BEGIN;");

        auto outer = std::make_unique<pg::SeqScanNode>(engine.heap(), *engine.default_session().snapshot, engine.tm());
        auto inner = std::make_unique<pg::SeqScanNode>(engine.heap(), *engine.default_session().snapshot, engine.tm());

        pg::NestedLoopJoinNode cross_join(std::move(outer), std::move(inner), nullptr, "cross-join");
        auto results = pg::ExecutionEngine::execute_slots(cross_join);

        // 5 items x 5 items = 25 rows
        assert(results.size() == 25);
        for (const auto& slot : results) {
            assert(slot.has_inner);
            assert(slot.ctid.is_valid());
            assert(slot.inner_ctid.is_valid());
        }
        std::cout << " -> Cross-join correctly produced " << results.size() << " tuples (5x5 Cartesian product).\n";

        std::string exp = cross_join.explain();
        assert(exp.find("Nested Loop") != std::string::npos);
        assert(exp.find("Seq Scan") != std::string::npos);
        std::cout << " -> Explain output:\n" << exp << std::endl;
    }

    // =========================================================================
    // TEST 2: NestedLoopJoinNode Equi-Join on price
    // =========================================================================
    std::cout << "\n[Step 3] Testing NestedLoopJoinNode equi-join (a.price = b.price)..." << std::endl;
    std::vector<pg::TupleTableSlot> nl_results;
    {
        pg::Engine engine(db_prefix);
        engine.execute("BEGIN;");

        auto outer = std::make_unique<pg::SeqScanNode>(engine.heap(), *engine.default_session().snapshot, engine.tm());
        auto inner = std::make_unique<pg::SeqScanNode>(engine.heap(), *engine.default_session().snapshot, engine.tm());

        auto pred = [](const pg::TupleTableSlot& out, const pg::TupleTableSlot& in) {
            return out.tuple.data.price == in.tuple.data.price;
        };

        pg::NestedLoopJoinNode nl_join(std::move(outer), std::move(inner), pred, "a.price = b.price");
        nl_results = pg::ExecutionEngine::execute_slots(nl_join);

        // Matches breakdown:
        // price 100: items (1, 3) -> 2 x 2 = 4 pairs
        // price 200: items (2, 5) -> 2 x 2 = 4 pairs
        // price 300: item (4)     -> 1 x 1 = 1 pair
        // Total = 4 + 4 + 1 = 9 matches
        assert(nl_results.size() == 9);
        for (const auto& slot : nl_results) {
            assert(slot.has_inner);
            assert(slot.tuple.data.price == slot.inner_tuple.data.price);
        }
        std::cout << " -> Nested Loop equi-join produced " << nl_results.size() << " matching pairs.\n";
    }

    // =========================================================================
    // TEST 3: HashJoinNode Equi-Join & Semantic Equivalence to Nested-Loop
    // =========================================================================
    std::cout << "\n[Step 4] Testing HashJoinNode equi-join and equivalence..." << std::endl;
    {
        pg::Engine engine(db_prefix);
        engine.execute("BEGIN;");

        auto outer = std::make_unique<pg::SeqScanNode>(engine.heap(), *engine.default_session().snapshot, engine.tm());
        auto inner = std::make_unique<pg::SeqScanNode>(engine.heap(), *engine.default_session().snapshot, engine.tm());

        auto outer_key_fn = [](const pg::TupleTableSlot& s) { return s.tuple.data.price; };
        auto inner_key_fn = [](const pg::TupleTableSlot& s) { return s.tuple.data.price; };

        pg::HashJoinNode hash_join(std::move(outer), std::move(inner),
                                   outer_key_fn, inner_key_fn, "a.price = b.price");
        auto hash_results = pg::ExecutionEngine::execute_slots(hash_join);

        assert(hash_results.size() == nl_results.size());
        assert(hash_results.size() == 9);

        // Verify each joined pair in hash_results has equal prices
        for (const auto& slot : hash_results) {
            assert(slot.has_inner);
            assert(slot.tuple.data.price == slot.inner_tuple.data.price);
        }

        // Sort both result vectors by (a.item_id, b.item_id) to verify identical multiset contents
        auto sort_key = [](const pg::TupleTableSlot& s) {
            return std::make_pair(s.tuple.data.item_id, s.inner_tuple.data.item_id);
        };
        std::sort(nl_results.begin(), nl_results.end(), [&](const auto& x, const auto& y) {
            return sort_key(x) < sort_key(y);
        });
        std::sort(hash_results.begin(), hash_results.end(), [&](const auto& x, const auto& y) {
            return sort_key(x) < sort_key(y);
        });

        for (size_t i = 0; i < nl_results.size(); ++i) {
            assert(nl_results[i].tuple.data.item_id == hash_results[i].tuple.data.item_id);
            assert(nl_results[i].inner_tuple.data.item_id == hash_results[i].inner_tuple.data.item_id);
            assert(nl_results[i].tuple.data.price == hash_results[i].tuple.data.price);
            assert(nl_results[i].inner_tuple.data.price == hash_results[i].inner_tuple.data.price);
        }
        std::cout << " -> Hash Join matched Nested-Loop results with 100% precision.\n";

        std::string exp = hash_join.explain();
        assert(exp.find("Hash Join") != std::string::npos);
        assert(exp.find("hash_entries=5") != std::string::npos);
        std::cout << " -> Explain output:\n" << exp << std::endl;
    }

    // =========================================================================
    // TEST 4: Pipelined Early Termination with LimitNode over Joins
    // =========================================================================
    std::cout << "\n[Step 5] Testing Pipelined Early Termination (LimitNode over HashJoin)..." << std::endl;
    {
        pg::Engine engine(db_prefix);
        engine.execute("BEGIN;");

        auto outer = std::make_unique<pg::SeqScanNode>(engine.heap(), *engine.default_session().snapshot, engine.tm());
        auto inner = std::make_unique<pg::SeqScanNode>(engine.heap(), *engine.default_session().snapshot, engine.tm());

        auto outer_key_fn = [](const pg::TupleTableSlot& s) { return s.tuple.data.price; };
        auto inner_key_fn = [](const pg::TupleTableSlot& s) { return s.tuple.data.price; };

        auto hash_join = std::make_unique<pg::HashJoinNode>(std::move(outer), std::move(inner),
                                                             outer_key_fn, inner_key_fn, "a.price = b.price");
        pg::LimitNode limit_join(std::move(hash_join), 3);
        auto results = pg::ExecutionEngine::execute_slots(limit_join);

        assert(results.size() == 3);
        std::cout << " -> LimitNode successfully halted HashJoin after exactly 3 tuples.\n";
    }

    // =========================================================================
    // TEST 5: Full SQL Execution & REPL Integration
    // =========================================================================
    std::cout << "\n[Step 6] Testing SQL JOIN and HASH JOIN parsing in Engine::execute..." << std::endl;
    {
        pg::Engine engine(db_prefix);

        // 1. Nested loop join SQL
        std::string nl_sql = engine.execute("SELECT * FROM items a JOIN items b ON a.price = b.price;");
        assert(nl_sql.find("9 rows returned via Nested Loop Join Scan") != std::string::npos);
        assert(nl_sql.find("a.item_id") != std::string::npos);
        assert(nl_sql.find("b.item_id") != std::string::npos);
        std::cout << " -> SQL Nested Loop Join executed successfully:\n" << nl_sql << std::endl;

        // 2. Hash join SQL
        std::string hj_sql = engine.execute("SELECT * FROM items a HASH JOIN items b ON a.price = b.price;");
        assert(hj_sql.find("9 rows returned via Hash Join Scan") != std::string::npos);
        std::cout << " -> SQL Hash Join executed successfully:\n" << hj_sql << std::endl;

        // 3. Hash join with LIMIT
        std::string limit_sql = engine.execute("SELECT * FROM items a HASH JOIN items b ON a.price = b.price LIMIT 2;");
        assert(limit_sql.find("2 rows returned via Hash Join Scan") != std::string::npos);
        std::cout << " -> SQL Hash Join with LIMIT 2 executed successfully:\n" << limit_sql << std::endl;

        // 4. EXPLAIN query
        std::string explain_sql = engine.execute("EXPLAIN SELECT * FROM items a JOIN items b ON a.price = b.price;");
        assert(explain_sql.find("QUERY PLAN:") != std::string::npos);
        assert(explain_sql.find("Nested Loop") != std::string::npos);
        std::cout << " -> EXPLAIN Join output:\n" << explain_sql << std::endl;

        // 5. EXPLAIN ANALYZE query
        std::string explain_an_sql = engine.execute("EXPLAIN ANALYZE SELECT * FROM items a HASH JOIN items b ON a.price = b.price;");
        assert(explain_an_sql.find("QUERY PLAN (ANALYZE:") != std::string::npos);
        assert(explain_an_sql.find("Hash Join") != std::string::npos);
        std::cout << " -> EXPLAIN ANALYZE Hash Join output:\n" << explain_an_sql << std::endl;

        // 6. Cross join SQL
        std::string cross_sql = engine.execute("SELECT * FROM items a JOIN items b;");
        assert(cross_sql.find("25 rows returned via Nested Loop Join Scan") != std::string::npos);
        std::cout << " -> SQL Cross Join executed successfully.\n";

        // 7. Error handling: HASH JOIN without ON
        std::string err_sql = engine.execute("SELECT * FROM items a HASH JOIN items b;");
        assert(err_sql.find("[ERROR] HASH JOIN requires an equi-join ON condition") != std::string::npos);
        std::cout << " -> Expected error caught for HASH JOIN without ON clause.\n";
    }

    cleanup();
    std::cout << "\n>>> ITEM 24 (RELATIONAL JOINS: NESTED-LOOP & HASH JOIN) TESTS PASSED SUCCESSFULLY! <<<\n" << std::endl;
}

int main() {
    try {
        run_joins_tests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Joins test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
