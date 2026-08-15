# Item 2: Slotted Page Architecture

**Date:** 2026-08-15  
**Topic:** PostgreSQL Slotted Page Layout, PageHeaderData, ItemIdData (Line Pointers), Free Space Management, and Indirection  
**Status:** Complete & Verified  

---

## 1. Multi-Layer Storage Engine Architecture

Here is the global map of how all data structures connect, from the raw disk file up to the B-Tree index:

```text
========================================================================================================
                                 POSTGRESQL STORAGE ENGINE ARCHITECTURE
========================================================================================================

+------------------------------------------------------------------------------------------------------+
| [LAYER 5: INDEX LAYER (Item 6)]                                                                      |
|                                                                                                      |
|   B-Tree Leaf Node                                                                                   |
|   +------------------------+------------------------+------------------------+                       |
|   | Key: item_id = 100     | Key: item_id = 200     | Key: item_id = 700     |                       |
|   | CTID: (Page 0, Slot 1) | CTID: (Page 0, Slot 2) | CTID: (Page 1, Slot 1) |                       |
|   +-----------|------------+------------------------+------------------------+                       |
+---------------|--------------------------------------------------------------------------------------+
                |
                |  CTID = (Page 0, Slot 1)
                v
+------------------------------------------------------------------------------------------------------+
| [LAYER 1: PAGER & DISK FILE (Item 1)]                                                                |
|                                                                                                      |
|   Database File on Disk ("items.db")                                                                 |
|   +------------------------------------+------------------------------------+                        |
|   | Page 0 (Offset 0B .. 8191B)        | Page 1 (Offset 8192B .. 16383B)    |  ...                   |
|   +-----------------|------------------+------------------------------------+                        |
+---------------------|--------------------------------------------------------------------------------+
                      |
                      |  Loads Page 0 into RAM (8192 Bytes)
                      v
+------------------------------------------------------------------------------------------------------+
| [LAYER 2: SLOTTED 8KB PAGE (Item 2)]                                                                 |
|                                                                                                      |
|   Offset 0B                                                                                          |
|   +----------------------------------------------------------------------------------------------+   |
|   | PageHeaderData (18 Bytes)                                                                    |   |
|   |   pd_lsn = 0 | pd_checksum = 0 | pd_lower = 26 | pd_upper = 8042 | pd_special = 8192         |   |
|   +----------------------------------------------------------------------------------------------+   |
|   | Line Pointer Array (ItemIdData, 4B each) -> [GROWS FORWARD]                                  |   |
|   |                                                                                              |   |
|   |   Slot 1 (CTID (0,1)): [ offset: 8142 | len: 50B | flags: NORMAL ] --------+                 |   |
|   |   Slot 2 (CTID (0,2)): [ offset: 8042 | len: 100B| flags: NORMAL ] -----+  |                 |   |
|   |   ...                                                                   |  |                 |   |
|   |   ^ (pd_lower = 26 points here)                                         |  |                 |   |
|   +---|---------------------------------------------------------------------|--|-----------------+   |
|   |   |                                                                     |  |                 |   |
|   |   |                           FREE SPACE GAP                            |  |                 |   |
|   |   |             (Available Space = pd_upper - pd_lower - 4)             |  |                 |   |
|   |   |                                                                     |  |                 |   |
|   +---|---------------------------------------------------------------------|--|-----------------+   |
|   |   v (pd_upper = 8042 points here)                                       |  |                 |   |
|   |                                                                         |  |                 |   |
|   | Tuple Storage Area <- [GROWS BACKWARD FROM 8192]                        |  |                 |   |
|   |                                                                         |  |                 |   |
|   |   +------------------------------------------------------------------+  |  |                 |   |
|   |   | Tuple Slot 2 [Offset 8042 .. 8141] (100 Bytes) <--------------------+  |                 |   |
|   |   +------------------------------------------------------------------+     |                 |   |
|   |   | Tuple Slot 1 [Offset 8142 .. 8191] (50 Bytes) <------------------------+                 |   |
|   |   +------------------------------------------------------------------+                       |   |
|   +----------------------------------------------------------------------------------------------+   |
|   Offset 8192B (PAGE_SIZE)                                                                           |
+------------------------------------------------------------------------------------------------------+
                      |
                      |  Zooming into the actual Tuple structure:
                      v
+------------------------------------------------------------------------------------------------------+
| [LAYER 3 & 4: HEAP TUPLE & MVCC (Items 3 & 4)]                                                       |
|                                                                                                      |
|   A Single Tuple Payload in Memory / Disk:                                                           |
|                                                                                                      |
|   +---------------------------------------------------+------------------------------------------+   |
|   | TupleHeader (System Headers for MVCC)             | User Data Columns (The "items" Schema)   |   |
|   |   xmin = 1  (Transaction that created this tuple) |   item_id = 100 (int32, 4 bytes)         |   |
|   |   xmax = 7  (Transaction that deleted/updated it) |   price   = 10  (int32, 4 bytes)         |   |
|   |   t_ctid = (0, 2) (Points to new version if HOT)  |                                          |   |
|   +---------------------------------------------------+------------------------------------------+   |
+------------------------------------------------------------------------------------------------------+
```

