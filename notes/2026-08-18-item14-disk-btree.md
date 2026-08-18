# Item 14: Disk-Resident B-Tree with Node Splits

**Date:** 2026-08-18  
**Topic:** PostgreSQL On-Disk B-Tree Architecture (`nbtree`), 12-Byte `BTreePageHeader`, Leaf & Internal Node Binary Layouts, Page Splits, Median Promotion, and Sibling Linked List Range Scans  
**Status:** Complete & Verified  

---

## 1. Directory Structure & Attached Code Files

- **Disk B-Tree Header**: [`include/pg/disk_btree.h`](../include/pg/disk_btree.h)
- **Disk B-Tree Implementation**: [`src/disk_btree.cpp`](../src/disk_btree.cpp)
- **Unit Test Suite**: [`tests/test_disk_btree.cpp`](../tests/test_disk_btree.cpp)

---

## 2. The Problem Solved

In Item 6, our `BTreeIndex` was backed by `std::multimap<int32_t, CTID>` in RAM. Every time the engine restarted, it had to perform a full sequential scan over the entire heap table to rebuild the index.

In real PostgreSQL (`src/backend/access/nbtree/`), B-Tree indexes reside on **dedicated 8KB disk pages**:
- **Internal Nodes**: Guide search descent using `(Key, Child Page ID)`.
- **Leaf Nodes**: Store sorted `(Key, CTID)` entries and are doubly/singly linked via `right_sibling` pointers for $O(K)$ range scans.
- **Node Splits**: When a leaf reaches capacity, it splits into left and right 8KB pages, pushing the median key up to its parent node.

```text
                                [ ROOT (Internal Page 2) ]
                                ┌─────────────────────────┐
                                │ [Key < 2000 -> Page 0]  │
                                │ [Key >= 2000 -> Page 1] │
                                └─────────────────────────┘
                                      /              \
                                     v                v
                 [ LEAF Page 0 ] ───right_sibling───► [ LEAF Page 1 ]
         ┌──────────────────────────────┐     ┌──────────────────────────────┐
         │ (10, (0,1)), (20, (0,2)),... │     │ (2000, (2,1)), (2010,...     │
         └──────────────────────────────┘     └──────────────────────────────┘
```

---

## 3. Binary Page Layouts

### A. 12-Byte `BTreePageHeader`
```cpp
#pragma pack(push, 1)
struct BTreePageHeader {
    uint8_t   level{0};                       // 0 = Leaf node, 1+ = Internal node
    uint8_t   flags{0};                       // BTREE_LEAF (0x01), BTREE_ROOT (0x02)
    uint16_t  num_keys{0};                    // Active key count
    page_id_t right_sibling{INVALID_PAGE_ID}; // Right sibling pointer for range scans
    page_id_t parent{INVALID_PAGE_ID};        // Parent node page ID
};
#pragma pack(pop)
```

### B. Node Entry Formats
- **Leaf Entry (10 Bytes)**: `[ key: int32_t (4B) ][ ctid: CTID (6B) ]`
- **Internal Entry (8 Bytes)**: `[ key: int32_t (4B) ][ child_page_id: page_id_t (4B) ]`

---

## 4. Node Splitting Algorithm

When a leaf node exceeds `BTREE_MAX_LEAF_KEYS = 64`:
1. Allocate a new 8KB page (`right_pid`).
2. Move upper half ($\lceil N/2 \rceil$) of entries to the right page.
3. Link `left_page.right_sibling = right_pid`.
4. Promote `right_page.entries[0].key` up to the parent internal node.
5. If the root node splits, allocate a new root page, set `level = old_level + 1`, and update the root pointer.

---

## 5. Verification Results (`tests/test_disk_btree.cpp`)

```text
--- REPRODUCING POSTGRESQL ON-DISK B-TREE INDEX WITH PAGE SPLITS ---
[Step 1] Inserting 10 keys into DiskBTree...
 -> Point lookups verified for single-page tree.

[Step 2] Inserting 500 keys to trigger node splits and tree growth...
 -> Total 8KB B-Tree pages allocated: 16
 -> Point queries across all split nodes verified successfully.

[Step 3] Testing Range Scan [2000 .. 2500] across multiple leaf pages...
 -> Range scan returned 51 entries.
 -> Range scan ordering and sibling traversal verified!

[Step 4] Reopening DiskBTree from disk and validating persistent tree...
 -> Key 3500 fetched directly from disk B-Tree index without scanning heap!

[Step 5] Testing multi-version duplicate keys for MVCC...
 -> Multi-version candidate accumulation verified (2 CTIDs returned for key 7777).

>>> ITEM 14 (DISK-RESIDENT B-TREE WITH PAGE SPLITS) TESTS PASSED SUCCESSFULLY! <<<
```

---

## 6. Self-Check & Calibration Questions

- **Definition (`DISK-BTREE-NODE-TYPES`)**: What is the structural difference between an internal B-Tree node and a leaf B-Tree node on disk?
- **Mechanism (`BTREE-PAGE-SPLIT-MEDIAN`)**: When a leaf node with 64 keys splits, describe the exact step-by-step redistribution of entries and how the parent node is updated.
- **System Design (`SIBLING-POINTER-RANGE-SCAN`)**: Why do PostgreSQL B-Tree leaf pages maintain `right_sibling` pointers, and how does this make range scans (e.g. `BETWEEN 100 AND 500`) faster than repeated tree descents?
