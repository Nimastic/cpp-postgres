// Regression tests for the durability and recovery findings in
// notes/2026-08-27-architecture-audit.md.
//
// Each block corresponds to a finding that was previously reproducible.

#include "pg/engine.h"
#include "pg/control.h"
#include "pg/wal.h"

#include <iostream>
#include <fstream>
#include <cassert>
#include <cstdio>
#include <string>

namespace {

const std::string PREFIX = "test_recovery_db";

void cleanup() {
    for (const char* suffix : {"_heap.db", "_wal.log", "_clog.db", "_toast.db", "_control.db"}) {
        std::remove((PREFIX + suffix).c_str());
    }
}

long file_size(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    return f ? static_cast<long>(f.tellg()) : -1;
}

// Finding 1.4: a database that did not shut down cleanly must replay the log on
// startup, without an operator invoking anything.
void test_startup_recovery_runs_automatically() {
    std::cout << "[1.4] Crash recovery runs at startup, not on request..." << std::endl;
    cleanup();

    {
        pg::Engine engine(PREFIX);
        engine.execute("INSERT INTO items VALUES (100, 10);");
        engine.execute("INSERT INTO items VALUES (200, 20);");
    }
    {
        // Simulate a crash by putting the control file back into IN_PRODUCTION
        // after the engine has gone. A live engine would just re-mark it clean
        // on the way out.
        pg::ControlFile cf(PREFIX + "_control.db");
        cf.mark_in_production();
    }

    {
        pg::Engine engine(PREFIX);
        assert(engine.recovered_at_startup());
        std::string out = engine.execute("SELECT * FROM items;");
        assert(out.find("100") != std::string::npos);
        assert(out.find("200") != std::string::npos);
        std::cout << "  -> Unclean shutdown detected and replayed automatically; both rows present.\n";
    }

    {
        // A clean shutdown must not trigger recovery next time.
        pg::Engine engine(PREFIX);
        assert(!engine.recovered_at_startup());
        std::cout << "  -> Clean shutdown recorded; the next start skips recovery.\n";
    }
}

// Finding 3.2: recovery's compensation and abort records used to be silently
// dropped, because the log had been read to EOF and the stream refused to write.
void test_undo_records_actually_reach_the_log() {
    std::cout << "[3.2] Undo pass writes CLR and ABORT records..." << std::endl;
    cleanup();

    {
        pg::Engine engine(PREFIX);
        engine.execute("INSERT INTO items VALUES (100, 10);");   // committed
        engine.execute("BEGIN;");
        engine.execute("INSERT INTO items VALUES (200, 20);");   // never committed
    }
    {
        pg::ControlFile cf(PREFIX + "_control.db");
        cf.mark_in_production();
    }

    long before = file_size(PREFIX + "_wal.log");
    {
        pg::Engine engine(PREFIX);
        assert(engine.recovered_at_startup());
    }
    long after = file_size(PREFIX + "_wal.log");

    // The undo pass must append at least a CLR and an ABORT record.
    assert(after > before);
    std::cout << "  -> WAL grew from " << before << " to " << after
              << " bytes during undo (was 0 growth before the fix).\n";

    // And the loser's row must not be visible afterwards.
    {
        pg::Engine engine(PREFIX);
        std::string out = engine.execute("SELECT * FROM items;");
        assert(out.find("100") != std::string::npos);
        assert(out.find("200") == std::string::npos);
        std::cout << "  -> Uncommitted row is gone; committed row survived.\n";
    }
}

// Finding 3.6: the transaction counter used to be re-derived from surviving heap
// data, so an empty table restarted it at 1 and reissued ids that already had a
// commit status recorded against them.
void test_xid_counter_survives_restart() {
    std::cout << "[3.6] Transaction ids are not reused after restart..." << std::endl;
    cleanup();

    pg::tx_id_t last = 0;
    {
        pg::Engine engine(PREFIX);
        for (int i = 0; i < 5; ++i) {
            engine.execute("INSERT INTO items VALUES (" + std::to_string(i) + ", 1);");
        }
        last = engine.tm().next_tx_id();
        assert(last > 1);
    }

    {
        pg::Engine engine(PREFIX);
        assert(engine.tm().next_tx_id() >= last);
        std::cout << "  -> Counter carried across restart: " << last
                  << " -> " << engine.tm().next_tx_id() << ".\n";
    }
}

// Finding 3.5: redo must restore a tuple to the exact slot the record names, or
// index entries end up pointing at the wrong row.
void test_redo_restores_exact_slots() {
    std::cout << "[3.5] Redo restores tuples to their logged slots..." << std::endl;
    cleanup();

    {
        pg::Engine engine(PREFIX);
        for (int i = 1; i <= 6; ++i) {
            engine.execute("INSERT INTO items VALUES (" + std::to_string(i * 100) + ", " +
                           std::to_string(i) + ");");
        }
    }
    {
        pg::ControlFile cf(PREFIX + "_control.db");
        cf.mark_in_production();
    }

    {
        pg::Engine engine(PREFIX);
        // Every key must resolve to a row whose payload matches the key, which
        // only holds if each tuple came back at the CTID the index recorded.
        for (int i = 1; i <= 6; ++i) {
            std::string out = engine.execute("SELECT * FROM items WHERE item_id = " +
                                             std::to_string(i * 100) + ";");
            assert(out.find("1 row") != std::string::npos);
            assert(out.find("$" + std::string(5 - std::to_string(i).size(), ' ') +
                            std::to_string(i)) != std::string::npos);
        }
        std::cout << "  -> All 6 keys resolve to the correct rows after replay.\n";
    }
}

} // namespace

int main() {
    std::cout << "\n--- DURABILITY AND CRASH RECOVERY REGRESSIONS ---" << std::endl;
    try {
        test_startup_recovery_runs_automatically();
        test_undo_records_actually_reach_the_log();
        test_xid_counter_survives_restart();
        test_redo_restores_exact_slots();
    } catch (const std::exception& e) {
        std::cerr << "Recovery test failed with exception: " << e.what() << std::endl;
        cleanup();
        return 1;
    }
    cleanup();
    std::cout << "\n>>> RECOVERY TESTS PASSED <<<\n" << std::endl;
    return 0;
}
