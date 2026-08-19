# Item 17: TOAST Heap + Index Full Integration

**Confidence**: `verified`  
**Citations**: [include/pg/tuple.h:40-55](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/include/pg/tuple.h), [src/engine.cpp:180-245](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/src/engine.cpp), [tests/test_toast_integration.cpp:1-135](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/tests/test_toast_integration.cpp)

---

## 1. Transparent Inline vs Out-of-Line Routing

Item 17 integrates the TOAST subsystem transparently into the main SQL execution pipeline, B-Tree index scans, and cross-file durability across restarts.

```mermaid
flowchart TD
    subgraph QueryLayer["INSERT INTO items VALUES (id, price, 'doc...');"]
        CHECK{"Document Byte Size > 2048 B?"}
    end

    subgraph InlinePath["Inline Storage Path (<= 2KB)"]
        INLINE["Store payload directly inside main HeapTuple\ninfomask: Normal (0x0000)"]
    end

    subgraph OutOfLinePath["TOAST Out-of-Line Path (> 2KB)"]
        CHUNK["1. Slice into 2KB chunks in ToastRelation (*_toast.db)\n2. Store 18B ToastPointer in HeapTuple\n3. Stamp infomask: HEAP_HASEXTERNAL (0x2000)"]
    end

    subgraph BTreeIndex["Secondary Index (Key -> CTID)"]
        IDX["Index points directly to Heap CTID (0, 1)\nIndex is 100% unaffected by TOAST!"]
    end

    CHECK -->|No| INLINE
    CHECK -->|Yes| CHUNK
    INLINE --> BTreeIndex
    CHUNK --> BTreeIndex
```

---

## 2. Invariants & Infomask Flags

1. **`HEAP_HASEXTERNAL` (`0x2000`)**: Stamped on `TupleHeader::infomask`. Signals to sequential scans and index fetchers that the tuple's trailing bytes contain an 18-byte `ToastPointer` rather than inline text (`[include/pg/tuple.h:44]`).
2. **Zero Index Decoupling Impact**: Because the secondary index stores $(\text{item\_id} \rightarrow \text{CTID})$, the index node size is strictly 10 bytes regardless of whether the indexed row contains a 10-byte string or a 100-megabyte document.
3. **Cross-Relation Durability**: On engine restart, the main heap relation (`*_heap.db`) and auxiliary TOAST relation (`*_toast.db`) are reopened simultaneously, ensuring data consistency.

---

## 3. Sequence Diagram: Transparent TOAST Query Execution

```mermaid
sequenceDiagram
    autonumber
    participant Client as SQL Client
    participant Engine as pg::Engine
    participant Index as BTreeIndex
    participant Heap as HeapFile
    participant Toast as ToastManager

    Client->>Engine: SELECT doc FROM items WHERE item_id = 500;
    Engine->>Index: find_entries(500) -> CTID (0, 2)
    Engine->>Heap: get((0, 2)) -> HeapTuple
    Note over Engine: Check tuple.header.infomask and HEAP_HASEXTERNAL
    alt HEAP_HASEXTERNAL is TRUE
        Engine->>Toast: fetch_toast_payload(pointer.toast_id, pointer.raw_size)
        Toast-->>Engine: Reassembled 10000-byte document text
    else HEAP_HASEXTERNAL is FALSE
        Engine->>Engine: Read inline string
    end
    Engine-->>Client: Return document text to user
```