---

## 2. End-to-End Query Walkthrough

Trace of: `SELECT price FROM items WHERE item_id = 100;`

1. **Step 1 (Index Lookup - Item 6):** Look up key `item_id = 100` in the B-Tree index. The leaf node returns pointer **`CTID (0, 1)`**.
2. **Step 2 (Pager Disk Read - Item 1):** Database requests **`Page 0`** from `pg::Pager`. Pager seeks to byte offset $0 \times 8192 = 0$ on disk and copies the 8KB block into RAM.
3. **Step 3 (Line Pointer Resolution - Item 2):** Inside `Page 0`, inspect **`Slot 1`** in the line pointer array (located at byte offset $18$). The line pointer reads: `offset = 8142`, `len = 50`, `flags = NORMAL`.
4. **Step 4 (Heap Tuple MVCC Check - Items 3 & 4):** Jump to byte offset $8142$. Read `TupleHeader`:
   - Inspect `xmin` and `xmax` against current transaction snapshot.
   - If visible, deserialize `price` column and return `$10`.
   - If superseded, follow `t_ctid` chain to slot 2 (`price = $20`).

---

## 3. The 3 Core Mechanics of the Slotted Page

```text
Byte 0                                                              Byte 8192
+----------------+---------------------+-------------------+----------------+
| PageHeaderData | Line Pointers (->)  |    FREE SPACE     |  (<-) Tuples   |
+----------------+---------------------+-------------------+----------------+
0               18                     pd_lower            pd_upper      8192
```

### 1. Bi-directional Growth (`pd_lower` vs `pd_upper`)
When a page is freshly initialized:
- `pd_lower = 18` (after the 18-byte `PageHeaderData`).
- `pd_upper = 8192` (end of page).

When inserting a tuple of size $L = 60\text{ bytes}$:
1. Tuple payload is written to the top end:
   $$\text{new } \text{pd\_upper} = 8192 - 60 = 8132$$
   Tuple occupies bytes $[8132, 8191]$.
2. Line pointer is appended to the bottom end:
   $$\text{new } \text{pd\_lower} = 18 + 4 = 22$$
3. Line pointer records: `lp_offset = 8132`, `lp_len = 60`, `flags = NORMAL`.

### 2. The Free Space Equation (Why "+4" matters)
The free gap between pointers and tuples is:
$$\text{Free Gap} = \text{pd\_upper} - \text{pd\_lower}$$
To insert a tuple of length $L$, total space needed is:
$$\text{Space Required} = L + \text{sizeof}(\text{LinePointer}) = L + 4\text{ bytes}$$
If $\text{Free Gap} < L + 4$, the insert fails (`INVALID_SLOT_ID`).

### 3. Why Line Pointers Exist (CTID Stability & Index Decoupling)
Why not store physical byte offsets directly in indexes (e.g. `(Page 0, Byte 8132)`)?
- Over time, deletes and updates leave empty gaps on a page.
- When `VACUUM` defragments the page by moving tuples into contiguous space (e.g. from byte $8132$ to byte $8000$), **only the 4-byte Line Pointer on that page is updated**.
- All external B-Tree indexes pointing to `CTID (0, 1)` **remain 100% valid and never need to be rewritten**!

---

## 4. File-by-File Technical Breakdown & Attached Code Files

- **Page Interface Header**: [`include/pg/page.h`](../include/pg/page.h)
- **Page Implementation**: [`src/page.cpp`](../src/page.cpp)
- **Unit Test Suite**: [`tests/test_page.cpp`](../tests/test_page.cpp)

### A. [`include/pg/page.h`](../include/pg/page.h)

