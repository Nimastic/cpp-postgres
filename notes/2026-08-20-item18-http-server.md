# Study Notes: Item 18 — Embedded HTTP/REST API Server (Approach A)

**Date**: 2026-08-20  
**Source Code**: [include/pg/http_server.h](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/include/pg/http_server.h), [src/http_server.cpp](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/src/http_server.cpp), [tests/test_http_server.cpp](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/tests/test_http_server.cpp)

---

## 1. Overview & Architecture

Item 18 implements **Approach A: 2-Tier Embedded HTTP/REST Architecture**.
Instead of requiring an intermediary Node.js, Python, or Go backend service, the database engine itself embeds a non-blocking Winsock2 HTTP 1.1 server.

```text
┌────────────────────────────────────────────────────────────────────────┐
│ REACT / WEB BROWSER                                                    │
│  fetch("http://localhost:8080/api/sql", {                              │
│    method: "POST",                                                     │
│    body: "INSERT INTO items VALUES (100, 10);"                         │
│  })                                                                    │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │ HTTP/1.1 (JSON)
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│ cpp-postgres HttpServer (Winsock2 Port 8080)                           │
│  ├── CORS Pre-flight Handler (`OPTIONS /*` -> HTTP 204)                │
│  ├── Engine Status Handler (`GET /api/status` -> JSON metrics)         │
│  ├── Direct Item Scanner (`GET /api/items` -> JSON row array)          │
│  └── SQL Execution Handler (`POST /api/sql` -> JSON {success, output}) │
│                          │                                             │
│                          ▼ (std::mutex thread-safe execution)          │
│                 ┌─────────────────┐                                    │
│                 │   pg::Engine    │                                    │
│                 └─────────────────┘                                    │
└────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Invariants & Implementation Details

1. **CORS (Cross-Origin Resource Sharing)**:
   - Modern browsers block cross-origin requests from frontend apps (e.g. `http://localhost:3000` or `file:///`) to `http://localhost:8080`.
   - `HttpServer` automatically intercepts `OPTIONS` pre-flight requests and responds with:
     - `Access-Control-Allow-Origin: *`
     - `Access-Control-Allow-Methods: GET, POST, OPTIONS, PUT, DELETE`
     - `Access-Control-Allow-Headers: Content-Type, Authorization`
2. **Thread Safety & Multi-Client Synchronization**:
   - `HttpServer` runs its listener on a background worker thread (`start_async()`).
   - Sockets use `select()` with a 200ms timeout to allow graceful, immediate stopping (`stop()`).
   - Calls into `pg::Engine` are protected by `std::mutex engine_mutex_` to prevent concurrent race conditions on shared buffers or page latching.
3. **Structured JSON Output**:
   - String fields are properly escaped (`\n`, `\r`, `\"`, `\\`) to prevent malformed JSON responses.
   - `GET /api/items` parses the slotted page rows directly and returns structured JSON arrays:
     ```json
     {
       "items": [
         { "item_id": 100, "price": 10, "xmin": 1, "xmax": 0, "ctid": "(0, 1)" }
       ]
     }
     ```

---

## 3. Verification & Diagnostics

Run integration test:
```powershell
.\build\test_http_server.exe
```

Test Results:
- `[Step 1]`: Asynchronous HTTP listener started on port `18080`.
- `[Step 2]`: `GET /api/status` returned HTTP 200 OK with online metrics.
- `[Step 3]`: `POST /api/sql` executed `INSERT INTO items VALUES (100, 10);` with `"success": true`.
- `[Step 4]`: `POST /api/sql` executed `SELECT * FROM items;` returning formatted ASCII table.
- `[Step 5]`: `GET /api/items` returned JSON array `[{"item_id":100,"price":10,"xmin":1,"xmax":0,"ctid":"(0, 1)"}]`.
- `[Step 6]`: `OPTIONS /api/sql` returned HTTP 204 No Content with `Access-Control-Allow-Origin: *`.
- `[Step 7]`: Server stopped cleanly.

---

## 4. Learner Ladder (Three-Depth Quiz)

### Question 1 (Recall — CORS)
Why must an embedded database HTTP server return `Access-Control-Allow-Origin: *` in response to `OPTIONS` requests?
1. To encrypt incoming database credentials over SSL.
2. Because web browsers execute a pre-flight CORS check to ensure the backend allows cross-origin requests from web frontends running on different ports/domains.
3. To tell the operating system to open port 8080.

### Question 2 (Mechanics — Thread Safety)
Why must the HTTP server acquire an `engine_mutex_` lock before executing queries against `pg::Engine`?
1. Because the `Engine` contains shared state (BufferPoolManager frames, WAL flush LSN, CLOG pages) that would suffer race conditions and memory corruption if concurrent HTTP worker threads modified them without synchronization.
2. Because Winsock sockets require mutexes to send data.
3. Because HTTP headers are not thread-safe.

### Question 3 (Trap/Bug — Statelessness)
If a frontend web app sends `POST /api/sql` with `"BEGIN;"` in Request 1, and then sends `"INSERT INTO items VALUES (100, 10);"` in Request 2 over HTTP:
1. Without a session/cookie token mechanism linking HTTP connections to a persistent backend transaction context, each request executes independently and uncommitted state could be lost or autocommitted.
2. The operating system automatically links all HTTP connections from the same IP into a single ACID transaction.
3. HTTP 1.1 automatically holds the database transaction lock forever until the browser tab closes.
