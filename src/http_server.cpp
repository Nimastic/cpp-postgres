#include "pg/http_server.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <sstream>
#include <vector>
#include <chrono>

namespace pg {

// Helper to escape strings for JSON
static std::string escape_json(const std::string& s) {
    std::ostringstream o;
    for (char c : s) {
        if (c == '"') o << "\\\"";
        else if (c == '\\') o << "\\\\";
        else if (c == '\b') o << "\\b";
        else if (c == '\f') o << "\\f";
        else if (c == '\n') o << "\\n";
        else if (c == '\r') o << "\\r";
        else if (c == '\t') o << "\\t";
        else if (static_cast<unsigned char>(c) <= 0x1f) {
            o << "\\u00" << std::hex << (static_cast<int>(c) & 0xff);
        } else {
            o << c;
        }
    }
    return o.str();
}

HttpServer::HttpServer(Engine& engine, int port)
    : engine_(engine), port_(port) {
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
}

HttpServer::~HttpServer() {
    stop();
    WSACleanup();
}

void HttpServer::start_async() {
    if (running_.load()) return;
    running_.store(true);
    worker_thread_ = std::thread(&HttpServer::listen_loop, this);
}

void HttpServer::start_blocking() {
    if (running_.load()) return;
    running_.store(true);
    listen_loop();
}

void HttpServer::stop() {
    if (!running_.load()) return;
    running_.store(false);

    if (server_socket_ != static_cast<uintptr_t>(INVALID_SOCKET)) {
        closesocket(static_cast<SOCKET>(server_socket_));
        server_socket_ = static_cast<uintptr_t>(INVALID_SOCKET);
    }

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void HttpServer::listen_loop() {
    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        running_.store(false);
        return;
    }

    server_socket_ = static_cast<uintptr_t>(listen_sock);

    BOOL opt = TRUE;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(static_cast<u_short>(port_));

    if (bind(listen_sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) == SOCKET_ERROR) {
        closesocket(listen_sock);
        server_socket_ = static_cast<uintptr_t>(INVALID_SOCKET);
        running_.store(false);
        return;
    }

    if (listen(listen_sock, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(listen_sock);
        server_socket_ = static_cast<uintptr_t>(INVALID_SOCKET);
        running_.store(false);
        return;
    }

    // Set non-blocking / timeout on accept
    while (running_.load()) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_sock, &read_fds);

        timeval tv{0, 200000}; // 200ms timeout for responsive stop
        int sel_res = select(0, &read_fds, nullptr, nullptr, &tv);
        if (sel_res > 0 && FD_ISSET(listen_sock, &read_fds)) {
            sockaddr_in client_addr{};
            int client_len = sizeof(client_addr);
            SOCKET client_sock = accept(listen_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (client_sock != INVALID_SOCKET) {
                // Handle client request in-thread or detach worker
                handle_client(static_cast<uintptr_t>(client_sock));
            }
        }
    }

    if (server_socket_ != static_cast<uintptr_t>(INVALID_SOCKET)) {
        closesocket(static_cast<SOCKET>(server_socket_));
        server_socket_ = static_cast<uintptr_t>(INVALID_SOCKET);
    }
}

void HttpServer::handle_client(uintptr_t client_socket) {
    SOCKET sock = static_cast<SOCKET>(client_socket);
    std::vector<char> buffer(65536, 0);
    int bytes_received = recv(sock, buffer.data(), static_cast<int>(buffer.size()) - 1, 0);
    if (bytes_received <= 0) {
        closesocket(sock);
        return;
    }

    std::string raw_req(buffer.data(), bytes_received);
    std::istringstream req_stream(raw_req);

    std::string method, path, http_ver;
    req_stream >> method >> path >> http_ver;

    // Parse headers
    std::string line;
    std::getline(req_stream, line); // finish request line
    size_t content_length = 0;
    while (std::getline(req_stream, line) && line != "\r" && !line.empty()) {
        if (line.back() == '\r') line.pop_back();
        if (line.rfind("Content-Length:", 0) == 0 || line.rfind("content-length:", 0) == 0) {
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                content_length = std::stoul(line.substr(colon + 1));
            }
        }
    }

    // Extract body
    std::string body;
    size_t header_end = raw_req.find("\r\n\r\n");
    if (header_end != std::string::npos) {
        body = raw_req.substr(header_end + 4);
    }

    // If body is shorter than content_length, read the rest
    while (body.size() < content_length) {
        int more = recv(sock, buffer.data(), static_cast<int>(buffer.size()) - 1, 0);
        if (more <= 0) break;
        body.append(buffer.data(), more);
    }

