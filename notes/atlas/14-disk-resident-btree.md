# Item 14: On-Disk B-Tree Index with Dynamic Node Splits

**Confidence**: `verified`  
**Citations**: [include/pg/disk_btree.h:1-95](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/include/pg/disk_btree.h), [src/disk_btree.cpp:1-240](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/src/disk_btree.cpp), [tests/test_disk_btree.cpp:1-140](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/tests/test_disk_btree.cpp)

---

## 1. Disk-Resident B-Tree Architecture

`DiskBTree` stores hierarchical B-Tree nodes on dedicated **8,192-byte disk pages**. Nodes split dynamically when overflowing, promoting median keys up to parent internal nodes.

```mermaid
flowchart TD
    subgraph RootNode["Root Internal Page (Page 0)"]
        R_HDR["Header: is_leaf=false, num_keys=1"]
        R_E1["Key under 500 -> Page 1 | Key 500 and above -> Page 2"]
    end

    subgraph LeafLevel["Leaf Level (Linked via right_sibling)"]
        L1["Leaf Page 1\nKeys: 100 to 400\nright_sibling -> Page 2"]
        L2["Leaf Page 2\nKeys: 500 to 900\nright_sibling -> 0 (Tail)"]
    end

    R_E1 --> L1
    R_E1 --> L2
    L1 -.->|Sibling Scan Pointer| L2
```

---

## 2. Invariants & Binary Page Formats

1. **B-Tree Page Header (12 Bytes)** (`[include/pg/disk_btree.h:20]`):
   - `is_leaf` (1 Byte): `true` for leaf pages, `false` for internal routing nodes.
   - `num_keys` (2 Bytes): Number of active key entries in this node.
   - `right_sibling` (4 Bytes): `page_id_t` linking to the right sibling page for fast linear range scans.
   - `parent_page_id` (4 Bytes): `page_id_t` of the parent router node.
2. **Entry Binary Layouts**:
   - **Leaf Entry (10 Bytes)**: `key` (4B int32) + `ctid` (4B CTID `{page, slot}`) + `flags` (2B).
   - **Internal Router Entry (8 Bytes)**: `key` (4B int32) + `child_page_id` (4B `page_id_t`).
3. **Median Split Algorithm**: When a leaf exceeds capacity ($N \ge 800$), the median entry at $N/2$ is promoted to the parent node, splitting the keys evenly into a newly allocated 8KB sibling page (`[src/disk_btree.cpp:125]`).

---

## 3. Sequence Diagram: Leaf Split & Key Insertion

```mermaid
sequenceDiagram
    autonumber
    participant Client as Engine DML
    participant Tree as DiskBTree (src/disk_btree.cpp)
    participant BPM as BufferPoolManager
    participant P0 as Leaf Page 0 (Full)
    participant P1 as New Leaf Page 1 (Allocated)

    Client->>Tree: insert_entry(key=550, ctid=(0, 20))
    Tree->>Tree: Traverse to target Leaf Page 0
    Tree->>P0: check num_keys >= MAX_KEYS (800)
    Note over Tree,P1: Node Overflow! Trigger Split Pass.
    Tree->>BPM: allocate_new_page() -> Page 1
    Tree->>P0: Copy upper 400 keys to Page 1
    Tree->>P0: Set right_sibling = Page 1
    Tree->>P1: Set right_sibling = old_sibling
    Tree->>Tree: Insert median key (500) into Root Parent Page
    Tree->>P1: Insert new key (550) into Page 1
    Tree-->>Client: void (Tree balanced!)
```
