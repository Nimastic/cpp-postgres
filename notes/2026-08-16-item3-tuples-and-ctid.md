# Item 3: Tuples, HeapFile, and CTID Addressing

**Date:** 2026-08-16  
**Topic:** PostgreSQL Heap Tuples, TupleHeader (xmin/xmax), Physical CTID `(page_id, slot_id)`, and Multi-Page `HeapFile` Management  
**Status:** Complete & Verified  

---

## 1. Directory Structure

```text
cpp-postgres/
├── CMakeLists.txt
├── PLAN.md
├── README.md
├── include/
│   └── pg/
│       ├── constants.h         # PAGE_SIZE, page_id_t, slot_id_t
│       ├── pager.h             # Pager binary stream interface
│       ├── page.h              # Slotted Page & Line Pointer
│       ├── tuple.h             # CTID, TupleHeader, ItemRecord, HeapTuple
│       └── heap.h              # HeapFile manager & Sequential Scan
├── src/
│   ├── pager.cpp
│   ├── page.cpp
│   └── heap.cpp
├── tests/
│   ├── test_pager.cpp          # Item 1 test
│   ├── test_page.cpp           # Item 2 test
│   └── test_heap.cpp           # Item 3 test
└── notes/
    ├── 2026-08-15-item1-pager.md
    ├── 2026-08-15-item2-slotted-page.md
    └── 2026-08-16-item3-tuples-and-ctid.md
```

---

## 2. Core Concepts & Data Structures

### A. The `CTID` (Current Tuple ID)
In PostgreSQL, every row version on disk has a physical address called **CTID**:
$$\text{CTID} = (\text{page\_id}, \text{slot\_id})$$
- `page_id`: Which 8KB block in the table file contains the tuple ($0, 1, 2, \dots$).
- `slot_id`: Which 1-based line pointer on that page points to the tuple ($1, 2, 3, \dots$).

```cpp
#pragma pack(push, 1)
struct CTID {
    page_id_t page{INVALID_PAGE_ID}; // 4 bytes
    slot_id_t slot{INVALID_SLOT_ID}; // 2 bytes
};
#pragma pack(pop)
// Total size = 6 bytes
```

---

### B. The `TupleHeader` (HeapTupleHeaderData)
Every physical tuple stored on a page has a hidden system header that PostgreSQL uses for MVCC and transaction isolation:

```text
+-------------------------------------------------------------------+
|                        TupleHeader (16 Bytes)                     |
+---------------------------------+---------------------------------+
| xmin (4 Bytes)                  | Transaction ID that created it  |
| xmax (4 Bytes)                  | Transaction ID that deleted/mod |
| t_ctid (6 Bytes: page, slot)    | Self pointer or forward pointer |
| infomask (2 Bytes)              | Status flags                    |
+---------------------------------+---------------------------------+
```

---

### C. The `HeapTuple` (Schema Record + Header)
In our engine, the user schema is the `items` table:
```sql
CREATE TABLE items (
    item_id INT,
    price   INT
);
```

The in-memory and on-disk representation is `HeapTuple` (24 bytes):
```text
+-------------------------------------------------------------------------------+
|                             HeapTuple (24 Bytes)                              |
+-----------------------------------------------+-------------------------------+
| TupleHeader (16 Bytes)                        | ItemRecord Data (8 Bytes)     |
|   xmin (4B) | xmax (4B) | t_ctid (6B) | flags |   item_id (4B) | price (4B)   |
+-----------------------------------------------+-------------------------------+
```

---

## 3. The `HeapFile` Manager (`pg::HeapFile`)

The `HeapFile` provides the table-level interface on top of `Pager` and `Page`:

```text
                                [ HeapFile ]
                                     |
               +---------------------+---------------------+
               |                                           |
               v                                           v
      Page 0 (8192 Bytes)                         Page 1 (8192 Bytes)
  +--------------------------+                +--------------------------+
  | PageHeaderData (18B)     |                | PageHeaderData (18B)     |
  | Line Pointers:           |                | Line Pointers:           |
  |   Slot 1 -> Offset 8168  |                |   Slot 1 -> Offset 8168  |
  |   Slot 2 -> Offset 8144  |                |   ...                    |
  |   ... (~291 slots max)   |                |                          |
  |                          |                |                          |
  | Tuples (<- backwards):   |                | Tuples (<- backwards):   |
  |   Slot 2 (24B) [8144]    |                |   ...                    |
  |   Slot 1 (24B) [8168]    |                |                          |
  +--------------------------+                +--------------------------+
```

