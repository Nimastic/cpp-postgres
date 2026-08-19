# Item 19: PostgreSQL Wire Protocol Server (Approach C)

**Confidence**: `verified`  
**Citations**: [include/pg/pgwire.h:1-55](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/include/pg/pgwire.h), [src/pgwire.cpp:1-320](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/src/pgwire.cpp), [tests/test_pgwire.cpp:1-160](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/tests/test_pgwire.cpp)

---

## 1. Enterprise 3-Tier Integration Architecture

Item 19 implements **Approach C: Native PostgreSQL Frontend/Backend Protocol v3.0**.
Listening on TCP port `5432`, `PgWireServer` transforms `cpp-postgres` into a drop-in PostgreSQL server compatible with standard PostgreSQL CLI tools, GUI clients, drivers, and ORMs.

```mermaid
flowchart LR
    subgraph Ecosystem["Standard PostgreSQL Ecosystem"]
        PSQL["psql CLI"]
        PGADMIN["pgAdmin / DBeaver"]
        NODE["Node.js ('pg' / Prisma)"]
        PYTHON["Python ('psycopg2')"]
    end

    subgraph PgWire["PgWireServer (Port 5432)"]
        SSL["SSLRequest ('N' Plaintext)"]
        AUTH["Startup / AuthOk ('R')"]
        QUERY["Simple Query ('Q') Engine"]
        ROWDESC["RowDescription ('T')"]
        DATAROW["DataRow ('D')"]
        COMPLETE["CommandComplete ('C')"]
        READY["ReadyForQuery ('Z')"]
    end

    subgraph Storage["pg::Engine"]
        CORE["Relational Engine"]
    end

    Ecosystem -->|Binary TCP Packets| SSL
    SSL --> AUTH --> QUERY
    QUERY --> ROWDESC --> DATAROW --> COMPLETE --> READY
    QUERY <--> CORE
```

---

## 2. Invariants & Packet Framing

1. **Big-Endian Integer Serialization**: All 16-bit and 32-bit fields are transmitted in Network Byte Order (`htonl`/`htons`) (`[src/pgwire.cpp:15]`).
2. **Binary Message Format**:
   - StartupMessage -> AuthenticationOk (R, len 8, 0) -> ParameterStatus (S) -> ReadyForQuery (Z, I).
   - Query (Q) -> RowDescription (T) -> DataRow (D)* -> CommandComplete (C) -> ReadyForQuery (Z).
3. **Transaction State Synchronization**: The ReadyForQuery (Z) packet transmits I when idle and T when an active transaction block is open.

---

## 3. Sequence Diagram: PostgreSQL Wire Protocol Handshake & Query

```mermaid
sequenceDiagram
    autonumber
    participant Client as psql Client Driver
    participant Server as PgWireServer (src/pgwire.cpp)
    participant Engine as pg::Engine Core

    Note over Client,Server: Connection Handshake
    Client->>Server: SSLRequest (len 8, code 80877103)
    Server-->>Client: Plaintext response N
    Client->>Server: StartupMessage (Protocol 3.0, user postgres)
    Server-->>Client: AuthenticationOk (R, len 8, 0)
    Server-->>Client: ParameterStatus (S, server_version 16.0)
    Server-->>Client: ReadyForQuery (Z, Idle)

    Note over Client,Server: Query Execution Lifecycle
    Client->>Server: Query (Q, SELECT star FROM items)
    Server->>Engine: execute select all items
    Server-->>Client: RowDescription (T, 5 columns: item_id, price, xmin, xmax, ctid)
    Server-->>Client: DataRow (D, values: 100, 10, 1, 0, 0:1)
    Server-->>Client: CommandComplete (C, SELECT 1)
    Server-->>Client: ReadyForQuery (Z, Idle)

    Note over Client,Server: Graceful Termination
    Client->>Server: Terminate (X, len 4)
    Server->>Server: Close Socket
```
