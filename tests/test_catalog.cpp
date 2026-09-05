#include "pg/engine.h"
#include <cassert>
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;

void run_catalog_tests() {
    const std::string db_prefix = "test_catalog_db";

    auto cleanup = [&]() {
        for (const auto& entry : fs::directory_iterator(".")) {
            std::string path = entry.path().string();
            if (path.find(db_prefix) != std::string::npos) {
                fs::remove(entry.path());
            }
        }
    };

    cleanup();

    std::cout << "\n--- TESTING DYNAMIC CATALOGS & MULTI-TABLE DDL (MILESTONE 26) ---" << std::endl;

    // =========================================================================
    // TEST 1: Default Catalog Bootstrap
    // =========================================================================
    std::cout << "[Step 1] Verifying System Catalog bootstrap with default 'items' table..." << std::endl;
    {
        pg::Engine engine(db_prefix);
        assert(engine.catalog().has_table("items"));
        const auto* meta = engine.catalog().get_table("items");
        assert(meta != nullptr);
        assert(meta->relname == "items");
        assert(meta->columns.size() == 2);
        assert(meta->columns[0].name == "item_id");
        assert(meta->columns[1].name == "price");

        std::string tables_str = engine.execute("SHOW TABLES;");
        assert(tables_str.find("items") != std::string::npos);
        std::cout << " -> Catalog bootstrapped successfully:\n" << tables_str << std::endl;
    }

    // =========================================================================
    // TEST 2: Dynamic CREATE TABLE DDL
    // =========================================================================
    std::cout << "[Step 2] Testing dynamic CREATE TABLE for 'users' and 'orders'..." << std::endl;
    {
        pg::Engine engine(db_prefix);

        std::string res1 = engine.execute("CREATE TABLE users (user_id INT, age INT);");
        assert(res1.find("successfully created") != std::string::npos);
        std::cout << " -> " << res1;

        std::string res2 = engine.execute("CREATE TABLE orders (order_id INT, amount INT);");
        assert(res2.find("successfully created") != std::string::npos);
        std::cout << " -> " << res2;

        std::string tables_str = engine.execute("\\dt;");
        assert(tables_str.find("items") != std::string::npos);
        assert(tables_str.find("users") != std::string::npos);
        assert(tables_str.find("orders") != std::string::npos);
        assert(tables_str.find("3 relations") != std::string::npos);
        std::cout << " -> All 3 relations registered in catalog:\n" << tables_str << std::endl;

        std::string desc_users = engine.execute("DESCRIBE users;");
        assert(desc_users.find("user_id") != std::string::npos);
        assert(desc_users.find("age") != std::string::npos);
        std::cout << " -> Schema description verified:\n" << desc_users << std::endl;

        // Duplicate table rejection
        std::string dup = engine.execute("CREATE TABLE users (id INT);");
        assert(dup.find("[ERROR] relation \"users\" already exists") != std::string::npos);
        std::cout << " -> Duplicate table creation rejected cleanly.\n";
    }

    // =========================================================================
    // TEST 3: Multi-Table DML & Storage Isolation
    // =========================================================================
    std::cout << "\n[Step 3] Testing multi-table INSERT and SELECT with independent storage..." << std::endl;
    {
        pg::Engine engine(db_prefix);

        // Populate users table
        engine.execute("INSERT INTO users VALUES (1, 25);");
        engine.execute("INSERT INTO users VALUES (2, 30);");
        engine.execute("INSERT INTO users VALUES (3, 40);");

        // Populate orders table
        engine.execute("INSERT INTO orders VALUES (101, 500);");
        engine.execute("INSERT INTO orders VALUES (102, 1200);");

        // Query users
        std::string users_res = engine.execute("SELECT * FROM users;");
        assert(users_res.find("3 rows returned via Sequential Scan on users") != std::string::npos);
        std::cout << " -> Users table contents:\n" << users_res << std::endl;

        // Query orders
        std::string orders_res = engine.execute("SELECT * FROM orders;");
        assert(orders_res.find("2 rows returned via Sequential Scan on orders") != std::string::npos);
        std::cout << " -> Orders table contents:\n" << orders_res << std::endl;

        // Verify default items table is empty
        std::string items_res = engine.execute("SELECT * FROM items;");
        assert(items_res.find("0 rows returned") != std::string::npos);
        std::cout << " -> Table storage isolation confirmed.\n";
    }

    // =========================================================================
    // TEST 4: Dynamic DROP TABLE DDL
    // =========================================================================
    std::cout << "\n[Step 4] Testing DROP TABLE and physical file deletion..." << std::endl;
    {
        pg::Engine engine(db_prefix);

        std::string drop_res = engine.execute("DROP TABLE orders;");
        assert(drop_res.find("successfully dropped") != std::string::npos);
        std::cout << " -> " << drop_res;

        // Verify orders removed from catalog
        assert(!engine.catalog().has_table("orders"));
        std::string tables_str = engine.execute("SHOW TABLES;");
        assert(tables_str.find("orders") == std::string::npos);
        assert(tables_str.find("2 relations") != std::string::npos);

        // Verify querying dropped table returns error
        std::string err_sel = engine.execute("SELECT * FROM orders;");
        assert(err_sel.find("[ERROR] relation \"orders\" does not exist") != std::string::npos);

        // Verify system table items cannot be dropped
        std::string drop_items = engine.execute("DROP TABLE items;");
        assert(drop_items.find("cannot drop system/default relation") != std::string::npos);
        std::cout << " -> System table protection verified.\n";
    }

    // =========================================================================
    // TEST 5: Catalog Persistence & Engine Restart
    // =========================================================================
    std::cout << "\n[Step 5] Testing Catalog persistence across database restarts..." << std::endl;
    {
        // Reopen new Engine instance from the same directory
        pg::Engine engine(db_prefix);

        assert(engine.catalog().has_table("users"));
        std::string users_res = engine.execute("SELECT * FROM users;");
        assert(users_res.find("3 rows returned via Sequential Scan on users") != std::string::npos);
        std::cout << " -> 'users' relation and its 3 tuples survived engine restart intact!\n";
    }

    cleanup();
    std::cout << "\n>>> ITEM 26 (DYNAMIC CATALOGS & MULTI-TABLE DDL) TESTS PASSED! <<<\n" << std::endl;
}

int main() {
    try {
        run_catalog_tests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Catalog test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
