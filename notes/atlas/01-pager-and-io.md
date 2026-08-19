# Item 1: Pager & 8KB Fixed Page Disk I/O

**Confidence**: `verified`  
**Citations**: [include/pg/pager.h:1-62](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/include/pg/pager.h), [src/pager.cpp:1-85](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/src/pager.cpp), [tests/test_pager.cpp:1-95](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/tests/test_pager.cpp)

---

## 1. Architectural Role

The **Pager** is the lowest-level storage primitive in the database engine. It abstracts the host operating system's filesystem into a linear sequence of fixed-size **8,192-byte (8KB)** disk pages indexed by a 32-bit `page_id_t`.

```mermaid
flowchart LR
    subgraph DiskFile["Physical Database File (*.db)"]
        P0["Page 0\n[0 .. 8191 B]"]
        P1["Page 1\n[8192 .. 16383 B]"]
        P2["Page 2\n[16384 .. 24575 B]"]
        PN["Page N\n[N*8192 .. (N+1)*8192-1 B]"]
    end

    subgraph PagerSubsystem["Pager (src/pager.cpp)"]
        CALC["Offset Calculation:\noffset = page_id * 8192"]
        SEEK["seekg() / seekp()"]
        IO["read() / write()"]
    end

    CALC --> SEEK --> IO <--> DiskFile
```

---

## 2. Invariants & Formulas

1. **Exact Page Boundary Alignment**:
   $$\text{file\_offset} = \text{page\_id} \times 8192$$
   Every disk read and write transfers exactly $8,192\text{ bytes}$. No partial page I/O is ever permitted.
2. **Deterministic File Growth**:
   When `write_page(page_id, ...)` is called on an unallocated `page_id \ge \text{num\_pages()}`, the file automatically grows in discrete 8KB increments padded with zeros (`[src/pager.cpp:52]`).
3. **C++ Stream Flag Integrity**:
   Because reading past EOF in `std::fstream` sets `eofbit` and `failbit`, subsequent seek operations silently fail unless `stream.clear()` is invoked prior to any seek or write (`[src/pager.cpp:25]`).

---

## 3. Sequence Diagram: Reading & Writing Pages

```mermaid
sequenceDiagram
    autonumber
    participant Client as Subsystem (Heap/BTree/CLOG)
    participant Pager as Pager (src/pager.cpp)
    participant OS as OS File System (*.db)

    Note over Client,OS: Reading an 8KB Page
    Client->>Pager: read_page(page_id, dest_buffer)
    Pager->>Pager: stream.clear()
    Pager->>Pager: offset = page_id * 8192
    Pager->>OS: seekg(offset, ios::beg)
    Pager->>OS: read(dest_buffer, 8192)
    Pager-->>Client: void (dest_buffer populated)

    Note over Client,OS: Writing an 8KB Page
    Client->>Pager: write_page(page_id, src_data)
    Pager->>Pager: stream.clear()
    Pager->>Pager: offset = page_id * 8192
    Pager->>OS: seekp(offset, ios::beg)
    Pager->>OS: write(src_data, 8192)
    Pager->>OS: flush() (push to OS disk buffer)
    Pager-->>Client: void (page persisted)
```
