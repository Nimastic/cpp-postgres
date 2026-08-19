#include "pg/engine.h"
#include "pg/http_server.h"
#include "pg/pgwire.h"
#include <iostream>
#include <string>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>

static std::atomic<bool> g_running{true};

void sigint_handler(int) {
    g_running.store(false);
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, sigint_handler);

    int http_port = 8080;
    int pg_port = 5432;
    std::string db_prefix = "pg_server_data";

    if (argc > 1) db_prefix = argv[1];

    std::cout << "====================================================================\n";
    std::cout << "       🐘 CPP-POSTGRES UNIFIED STORAGE ENGINE SERVER DAEMON         \n";
    std::cout << "====================================================================\n";

    pg::Engine engine(db_prefix);

    pg::HttpServer http_server(engine, http_port);
    pg::PgWireServer pgwire_server(engine, pg_port);

    std::cout << "[DAEMON] Starting HTTP REST API Server on port " << http_port << "...\n";
    http_server.start_async();

    std::cout << "[DAEMON] Starting PostgreSQL Wire Protocol Server on port " << pg_port << "...\n";
    pgwire_server.start_async();

    std::cout << "--------------------------------------------------------------------\n";
    std::cout << "  🌐 Approach A (HTTP REST API): http://localhost:" << http_port << "/api/sql\n";
    std::cout << "     • GET  /api/status  - Engine & buffer pool metrics\n";
    std::cout << "     • GET  /api/items   - Structured JSON rows\n";
    std::cout << "     • POST /api/sql     - SQL execution with JSON response\n";
    std::cout << "     • CORS Enabled for direct React / Browser fetch()\n";
    std::cout << "--------------------------------------------------------------------\n";
    std::cout << "  🐘 Approach C (PostgreSQL Wire Protocol): 127.0.0.1:" << pg_port << "\n";
    std::cout << "     • Connect with: psql -h 127.0.0.1 -p " << pg_port << " -U postgres\n";
    std::cout << "     • Compatible with: pgAdmin, DBeaver, Prisma, TypeORM, psycopg2\n";
    std::cout << "====================================================================\n";
    std::cout << "Server daemon running. Type 'quit' or 'exit' or press Ctrl+C to stop.\n\n";

    std::string line;
    while (g_running.load()) {
        if (std::cin.peek() != EOF && std::getline(std::cin, line)) {
            if (line == "exit" || line == "quit" || line == "\\q") {
                break;
            } else if (!line.empty()) {
                std::cout << engine.execute(line) << "\n";
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    std::cout << "\n[DAEMON] Shutting down servers...\n";
    http_server.stop();
    pgwire_server.stop();
    std::cout << "[DAEMON] Servers stopped cleanly. Database persisted to disk.\n";

    return 0;
}
