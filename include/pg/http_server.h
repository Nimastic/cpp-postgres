#pragma once

#include "pg/engine.h"
#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdint>

namespace pg {

// Embedded HTTP/REST API Server (Approach A)
// Exposes REST/JSON endpoints over HTTP with CORS for direct web integration.
class HttpServer {
public:
    explicit HttpServer(Engine& engine, int port = 8080);
    ~HttpServer();

    // Disable copy
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

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
    void handle_client(uintptr_t client_socket);
    std::string process_http_request(const std::string& method, const std::string& path, const std::string& body);

    Engine& engine_;
    int port_;
    std::atomic<bool> running_{false};
    std::thread worker_thread_;
    uintptr_t server_socket_{~static_cast<uintptr_t>(0)}; // INVALID_SOCKET
    std::mutex engine_mutex_;
};

} // namespace pg
