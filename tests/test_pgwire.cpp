#include "pg/pgwire.h"
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
#include <cstring>

namespace fs = std::filesystem;

static void clean_test_db(const std::string& prefix) {
    fs::remove(prefix + "_heap.db");
    fs::remove(prefix + "_wal.log");
    fs::remove(prefix + "_clog.db");
    fs::remove(prefix + "_btree.db");
    fs::remove(prefix + "_toast.db");
}

static bool read_exact(SOCKET sock, void* dest, size_t len) {
    uint8_t* ptr = reinterpret_cast<uint8_t*>(dest);
    size_t total = 0;
    while (total < len) {
        int bytes = recv(sock, reinterpret_cast<char*>(ptr + total), static_cast<int>(len - total), 0);
        if (bytes <= 0) return false;
        total += static_cast<size_t>(bytes);
    }
    return true;
}

int main() {
    std::cout << "--- REPRODUCING ITEM 19: POSTGRESQL WIRE PROTOCOL SERVER (APPROACH C) ---\n";
    clean_test_db("test_pgwire_data");

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    const int test_port = 15432;
    {
        pg::Engine engine("test_pgwire_data");
        pg::PgWireServer server(engine, test_port);

        std::cout << "[Step 1] Starting PgWireServer on TCP port " << test_port << "...\n";
        server.start_async();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        assert(server.is_running());

        // Connect client TCP socket
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        assert(sock != INVALID_SOCKET);

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(static_cast<u_short>(test_port));
        inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

        int conn = connect(sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr));
        assert(conn == 0);
        std::cout << " -> Client connected to PostgreSQL wire server.\n";

        // 1. Send SSLRequest (len=8, code=80877103)
        std::cout << "[Step 2] Sending SSLRequest probe...\n";
        uint32_t ssl_pkt[2] = { htonl(8), htonl(80877103) };
        send(sock, reinterpret_cast<const char*>(ssl_pkt), 8, 0);

        char ssl_resp = 0;
        assert(read_exact(sock, &ssl_resp, 1));
        std::cout << " -> Server SSL response: '" << ssl_resp << "' (Plaintext supported)\n";
        assert(ssl_resp == 'N');

        // 2. Send StartupMessage (Protocol 3.0)
        std::cout << "[Step 3] Sending Protocol 3.0 StartupMessage (user=postgres)...\n";
        std::vector<uint8_t> start_buf;
        const char raw_params[] = "user\0postgres\0database\0postgres\0\0";
        size_t raw_len = sizeof(raw_params) - 1;
        uint32_t total_len = static_cast<uint32_t>(8 + raw_len);

        uint32_t net_len = htonl(total_len);
        uint32_t proto_ver = htonl(196608); // 3.0
        start_buf.insert(start_buf.end(), reinterpret_cast<uint8_t*>(&net_len), reinterpret_cast<uint8_t*>(&net_len) + 4);
        start_buf.insert(start_buf.end(), reinterpret_cast<uint8_t*>(&proto_ver), reinterpret_cast<uint8_t*>(&proto_ver) + 4);
        start_buf.insert(start_buf.end(), raw_params, raw_params + raw_len);

        send(sock, reinterpret_cast<const char*>(start_buf.data()), static_cast<int>(start_buf.size()), 0);

        // Read handshake packets from server
        bool got_auth = false;
        bool got_ready = false;
        while (!got_ready) {
            char pkt_type = 0;
            assert(read_exact(sock, &pkt_type, 1));
            uint32_t pkt_len = 0;
            assert(read_exact(sock, &pkt_len, 4));
            pkt_len = ntohl(pkt_len);

            std::vector<char> payload(pkt_len - 4, 0);
            if (pkt_len > 4) {
                assert(read_exact(sock, payload.data(), pkt_len - 4));
            }

            if (pkt_type == 'R') {
                std::cout << " -> Received AuthenticationOk ('R')\n";
                got_auth = true;
            } else if (pkt_type == 'S') {
                std::cout << " -> Received ParameterStatus ('S'): " << payload.data() << "\n";
            } else if (pkt_type == 'Z') {
                std::cout << " -> Received ReadyForQuery ('Z') TxStatus: '" << payload[0] << "'\n";
                assert(payload[0] == 'I'); // Idle
                got_ready = true;
            }
        }
        assert(got_auth && got_ready);

        // 3. Send Simple Query ('Q'): INSERT INTO items VALUES (500, 50);
        std::cout << "[Step 4] Sending Query ('Q'): INSERT INTO items VALUES (500, 50);...\n";
        std::string sql_ins = "INSERT INTO items VALUES (500, 50);";
        std::vector<uint8_t> q_ins_pkt;
        q_ins_pkt.push_back('Q');
        uint32_t q_ins_len = htonl(static_cast<uint32_t>(4 + sql_ins.size() + 1));
        q_ins_pkt.insert(q_ins_pkt.end(), reinterpret_cast<uint8_t*>(&q_ins_len), reinterpret_cast<uint8_t*>(&q_ins_len) + 4);
        q_ins_pkt.insert(q_ins_pkt.end(), sql_ins.begin(), sql_ins.end());
        q_ins_pkt.push_back(0);

        send(sock, reinterpret_cast<const char*>(q_ins_pkt.data()), static_cast<int>(q_ins_pkt.size()), 0);

        // Read response
        bool got_ins_complete = false;
        got_ready = false;
        while (!got_ready) {
            char pkt_type = 0;
            assert(read_exact(sock, &pkt_type, 1));
            uint32_t pkt_len = 0;
            assert(read_exact(sock, &pkt_len, 4));
            pkt_len = ntohl(pkt_len);

            std::vector<char> payload(pkt_len - 4, 0);
            if (pkt_len > 4) assert(read_exact(sock, payload.data(), pkt_len - 4));

            if (pkt_type == 'C') {
                std::cout << " -> Received CommandComplete ('C'): " << payload.data() << "\n";
                assert(std::string(payload.data()).find("INSERT") != std::string::npos);
                got_ins_complete = true;
            } else if (pkt_type == 'Z') {
                got_ready = true;
            }
        }
        assert(got_ins_complete && got_ready);

        // 4. Send Simple Query ('Q'): SELECT * FROM items;
        std::cout << "[Step 5] Sending Query ('Q'): SELECT * FROM items;...\n";
        std::string sql_sel = "SELECT * FROM items;";
        std::vector<uint8_t> q_sel_pkt;
        q_sel_pkt.push_back('Q');
        uint32_t q_sel_len = htonl(static_cast<uint32_t>(4 + sql_sel.size() + 1));
        q_sel_pkt.insert(q_sel_pkt.end(), reinterpret_cast<uint8_t*>(&q_sel_len), reinterpret_cast<uint8_t*>(&q_sel_len) + 4);
        q_sel_pkt.insert(q_sel_pkt.end(), sql_sel.begin(), sql_sel.end());
        q_sel_pkt.push_back(0);

        send(sock, reinterpret_cast<const char*>(q_sel_pkt.data()), static_cast<int>(q_sel_pkt.size()), 0);

        bool got_row_desc = false;
        int data_rows_count = 0;
        bool got_sel_complete = false;
        got_ready = false;

        while (!got_ready) {
            char pkt_type = 0;
            assert(read_exact(sock, &pkt_type, 1));
            uint32_t pkt_len = 0;
            assert(read_exact(sock, &pkt_len, 4));
            pkt_len = ntohl(pkt_len);

            std::vector<char> payload(pkt_len - 4, 0);
            if (pkt_len > 4) assert(read_exact(sock, payload.data(), pkt_len - 4));

            if (pkt_type == 'T') {
                std::cout << " -> Received RowDescription ('T') packet!\n";
                got_row_desc = true;
            } else if (pkt_type == 'D') {
                std::cout << " -> Received DataRow ('D') packet!\n";
                data_rows_count++;
            } else if (pkt_type == 'C') {
                std::cout << " -> Received CommandComplete ('C'): " << payload.data() << "\n";
                got_sel_complete = true;
            } else if (pkt_type == 'Z') {
                got_ready = true;
            }
        }
        assert(got_row_desc && data_rows_count == 1 && got_sel_complete && got_ready);

        // 5. Send Terminate ('X')
        std::cout << "[Step 6] Sending Terminate ('X')...\n";
        uint32_t term_len = htonl(4);
        char term_pkt[5] = { 'X', 0, 0, 0, 4 };
        std::memcpy(&term_pkt[1], &term_len, 4);
        send(sock, term_pkt, 5, 0);
        closesocket(sock);

        std::cout << "[Step 7] Stopping PgWireServer...\n";
        server.stop();
        assert(!server.is_running());
        std::cout << " -> Server stopped cleanly.\n";
    }

    WSACleanup();
    clean_test_db("test_pgwire_data");

    std::cout << "\n>>> ITEM 19 (POSTGRESQL WIRE PROTOCOL SERVER) TESTS PASSED SUCCESSFULLY! <<<\n";
    return 0;
}
