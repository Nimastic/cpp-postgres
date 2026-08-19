#include "pg/http_server.h"
#include "pg/engine.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;

static void clean_test_db(const std::string& prefix) {
    fs::remove(prefix + "_heap.db");
    fs::remove(prefix + "_wal.log");
    fs::remove(prefix + "_clog.db");
    fs::remove(prefix + "_btree.db");
    fs::remove(prefix + "_toast.db");
}

static std::string send_http_request(int port, const std::string& req) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(sock != INVALID_SOCKET);

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    int conn_res = connect(sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr));
    assert(conn_res == 0);

    send(sock, req.data(), static_cast<int>(req.size()), 0);

    std::string response;
    char buf[4096];
    while (true) {
        int bytes = recv(sock, buf, sizeof(buf) - 1, 0);
        if (bytes <= 0) break;
        buf[bytes] = '\0';
        response.append(buf, bytes);
    }
    closesocket(sock);
    return response;
}

int main() {
    std::cout << "--- REPRODUCING ITEM 18: EMBEDDED HTTP/REST API SERVER (APPROACH A) ---\n";
    clean_test_db("test_http_data");

    // Initialize Winsock for client test
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    const int test_port = 18080;
    {
        pg::Engine engine("test_http_data");
        pg::HttpServer server(engine, test_port);

        std::cout << "[Step 1] Starting HTTP Server asynchronously on port " << test_port << "...\n";
        server.start_async();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        assert(server.is_running());

        // 1. Test GET /api/status
        std::cout << "[Step 2] Sending GET /api/status...\n";
        std::string req1 = "GET /api/status HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        std::string resp1 = send_http_request(test_port, req1);
        std::cout << " -> Response:\n" << resp1 << "\n";
        assert(resp1.find("HTTP/1.1 200 OK") != std::string::npos);
        assert(resp1.find("\"status\": \"online\"") != std::string::npos);
        assert(resp1.find("\"engine\": \"cpp-postgres\"") != std::string::npos);
        assert(resp1.find("Access-Control-Allow-Origin: *") != std::string::npos);

        // 2. Test POST /api/sql (INSERT)
        std::cout << "[Step 3] Sending POST /api/sql (INSERT INTO items VALUES (100, 10))...\n";
        std::string sql_insert = "INSERT INTO items VALUES (100, 10);";
        std::string req2 = "POST /api/sql HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + 
                           std::to_string(sql_insert.size()) + "\r\nConnection: close\r\n\r\n" + sql_insert;
        std::string resp2 = send_http_request(test_port, req2);
        std::cout << " -> Response:\n" << resp2 << "\n";
        assert(resp2.find("HTTP/1.1 200 OK") != std::string::npos);
        assert(resp2.find("\"success\": true") != std::string::npos);
        assert(resp2.find("(0, 1)") != std::string::npos);

        // 3. Test POST /api/sql (SELECT)
        std::cout << "[Step 4] Sending POST /api/sql (SELECT * FROM items)...\n";
        std::string sql_select = "SELECT * FROM items;";
        std::string req3 = "POST /api/sql HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + 
                           std::to_string(sql_select.size()) + "\r\nConnection: close\r\n\r\n" + sql_select;
        std::string resp3 = send_http_request(test_port, req3);
        std::cout << " -> Response:\n" << resp3 << "\n";
        assert(resp3.find("100") != std::string::npos);
        assert(resp3.find("$   10") != std::string::npos);

        // 4. Test GET /api/items (Direct JSON Array)
        std::cout << "[Step 5] Sending GET /api/items...\n";
        std::string req4 = "GET /api/items HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        std::string resp4 = send_http_request(test_port, req4);
        std::cout << " -> Response:\n" << resp4 << "\n";
        assert(resp4.find("\"item_id\": 100") != std::string::npos);
        assert(resp4.find("\"price\": 10") != std::string::npos);
        assert(resp4.find("\"ctid\": \"(0, 1)\"") != std::string::npos);

        // 5. Test OPTIONS /api/sql (CORS Pre-Flight)
        std::cout << "[Step 6] Testing CORS OPTIONS pre-flight...\n";
        std::string req5 = "OPTIONS /api/sql HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        std::string resp5 = send_http_request(test_port, req5);
        std::cout << " -> Response:\n" << resp5 << "\n";
        assert(resp5.find("HTTP/1.1 204 No Content") != std::string::npos);
        assert(resp5.find("Access-Control-Allow-Origin: *") != std::string::npos);
        assert(resp5.find("Access-Control-Allow-Methods: GET, POST, OPTIONS") != std::string::npos);

        std::cout << "[Step 7] Stopping server...\n";
        server.stop();
        assert(!server.is_running());
        std::cout << " -> Server stopped cleanly.\n";
    }

    WSACleanup();
    clean_test_db("test_http_data");

    std::cout << "\n>>> ITEM 18 (EMBEDDED HTTP/REST API SERVER) TESTS PASSED SUCCESSFULLY! <<<\n";
    return 0;
}
