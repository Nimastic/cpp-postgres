#pragma once

#include "pg/engine.h"
#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <cstdint>

namespace pg {

// PostgreSQL Wire Protocol Server (Approach C)
// Implements PostgreSQL Frontend/Backend Protocol v3.0 on TCP Port 5432.
// Allows standard psql, pgAdmin, DBeaver, Prisma, TypeORM, and psycopg2 to connect.
class PgWireServer {
public:
    explicit PgWireServer(Engine& engine, int port = 5432);
    ~PgWireServer();

    // Disable copy
    PgWireServer(const PgWireServer&) = delete;
    PgWireServer& operator=(const PgWireServer&) = delete;

    // Start server asynchronously on a background worker thread
    void start_async();

    // Start server synchronously (blocks calling thread until stopped)
    void start_blocking();

    // Stop server and clean up sockets
    void stop();

    // Query server state
    bool is_running() const { return running_.load(); }
    int port() const { return port_; }

private:
    void listen_loop();
    void handle_client_session(uintptr_t client_socket);

    // Protocol helper methods
    bool handle_startup_handshake(uintptr_t sock);
    bool process_query_packet(uintptr_t sock, const std::string& query);
    void send_ready_for_query(uintptr_t sock, char tx_status);
    void send_command_complete(uintptr_t sock, const std::string& tag);
    void send_error_response(uintptr_t sock, const std::string& msg);

    Engine& engine_;
    int port_;
    std::atomic<bool> running_{false};
    std::thread worker_thread_;
    uintptr_t server_socket_{~static_cast<uintptr_t>(0)}; // INVALID_SOCKET
    std::mutex engine_mutex_;
};

} // namespace pg
