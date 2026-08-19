#include "pg/pgwire.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <sstream>
#include <vector>
#include <cstring>
#include <algorithm>

namespace pg {

// Packet Serialization Helpers
static void write_int32(std::vector<uint8_t>& buf, int32_t val) {
    uint32_t net = htonl(static_cast<uint32_t>(val));
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&net);
    buf.insert(buf.end(), p, p + 4);
}

static void write_int16(std::vector<uint8_t>& buf, int16_t val) {
    uint16_t net = htons(static_cast<uint16_t>(val));
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&net);
    buf.insert(buf.end(), p, p + 2);
}

static void write_string(std::vector<uint8_t>& buf, const std::string& str) {
    buf.insert(buf.end(), str.begin(), str.end());
    buf.push_back(0); // Null terminator
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

PgWireServer::PgWireServer(Engine& engine, int port)
    : engine_(engine), port_(port) {
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
}

PgWireServer::~PgWireServer() {
    stop();
    WSACleanup();
}

void PgWireServer::start_async() {
    if (running_.load()) return;
    running_.store(true);
    worker_thread_ = std::thread(&PgWireServer::listen_loop, this);
}

void PgWireServer::start_blocking() {
    if (running_.load()) return;
    running_.store(true);
    listen_loop();
}

void PgWireServer::stop() {
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

void PgWireServer::listen_loop() {
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

    while (running_.load()) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_sock, &read_fds);

        timeval tv{0, 200000}; // 200ms select timeout
        int sel_res = select(0, &read_fds, nullptr, nullptr, &tv);
        if (sel_res > 0 && FD_ISSET(listen_sock, &read_fds)) {
            sockaddr_in client_addr{};
            int client_len = sizeof(client_addr);
            SOCKET client_sock = accept(listen_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (client_sock != INVALID_SOCKET) {
                // Handle client session in a detached thread or inline
                std::thread([this, client_sock]() {
                    handle_client_session(static_cast<uintptr_t>(client_sock));
                }).detach();
            }
        }
    }

    if (server_socket_ != static_cast<uintptr_t>(INVALID_SOCKET)) {
        closesocket(static_cast<SOCKET>(server_socket_));
        server_socket_ = static_cast<uintptr_t>(INVALID_SOCKET);
    }
}

void PgWireServer::handle_client_session(uintptr_t client_socket) {
    SOCKET sock = static_cast<SOCKET>(client_socket);

    if (!handle_startup_handshake(client_socket)) {
        closesocket(sock);
        return;
    }

    // Message loop
    while (running_.load()) {
        char msg_type = 0;
        if (!read_exact(sock, &msg_type, 1)) break;

        uint32_t net_len = 0;
        if (!read_exact(sock, &net_len, 4)) break;
        uint32_t len = ntohl(net_len);

        if (len < 4) break;
        uint32_t payload_len = len - 4;
        std::vector<char> payload(payload_len, 0);
        if (payload_len > 0) {
            if (!read_exact(sock, payload.data(), payload_len)) break;
        }

        if (msg_type == 'Q') { // Simple Query
            std::string query(payload.data(), payload.size());
            if (!query.empty() && query.back() == '\0') query.pop_back();
            process_query_packet(client_socket, query);
        } else if (msg_type == 'X') { // Terminate
            break;
        } else {
            // Unhandled message: respond with ReadyForQuery to keep sync
            send_ready_for_query(client_socket, 'I');
        }
    }

    closesocket(sock);
}

bool PgWireServer::handle_startup_handshake(uintptr_t sock_ptr) {
    SOCKET sock = static_cast<SOCKET>(sock_ptr);

    while (true) {
        uint32_t net_len = 0;
        if (!read_exact(sock, &net_len, 4)) return false;
        uint32_t len = ntohl(net_len);

        if (len < 8) return false;

        uint32_t net_code = 0;
        if (!read_exact(sock, &net_code, 4)) return false;
        uint32_t code = ntohl(net_code);

        if (code == 80877103) { // SSLRequest (0x04D2162F)
            // Respond with 'N' (No SSL supported, fallback to plaintext)
            char no_ssl = 'N';
            send(sock, &no_ssl, 1, 0);
            continue; // Next packet will be the real StartupMessage
        } else if (code == 196608) { // Protocol 3.0 (0x00030000)
            uint32_t remaining = len - 8;
            if (remaining > 0) {
                std::vector<char> params(remaining, 0);
                if (!read_exact(sock, params.data(), remaining)) return false;
            }

            // 1. Send AuthenticationOk ('R', len 8, auth_code 0)
            std::vector<uint8_t> auth_pkt;
            auth_pkt.push_back('R');
            write_int32(auth_pkt, 8);
            write_int32(auth_pkt, 0); // AUTH_REQ_OK
            send(sock, reinterpret_cast<const char*>(auth_pkt.data()), static_cast<int>(auth_pkt.size()), 0);

            // 2. Send ParameterStatus ('S')
            auto send_param = [&](const std::string& name, const std::string& val) {
                std::vector<uint8_t> p_pkt;
                p_pkt.push_back('S');
                int32_t pkt_len = static_cast<int32_t>(4 + name.size() + 1 + val.size() + 1);
                write_int32(p_pkt, pkt_len);
                write_string(p_pkt, name);
                write_string(p_pkt, val);
                send(sock, reinterpret_cast<const char*>(p_pkt.data()), static_cast<int>(p_pkt.size()), 0);
            };

            send_param("server_version", "16.0 (cpp-postgres)");
            send_param("client_encoding", "UTF8");
            send_param("server_encoding", "UTF8");
            send_param("DateStyle", "ISO, MDY");
            send_param("integer_datetimes", "on");

            // 3. Send ReadyForQuery ('Z')
            send_ready_for_query(sock_ptr, 'I');
            return true;
        } else {
            return false;
        }
    }
}

bool PgWireServer::process_query_packet(uintptr_t sock_ptr, const std::string& query) {
    SOCKET sock = static_cast<SOCKET>(sock_ptr);

    std::string q_upper = query;
    std::transform(q_upper.begin(), q_upper.end(), q_upper.begin(), ::toupper);

    std::lock_guard<std::mutex> lock(engine_mutex_);

    // Check if query is a SELECT
    if (q_upper.find("SELECT") != std::string::npos) {
        // Collect rows
        std::vector<std::pair<CTID, HeapTuple>> result_rows;

        // Check if query is index lookup (SELECT ... WHERE item_id = X)
        size_t where_pos = q_upper.find("WHERE");
        size_t eq_pos = q_upper.find("=", where_pos);
        if (where_pos != std::string::npos && eq_pos != std::string::npos) {
            std::string id_str = q_upper.substr(eq_pos + 1);
            size_t semi = id_str.find(';');
            if (semi != std::string::npos) id_str = id_str.substr(0, semi);
            try {
                int32_t target_id = std::stoi(id_str);
                auto candidates = engine_.index().find_entries(target_id);
                for (const auto& ctid : candidates) {
                    auto tuple_opt = engine_.heap().get(ctid);
                    if (tuple_opt.has_value() && tuple_opt->data.item_id == target_id) {
                        result_rows.emplace_back(ctid, *tuple_opt);
                    }
                }
            } catch (...) {
                result_rows = engine_.heap().seq_scan();
            }
        } else {
            result_rows = engine_.heap().seq_scan();
        }

        // 1. Send RowDescription ('T')
        // Columns: item_id (INT4/23), price (INT4/23), xmin (INT4/23), xmax (INT4/23), ctid (TEXT/25)
        struct ColInfo { std::string name; int32_t type_oid; int16_t type_size; };
        std::vector<ColInfo> cols = {
            {"item_id", 23, 4},
            {"price", 23, 4},
            {"xmin", 23, 4},
            {"xmax", 23, 4},
            {"ctid", 25, -1}
        };

        std::vector<uint8_t> t_pkt;
        t_pkt.push_back('T');
        size_t len_pos = t_pkt.size();
        write_int32(t_pkt, 0); // placeholder length
        write_int16(t_pkt, static_cast<int16_t>(cols.size())); // 5 columns

        for (size_t i = 0; i < cols.size(); ++i) {
            write_string(t_pkt, cols[i].name);
            write_int32(t_pkt, 0); // table OID
            write_int16(t_pkt, static_cast<int16_t>(i + 1)); // attr num
            write_int32(t_pkt, cols[i].type_oid); // type OID
            write_int16(t_pkt, cols[i].type_size); // type size
            write_int32(t_pkt, -1); // type mod
            write_int16(t_pkt, 0); // format (0 = text)
        }

        // Patch length
        uint32_t t_len = htonl(static_cast<uint32_t>(t_pkt.size() - 1));
        std::memcpy(&t_pkt[len_pos], &t_len, 4);
        send(sock, reinterpret_cast<const char*>(t_pkt.data()), static_cast<int>(t_pkt.size()), 0);

        // 2. Send DataRow ('D') for each tuple
        for (const auto& [ctid, tuple] : result_rows) {
            std::vector<uint8_t> d_pkt;
            d_pkt.push_back('D');
            size_t d_len_pos = d_pkt.size();
            write_int32(d_pkt, 0); // placeholder length
            write_int16(d_pkt, static_cast<int16_t>(cols.size()));

            auto write_col_val = [&](const std::string& val_str) {
                write_int32(d_pkt, static_cast<int32_t>(val_str.size()));
                d_pkt.insert(d_pkt.end(), val_str.begin(), val_str.end());
            };

            write_col_val(std::to_string(tuple.data.item_id));
            write_col_val(std::to_string(tuple.data.price));
            write_col_val(std::to_string(tuple.header.xmin));
            write_col_val(std::to_string(tuple.header.xmax));
            write_col_val(ctid.to_string());

            uint32_t d_len = htonl(static_cast<uint32_t>(d_pkt.size() - 1));
            std::memcpy(&d_pkt[d_len_pos], &d_len, 4);
            send(sock, reinterpret_cast<const char*>(d_pkt.data()), static_cast<int>(d_pkt.size()), 0);
        }

        // 3. Send CommandComplete ('C')
        send_command_complete(sock_ptr, "SELECT " + std::to_string(result_rows.size()));
    } else {
        // Run general command through Engine (INSERT, UPDATE, BEGIN, COMMIT, ROLLBACK, VACUUM, etc.)
        std::string out = engine_.execute(query);

        if (q_upper.find("INSERT") != std::string::npos) {
            send_command_complete(sock_ptr, "INSERT 0 1");
        } else if (q_upper.find("UPDATE") != std::string::npos) {
            send_command_complete(sock_ptr, "UPDATE 1");
        } else if (q_upper.find("BEGIN") != std::string::npos) {
            send_command_complete(sock_ptr, "BEGIN");
        } else if (q_upper.find("COMMIT") != std::string::npos) {
            send_command_complete(sock_ptr, "COMMIT");
        } else if (q_upper.find("ROLLBACK") != std::string::npos) {
            send_command_complete(sock_ptr, "ROLLBACK");
        } else {
            send_command_complete(sock_ptr, "OK");
        }
    }

    // Determine transaction status
    char tx_st = engine_.is_in_transaction() ? 'T' : 'I';
    send_ready_for_query(sock_ptr, tx_st);
    return true;
}

void PgWireServer::send_command_complete(uintptr_t sock_ptr, const std::string& tag) {
    SOCKET sock = static_cast<SOCKET>(sock_ptr);
    std::vector<uint8_t> pkt;
    pkt.push_back('C');
    int32_t pkt_len = static_cast<int32_t>(4 + tag.size() + 1);
    write_int32(pkt, pkt_len);
    write_string(pkt, tag);
    send(sock, reinterpret_cast<const char*>(pkt.data()), static_cast<int>(pkt.size()), 0);
}

void PgWireServer::send_ready_for_query(uintptr_t sock_ptr, char tx_status) {
    SOCKET sock = static_cast<SOCKET>(sock_ptr);
    std::vector<uint8_t> pkt;
    pkt.push_back('Z');
    write_int32(pkt, 5);
    pkt.push_back(static_cast<uint8_t>(tx_status)); // 'I' (Idle) or 'T' (Transaction)
    send(sock, reinterpret_cast<const char*>(pkt.data()), static_cast<int>(pkt.size()), 0);
}

void PgWireServer::send_error_response(uintptr_t sock_ptr, const std::string& msg) {
    SOCKET sock = static_cast<SOCKET>(sock_ptr);
    std::vector<uint8_t> pkt;
    pkt.push_back('E');
    size_t len_pos = pkt.size();
    write_int32(pkt, 0); // placeholder length

    pkt.push_back('S'); write_string(pkt, "ERROR");
    pkt.push_back('C'); write_string(pkt, "42601"); // Syntax error
    pkt.push_back('M'); write_string(pkt, msg);
    pkt.push_back(0); // terminating zero

    uint32_t e_len = htonl(static_cast<uint32_t>(pkt.size() - 1));
    std::memcpy(&pkt[len_pos], &e_len, 4);
    send(sock, reinterpret_cast<const char*>(pkt.data()), static_cast<int>(pkt.size()), 0);
}

} // namespace pg
