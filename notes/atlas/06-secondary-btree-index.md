# Item 6: Secondary B-Tree Index Subsystem

**Confidence**: `verified`  
**Citations**: [include/pg/btree.h:1-69](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/include/pg/btree.h), [src/btree.cpp:1-85](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/src/btree.cpp), [tests/test_index.cpp:1-125](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/tests/test_index.cpp)

---

## 1. Index Decoupling & Multi-Version Addressing

In PostgreSQL, secondary indexes store **only** $(\text{Key} \rightarrow \text{CTID})$ mappings.
The index does **not** store row columns, null bitmaps, or MVCC transaction headers (`xmin`/`xmax`).

```mermaid
flowchart LR
    subgraph SecondaryIndex["B-Tree Index Map"]
        K100["Key 100"] --> CTID_1["CTID (0, 1)"]
        K100 --> CTID_2["CTID (0, 2)"]
        K200["Key 200"] --> CTID_3["CTID (0, 3)"]
    end

    subgraph HeapTable["Physical Heap Pages"]
        H1["Tuple (0, 1)\n[xmin=1, xmax=2] (DEAD)"]
        H2["Tuple (0, 2)\n[xmin=2, xmax=0] (LIVE)"]
        H3["Tuple (0, 3)\n[xmin=3, xmax=0] (LIVE)"]
    end

    CTID_1 --> H1
    CTID_2 --> H2
    CTID_3 --> H3
```

---

## 2. Invariants & Lookup Pipeline

1. **Multi-Version Key Duplication**: Because updates create new row versions at new physical CTIDs without deleting the old version, a single index key can map to multiple candidate CTIDs simultaneously (`[src/btree.cpp:22]`).
2. **Mandatory Heap MVCC Dereference**: The index lookup returns candidate CTIDs. The database must dereference each CTID against the physical Heap and evaluate snapshot visibility rules before returning tuples to the query caller (`[src/engine.cpp:140]`).

---

## 3. Sequence Diagram: Index Point Scan

```mermaid
sequenceDiagram
    autonumber
    participant SQL as SQL Query Engine
    participant Idx as B-Tree Index (src/btree.cpp)
    participant Heap as Heap Storage (src/heap.cpp)
    participant MVCC as MVCC Evaluator (src/mvcc.cpp)

    SQL->>Idx: find_entries(item_id = 100)
    Idx-->>SQL: [ (0, 1), (0, 2) ] (Candidate CTIDs)

    loop For each candidate CTID
        SQL->>Heap: get(CTID)
        Heap-->>SQL: HeapTuple (header, data)
        SQL->>MVCC: is_visible(tuple.header, snapshot)
        alt Visible
            MVCC-->>SQL: true (Return tuple to client)
        else Invisible (Dead or Future version)
            MVCC-->>SQL: false (Skip candidate)
        end
    end
```
