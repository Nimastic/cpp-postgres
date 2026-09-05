// Regression tests for the buffer-pool, VACUUM and concurrency findings in
// notes/2026-08-27-architecture-audit.md.

#include "pg/engine.h"
#include "pg/buffer_pool.h"
#include "pg/vacuum.h"

#include <iostream>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

namespace {

const std::string PREFIX = "test_conc_db";

void cleanup() {
    for (const char* suffix : {"_heap.db", "_wal.log", "_clog.db", "_toast.db", "_control.db"}) {
        std::remove((PREFIX + suffix).c_str());
    }
}

// Finding 3.1: the clock sweep used to give up after a fixed number of steps, so
// once every frame's usage count exceeded that budget it threw "all frames are
// pinned" while nothing was pinned at all.
void test_clock_sweep_terminates() {
    std::cout << "[3.1] Clock sweep finds a victim however hot the pool is..." << std::endl;
    const char* path = "test_conc_bpm.db";
    std::remove(path);

    const size_t POOL = 4;
    auto pager = pg::Pager::open(path);
    for (size_t i = 0; i < 8; ++i) {
        pager->allocate_page();
        pg::PageBuffer p;
        pager->write_page(static_cast<pg::page_id_t>(i), p.data());
    }

    pg::BufferPoolManager bpm(*pager, POOL);

    // Drive every resident frame's usage count well past the old scan budget.
    for (int round = 0; round < 10; ++round) {
        for (pg::page_id_t i = 0; i < POOL; ++i) {
            bpm.fetch_page(i);
            bpm.unpin_page(i, false);
        }
    }
    assert(bpm.get_pin_count(0) == 0);

    // Nothing is pinned, so eviction must succeed.
    pg::Page* p = bpm.fetch_page(static_cast<pg::page_id_t>(POOL));
    assert(p != nullptr);
    bpm.unpin_page(static_cast<pg::page_id_t>(POOL), false);
    std::cout << "  -> Evicted successfully after " << 10 * POOL << " touches ("
              << bpm.evictions() << " evictions total).\n";

    std::remove(path);
}

// Finding 1.5: VACUUM used to read and write through the Pager while every other
// subsystem went through the pool, so it acted on stale pages and its work was
// overwritten by whichever cached frame flushed last.
void test_vacuum_shares_the_buffer_pool() {
    std::cout << "[1.5] VACUUM sees current data and its work survives..." << std::endl;
    cleanup();

    pg::Engine engine(PREFIX);
    engine.execute("INSERT INTO items VALUES (100, 10);");
    engine.execute("UPDATE items SET price = 20 WHERE item_id = 100;");

    // No checkpoint first. The xmax stamp is still dirty in the pool, and VACUUM
    // must nonetheless see it -- previously this reported 0 reclaimed.
    std::string vac = engine.execute("VACUUM;");
    std::cout << "  " << vac;
    assert(vac.find("redirected 1 HOT roots") != std::string::npos);

    // The live version stays reachable through the redirect left at the root.
    std::string sel = engine.execute("SELECT * FROM items WHERE item_id = 100;");
    assert(sel.find("1 row") != std::string::npos);
    assert(sel.find("$   20") != std::string::npos);
    std::cout << "  -> Row still reachable through the LP_REDIRECT root.\n";

    // And it survives a checkpoint rather than being reverted by a stale frame.
    engine.execute("CHECKPOINT;");
    std::string dump = engine.execute("DUMP PAGE 0;");
    assert(dump.find("REDIRECT") != std::string::npos);
    std::cout << "  -> Redirect persisted through CHECKPOINT (previously reverted to NORMAL).\n";

    cleanup();
}

// Finding 3.4: VACUUM must clean index entries before a slot can be recycled,
// or a stale entry resolves to whatever row later takes the slot.
void test_vacuum_cleans_the_index() {
    std::cout << "[3.4] VACUUM removes index entries before freeing slots..." << std::endl;
    cleanup();

    pg::Engine engine(PREFIX);
    engine.execute("INSERT INTO items VALUES (100, 10);");
    engine.execute("INSERT INTO items VALUES (200, 20);");
    engine.execute("UPDATE items SET price = 99 WHERE item_id = 200;");

    size_t before = engine.index().num_entries();
    std::string vac = engine.execute("VACUUM;");
    std::cout << "  " << vac;
    size_t after = engine.index().num_entries();
    assert(after <= before);

    // Every surviving key must still resolve to its own row.
    std::string a = engine.execute("SELECT * FROM items WHERE item_id = 100;");
    std::string b = engine.execute("SELECT * FROM items WHERE item_id = 200;");
    assert(a.find("$   10") != std::string::npos);
    assert(b.find("$   99") != std::string::npos);
    std::cout << "  -> Index entries: " << before << " -> " << after
              << "; both keys still resolve to their own rows.\n";

    cleanup();
}

// Finding 2.1: transaction state belongs to a session. Two clients must not
// share a transaction.
void test_sessions_are_independent() {
    std::cout << "[2.1] Each session owns its transaction..." << std::endl;
    cleanup();

    pg::Engine engine(PREFIX);
    pg::Session alice = engine.new_session();
    pg::Session bob = engine.new_session();

    engine.execute("INSERT INTO items VALUES (1, 1);", alice);

    // Alice opens a transaction; Bob must remain outside it.
    engine.execute("BEGIN;", alice);
    assert(alice.in_transaction());
    assert(!bob.in_transaction());

    // Bob's autocommitted insert is his own transaction, not Alice's.
    engine.execute("INSERT INTO items VALUES (2, 2);", bob);
    assert(!bob.in_transaction());
    assert(alice.in_transaction());

    engine.execute("COMMIT;", alice);
    assert(!alice.in_transaction());
    std::cout << "  -> Alice's BEGIN/COMMIT never touched Bob's transaction state.\n";

    cleanup();
}

// Finding 2.2: two writers on one row must not both succeed.
void test_row_write_conflicts_are_detected() {
    std::cout << "[2.2] Concurrent writers on one row are serialised..." << std::endl;
    cleanup();

    pg::Engine engine(PREFIX);
    pg::Session alice = engine.new_session();
    pg::Session bob = engine.new_session();

    engine.execute("INSERT INTO items VALUES (500, 5);", alice);

    engine.execute("BEGIN;", alice);
    std::string ok = engine.execute("UPDATE items SET price = 50 WHERE item_id = 500;", alice);
    assert(ok.find("ERROR") == std::string::npos);

    // Bob tries to update the same row while Alice still holds it.
    engine.execute("BEGIN;", bob);
    std::string conflict = engine.execute("UPDATE items SET price = 99 WHERE item_id = 500;", bob);
    std::cout << "  " << conflict;
    assert(conflict.find("could not serialize access") != std::string::npos);

    // Once Alice commits and releases, Bob can proceed.
    engine.execute("COMMIT;", alice);
    engine.execute("ROLLBACK;", bob);
    engine.execute("BEGIN;", bob);
    std::string retry = engine.execute("UPDATE items SET price = 99 WHERE item_id = 500;", bob);
    assert(retry.find("ERROR") == std::string::npos);
    engine.execute("COMMIT;", bob);
    std::cout << "  -> Second writer blocked while the lock was held, allowed after COMMIT.\n";

    cleanup();
}

// Finding 3.3: the VACUUM horizon must come from snapshots, not transaction ids,
// or a transaction holding an old snapshot loses rows underneath it.
void test_vacuum_respects_open_snapshots() {
    std::cout << "[3.3] VACUUM horizon respects an open snapshot..." << std::endl;
    cleanup();

    pg::Engine engine(PREFIX);
    pg::Session writer = engine.new_session();
    pg::Session reader = engine.new_session();

    engine.execute("INSERT INTO items VALUES (700, 7);", writer);

    // Reader opens a transaction and takes a snapshot that can see the row.
    engine.execute("BEGIN;", reader);
    std::string before = engine.execute("SELECT * FROM items WHERE item_id = 700;", reader);
    assert(before.find("1 row") != std::string::npos);

    // Writer replaces it and commits, then VACUUM runs.
    engine.execute("UPDATE items SET price = 77 WHERE item_id = 700;", writer);
    engine.execute("VACUUM;", writer);

    // The reader's snapshot must still see its original version.
    std::string after = engine.execute("SELECT * FROM items WHERE item_id = 700;", reader);
    assert(after.find("1 row") != std::string::npos);
    assert(after.find("$    7") != std::string::npos);
    std::cout << "  -> Open snapshot still sees price 7 after the update and VACUUM.\n";

    engine.execute("COMMIT;", reader);
    cleanup();
}

} // namespace

int main() {
    std::cout << "\n--- BUFFER POOL, VACUUM AND SESSION REGRESSIONS ---" << std::endl;
    try {
        test_clock_sweep_terminates();
        test_vacuum_shares_the_buffer_pool();
        test_vacuum_cleans_the_index();
        test_sessions_are_independent();
        test_row_write_conflicts_are_detected();
        test_vacuum_respects_open_snapshots();
    } catch (const std::exception& e) {
        std::cerr << "Concurrency test failed with exception: " << e.what() << std::endl;
        cleanup();
        return 1;
    }
    cleanup();
    std::cout << "\n>>> CONCURRENCY TESTS PASSED <<<\n" << std::endl;
    return 0;
}