    // Process request
    std::string http_response = process_http_request(method, path, body);
    send(sock, http_response.data(), static_cast<int>(http_response.size()), 0);
    closesocket(sock);
}

std::string HttpServer::process_http_request(const std::string& method, const std::string& path, const std::string& body) {
    std::string status_line = "HTTP/1.1 200 OK\r\n";
    std::string content_type = "application/json";
    std::string response_body;

    // Handle CORS Pre-flight
    if (method == "OPTIONS") {
        std::ostringstream resp;
        resp << "HTTP/1.1 204 No Content\r\n"
             << "Access-Control-Allow-Origin: *\r\n"
             << "Access-Control-Allow-Methods: GET, POST, OPTIONS, PUT, DELETE\r\n"
             << "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
             << "Access-Control-Max-Age: 86400\r\n"
             << "Content-Length: 0\r\n"
             << "Connection: close\r\n\r\n";
        return resp.str();
    }

    if (path == "/api/status" && method == "GET") {
        std::lock_guard<std::mutex> lock(engine_mutex_);
        std::ostringstream json;
        json << "{\n"
             << "  \"status\": \"online\",\n"
             << "  \"engine\": \"cpp-postgres\",\n"
             << "  \"heap_pages\": " << engine_.heap().num_pages() << ",\n"
             << "  \"buffer_pool_resident\": " << engine_.bpm().resident_pages() << ",\n"
             << "  \"buffer_pool_size\": " << engine_.bpm().pool_size() << ",\n"
             << "  \"wal_flushed_lsn\": " << engine_.wal().flushed_lsn() << ",\n"
             << "  \"toast_chunks\": " << engine_.toast().total_chunks() << ",\n"
             << "  \"oldest_active_xmin\": " << engine_.tm().oldest_active_xmin() << "\n"
             << "}";
        response_body = json.str();
    } else if ((path == "/api/sql" || path == "/api/query") && method == "POST") {
        // Extract SQL from body (can be raw text or JSON {"sql": "..."})
        std::string sql = body;
        // Basic JSON extraction if body starts with {"sql":
        size_t sql_key = body.find("\"sql\"");
        if (sql_key != std::string::npos) {
            size_t colon = body.find(':', sql_key);
            if (colon != std::string::npos) {
                size_t first_quote = body.find('"', colon);
                if (first_quote != std::string::npos) {
                    size_t second_quote = body.find('"', first_quote + 1);
                    if (second_quote != std::string::npos) {
                        sql = body.substr(first_quote + 1, second_quote - first_quote - 1);
                    }
                }
            }
        }

        std::lock_guard<std::mutex> lock(engine_mutex_);
        // A REST call is stateless, so it gets its own session and therefore its
        // own autocommit transaction rather than joining whatever transaction
        // some other client happens to have open.
        Session session = engine_.new_session();
        std::string engine_out = engine_.execute(sql, session);

        std::ostringstream json;
        json << "{\n"
             << "  \"success\": true,\n"
             << "  \"sql\": \"" << escape_json(sql) << "\",\n"
             << "  \"output\": \"" << escape_json(engine_out) << "\"\n"
             << "}";
        response_body = json.str();
    } else if (path == "/api/items" && method == "GET") {
        std::lock_guard<std::mutex> lock(engine_mutex_);
        auto rows = engine_.heap().seq_scan();
        std::ostringstream json;
        json << "{\n  \"items\": [\n";
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto& [ctid, tuple] = rows[i];
            json << "    {"
                 << "\"item_id\": " << tuple.data.item_id << ", "
                 << "\"price\": " << tuple.data.price << ", "
                 << "\"xmin\": " << tuple.header.xmin << ", "
                 << "\"xmax\": " << tuple.header.xmax << ", "
                 << "\"ctid\": \"(" << ctid.page << ", " << ctid.slot << ")\""
                 << "}" << (i + 1 < rows.size() ? "," : "") << "\n";
        }
        json << "  ]\n}";
        response_body = json.str();
    } else {
        status_line = "HTTP/1.1 404 Not Found\r\n";
        response_body = "{\"error\": \"Endpoint not found\"}";
    }

    std::ostringstream resp;
    resp << status_line
         << "Content-Type: " << content_type << "\r\n"
         << "Access-Control-Allow-Origin: *\r\n"
         << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
         << "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
         << "Content-Length: " << response_body.size() << "\r\n"
         << "Connection: close\r\n\r\n"
         << response_body;

    return resp.str();
}

} // namespace pg