```cpp
#pragma once
#include "pg/constants.h"
#include <cstdint>
#include <vector>
#include <optional>
#include <string>

namespace pg {

enum class ItemFlags : uint8_t {
    UNUSED = 0,   // Slot is empty
    NORMAL = 1,   // Points to live tuple data
    REDIRECT = 2, // HOT redirect pointer to another slot
    DEAD = 3      // Marked dead by vacuum
};

#pragma pack(push, 1)
struct LinePointer {
    uint16_t lp_offset{0};    // Byte offset where tuple data begins
    uint16_t lp_len_flags{0}; // Lower 14 bits: length; Upper 2 bits: flags

    ItemFlags flags() const {
        return static_cast<ItemFlags>((lp_len_flags >> 14) & 0x03);
    }
    uint16_t length() const {
        return lp_len_flags & 0x3FFF;
    }
    void set(uint16_t offset, uint16_t len, ItemFlags f) {
        lp_offset = offset;
        lp_len_flags = (len & 0x3FFF) | (static_cast<uint16_t>(f) << 14);
    }
};

struct PageHeaderData {
    uint64_t pd_lsn{0};      // WAL Log Sequence Number
    uint16_t pd_checksum{0}; // Page checksum
    uint16_t pd_flags{0};    // Status flags
    uint16_t pd_lower{0};    // Offset to start of free space (end of line pointers)
    uint16_t pd_upper{0};    // Offset to end of free space (start of youngest tuple)
    uint16_t pd_special{static_cast<uint16_t>(PAGE_SIZE)}; // Special space offset
};
#pragma pack(pop)

static_assert(sizeof(LinePointer) == 4, "LinePointer must be 4 bytes");
static_assert(sizeof(PageHeaderData) == 18, "PageHeaderData must be 18 bytes");
```

---

### B. `src/page.cpp`

Key operational mechanics:
```cpp
slot_id_t Page::insert_tuple(const void* data, size_t len) {
    if (data == nullptr || len == 0 || len > PAGE_SIZE) return INVALID_SLOT_ID;

    auto& hdr = header();
    size_t space_needed = len + sizeof(LinePointer);
    if (hdr.pd_upper < hdr.pd_lower || (hdr.pd_upper - hdr.pd_lower) < space_needed) {
        return INVALID_SLOT_ID;
    }

    uint16_t new_upper = static_cast<uint16_t>(hdr.pd_upper - len);
    std::memcpy(data_ + new_upper, data, len);

    LinePointer lp;
    lp.set(new_upper, static_cast<uint16_t>(len), ItemFlags::NORMAL);

    size_t slot_index = (hdr.pd_lower - sizeof(PageHeaderData)) / sizeof(LinePointer);
    line_pointers_internal()[slot_index] = lp;

    hdr.pd_upper = new_upper;
    hdr.pd_lower += static_cast<uint16_t>(sizeof(LinePointer));
    return static_cast<slot_id_t>(slot_index + 1); // 1-based slot ID
}
```

---

## 5. Visual Page Dump from Unit Test

```text
====================== PAGE LAYOUT DUMP ======================
Header Size   : 18 bytes
pd_lower      : 26 (end of line pointers)
pd_upper      : 8042 (start of youngest tuple)
Free Space    : 8016 bytes
Slot Count    : 2 items
--------------------------------------------------------------
 [0..17] PageHeaderData (18B)
 [Line Pointers -> growing forward from offset 18 to 26]
   Slot  1: offset=8142, len=  50, flags=NORMAL
   Slot  2: offset=8042, len= 100, flags=NORMAL
 [26..8042] Free Space Gap (8016 bytes)
 [8042..8191] Tuple Storage Area (<- growing backward)
==============================================================
```

---

## 6. Quiz Diagnostics & Graded Mechanics

### Q1 · Slot Layout & Offsets (`SLOT-LAYOUT`)
- **Question**: When inserting a 60-byte tuple into a fresh page (`pd_lower=18, pd_upper=8192`), what are the new values?
- **Answer**: Option 1 (`pd_lower = 22, pd_upper = 8132, offset = 8132`) ✅
- **Mechanism**: Tuple grows backward ($8192 - 60 = 8132$), pointer grows forward ($18 + 4 = 22$).

### Q2 · Free Space Boundary (`FREE-SPACE-GAP`)
- **Question**: If `pd_lower = 100` and `pd_upper = 200`, can a 98-byte tuple fit?
- **Answer**: Option 2 (`No, 98 + 4 = 102B needed > 100B free`) ✅
- **Mechanism**: Every tuple insertion requires allocating a 4-byte line pointer at `pd_lower`.

### Q3 · Indirection & Stability (`CTID-INDIRECTION`)
- **Question**: Why store Line Pointers rather than direct byte offsets in CTIDs?
- **Answer**: Option 3 (`CTID stability across vacuum and defragmentation`) ✅
- **Mechanism**: Indexes point to `(page_id, slot_id)`. Internal page defragmentation changes only the 4-byte line pointer's offset, never breaking external index pointers.