### Key Operations:

1. **`insert(record, xmin)`**:
   - Tries to insert the 24-byte `HeapTuple` onto the most recent page.
   - If the page has enough free space ($\ge 24\text{B tuple} + 4\text{B line pointer} = 28\text{B}$):
     - Inserts the tuple, records `t_ctid = (page_id, slot_id)`, writes to disk, and returns the CTID.
   - If the page is full:
     - Automatically calls `pager.allocate_page()`.
     - Initializes fresh `Page`, inserts tuple at `slot 1`, writes to disk, and returns `(new_page_id, 1)`.

2. **`get(ctid)`**:
   - Seeks to `ctid.page`, reads line pointer at `ctid.slot`, verifies `flags == NORMAL`, and deserializes `HeapTuple`.

3. **`seq_scan()`**:
   - Loops from page $0$ to $\text{num\_pages} - 1$.
   - Scans all valid slots on each page and returns an ordered list of `std::pair<CTID, HeapTuple>`.

---

## 4. Verification Results

From `tests/test_heap.cpp`:
```text
[Test 1] Opening HeapFile and verifying Page 0 initialization...
 -> HeapFile initialized with 1 page.
[Test 2] Inserting (100, $10) and (200, $5) from video lecture...
 -> Inserted item 100 at CTID: (0, 1)
 -> Inserted item 200 at CTID: (0, 2)
[Test 3] Fetching tuples directly by CTID...
 -> Fetched and verified both tuples by CTID successfully.
[Test 4] Running Sequential Heap Scan...
 -> Sequential scan returned 2 tuples in physical order.
[Test 5] Inserting 500 rows to trigger multi-page allocation...
 -> Row 300 landed on: (1, 11)
 -> Total pages allocated: 2
 -> Sequential scan across multiple pages returned all 502 tuples!
[Test 6] Re-opening HeapFile to verify disk persistence...
 -> Disk persistence verified! All 502 tuples reloaded successfully.
```

---

## 6. Quiz Diagnostics & Graded Mechanics

### Q1 · CTID Physical Addressing (`CTID-PHYSICAL`)
- **Question**: Where does `CTID(0, 2)` point physically?
- **Answered**: Option 3 (Page $0$ on disk, resolved through Line Pointer Slot $2$ at byte offset $18 + 4 = 22$) ✅
- **Mechanism**:
  $$\text{Page Offset} = 0 \times 8192 = 0\text{B}$$
  $$\text{Line Pointer Slot 2 Offset} = \text{sizeof}(\text{PageHeaderData}) + 1 \times \text{sizeof}(\text{LinePointer}) = 18 + 4 = 22\text{B}$$
  The line pointer at byte $22$ contains `lp_offset`, pointing to the start of the tuple's 24-byte payload near the end of Page 0.

---

### Q2 · Single-Page Tuple Capacity Math (`PAGE-CAPACITY`)
- **Question**: How many `items` tuples fit in an 8KB page?
- **Answered**: Option 2 ($291$ tuples) ✅
- **Mechanism**:
  $$\text{Usable Page Space} = \text{PAGE\_SIZE} - \text{sizeof}(\text{PageHeaderData}) = 8192 - 18 = 8174\text{ bytes}$$
  $$\text{Per-Slot Footprint} = \text{sizeof}(\text{HeapTuple}) + \text{sizeof}(\text{LinePointer}) = 24 + 4 = 28\text{ bytes}$$
  $$\text{Max Slots} = \left\lfloor \frac{8174}{28} \right\rfloor = 291\text{ tuples}$$
  When tuple #292 is inserted, `HeapFile` detects insufficient free space and automatically calls `Pager::allocate_page()` for Page 1.

---

### Q3 · Row-Level MVCC Header Granularity (`MVCC-HEADER-PLACEMENT`)
- **Question**: Why store `xmin`/`xmax` in `TupleHeader` rather than `PageHeaderData`?
- **Answered**: Option 2 (Different row versions on the same page have independent transactional lifecycles) ✅
- **Mechanism**:
  A single 8KB page might hold 200 rows created across 50 different transactions over days or years. Furthermore, updating row #5 creates a new tuple version with a new `xmin` while row #1 remains untouched. MVCC visibility is inherently a **row-version property**, not a page-level property.

