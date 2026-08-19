# Study Notes: Item 19 — PostgreSQL Wire Protocol Server (Approach C)

**Date**: 2026-08-20  
**Source Code**: [include/pg/pgwire.h](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/include/pg/pgwire.h), [src/pgwire.cpp](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/src/pgwire.cpp), [tests/test_pgwire.cpp](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/tests/test_pgwire.cpp)

---

## 1. Overview & Architecture

Item 19 implements **Approach C: PostgreSQL Frontend/Backend Protocol v3.0**.
By implementing the native PostgreSQL wire protocol over TCP port `5432`, our storage engine becomes a true drop-in PostgreSQL database server capable of interfacing directly with standard PostgreSQL tools, drivers, and ORMs.

```text
┌────────────────────────────────────────────────────────────────────────┐
│ CLIENT (psql / pgAdmin / DBeaver / Node 'pg' / Prisma / psycopg2)      │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │ TCP Port 5432 (Postgres Protocol v3.0)
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│ cpp-postgres PgWireServer (Winsock2 Port 5432)                         │
│  ├── 1. SSLRequest ('N' fallback to plaintext)                         │
│  ├── 2. StartupMessage (user/database handshake)                       │
│  │     └── Send AuthenticationOk ('R', len 8, 0)                       │
│  │     └── Send ParameterStatus ('S', server_version 16.0, UTF8)       │
│  │     └── Send ReadyForQuery ('Z', 'I')                               │
│  ├── 3. Simple Query ('Q', len, SQL string)                            │
│  │     ├── SELECT: RowDescription ('T') + DataRow ('D')* + Complete   │
│  │     ├── DML/Tx: CommandComplete ('C', "INSERT 0 1" / "COMMIT")      │
│  │     └── Sync: ReadyForQuery ('Z', 'I' or 'T')                       │
│  └── 4. Terminate ('X') -> Graceful socket close                       │
│                          │                                             │
│                          ▼ (std::mutex thread-safe execution)          │
│                 ┌─────────────────┐                                    │
│                 │   pg::Engine    │                                    │
│                 └─────────────────┘                                    │
└────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Invariants & Implementation Details

1. **Big-Endian Network Byte Order**:
   - All 16-bit and 32-bit integer fields in PostgreSQL packets are transmitted in **Network Byte Order (Big-Endian)**.
   - We use `htonl()` / `htons()` when serializing output packets and `ntohl()` / `ntohs()` when decoding incoming packets.
2. **Packet Framing**:
   - All backend messages start with a 1-byte type identifier followed by a 4-byte total length (which includes the 4 bytes of the length field itself):
     - `'R'`: Authentication request (`0` = Auth OK).
     - `'S'`: Server parameter key-value pair.
     - `'Z'`: Ready for query indicator (`'I'` idle, `'T'` in transaction).
     - `'T'`: Row description (schema definition of result columns).
     - `'D'`: Data row (column count + byte lengths + ASCII values).
     - `'C'`: Command completion tag (`SELECT 1`, `INSERT 0 1`, `COMMIT`).
     - `'E'`: Error response.
3. **SSL Negotiation**:
   - Standard PostgreSQL clients first send an 8-byte `SSLRequest` probe (`code == 80877103`).
   - The server replies with `'N'` (No SSL), causing the client to immediately send its plaintext `StartupMessage` (`code == 196608` for protocol 3.0).

---

## 3. Verification & Diagnostics

Run integration test:
```powershell
.\build\test_pgwire.exe
```

Test Results:
- `[Step 1]`: Asynchronous `PgWireServer` started on TCP port `15432`.
- `[Step 2]`: `SSLRequest` probe received; server responded with `'N'`.
- `[Step 3]`: `StartupMessage` received; server completed handshake by sending `AuthenticationOk ('R')`, `ParameterStatus ('S')`, and `ReadyForQuery ('Z')`.
- `[Step 4]`: Query `'Q'` (`INSERT INTO items VALUES (500, 50);`) received; executed and returned `CommandComplete ('C': INSERT 0 1)`.
- `[Step 5]`: Query `'Q'` (`SELECT * FROM items;`) received; executed and returned `RowDescription ('T')`, `DataRow ('D')`, and `CommandComplete ('C': SELECT 1)`.
- `[Step 6]`: Client sent `Terminate ('X')`; server closed session cleanly.
- `[Step 7]`: Server stopped cleanly.

---

## 4. Learner Ladder (Three-Depth Quiz)

### Question 1 (Recall — Network Byte Order)
What byte order does the PostgreSQL frontend/backend protocol mandate for all multi-byte integers?
1. Little-Endian (x86 native).
2. Big-Endian (Network Byte Order, MSB first).
3. Mixed PDP-endian.

### Question 2 (Mechanics — Query Synchronization)
What packet must the PostgreSQL server send after completing query execution and results streaming to inform the client that it is ready to receive the next SQL query?
1. `ReadyForQuery` (`'Z'`), containing a 1-byte transaction indicator (`'I'` for idle, `'T'` for active transaction block).
2. `AuthenticationOk` (`'R'`).
3. `RowDescription` (`'T'`).

### Question 3 (Trap/Bug — SSL Probe Handling)
If a custom PostgreSQL server does not handle the initial 8-byte `SSLRequest` packet and instead expects a `StartupMessage` directly:
1. Standard clients (like `psql` or `pgAdmin`) will hang or immediately disconnect because they wait for the server's single-byte `'S'` or `'N'` response to the SSL probe before sending the `StartupMessage`.
2. The operating system will automatically upgrade the connection to HTTPS.
3. The client will ignore the error and proceed without any issue.
