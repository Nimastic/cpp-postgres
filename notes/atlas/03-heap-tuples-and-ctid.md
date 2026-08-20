# Item 3: Tuples & Heap CTID Physical Addressing

**Confidence**: `verified`  
**Citations**: [include/pg/tuple.h:1-90](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/include/pg/tuple.h), [include/pg/heap.h:1-82](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/include/pg/heap.h), [src/heap.cpp:1-210](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/src/heap.cpp), [tests/test_heap.cpp:1-120](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/tests/test_heap.cpp)

---

## 1. Physical CTID Tuple Addressing

Every row version in the database is uniquely addressed by its **CTID (Current Tuple ID)**:

$$\text{CTID} = (\text{page\_id}, \text{slot\_id})$$

```mermaid
flowchart TD
    subgraph HeapRelation["HeapFile (.db)"]
        subgraph Page0["Page 0"]
            LP1["Slot 1 Line Pointer"] --> T1["Tuple (0, 1)\n[xmin=1, xmax=0]"]
            LP2["Slot 2 Line Pointer"] --> T2["Tuple (0, 2)\n[xmin=2, xmax=0]"]
        end
        subgraph Page1["Page 1"]
            LP3["Slot 1 Line Pointer"] --> T3["Tuple (1, 1)\n[xmin=3, xmax=0]"]
        end
    end

    CTID_A["CTID (0, 1)"] --> LP1
    CTID_B["CTID (1, 1)"] --> LP3
```

---

## 2. Invariants & Binary Layout

1. **TupleHeader Structure (16 Bytes)** (`[include/pg/tuple.h:42-47]`):
   - `xmin` (4 Bytes): Transaction ID that inserted this row version.
   - `xmax` (4 Bytes): Transaction ID that deleted or updated this row version (`0` if currently live).
   - `t_ctid` (6 Bytes): Forward pointer `CTID {page, slot}` to the newest row version (or self-reference `(page, slot)` if newest).
   - `infomask` (2 Bytes): Status flags (`HEAP_HASEXTERNAL = 0x2000`, `HEAP_HOT_UPDATED = 0x4000`, `HEAP_ONLY_TUPLE = 0x8000`).

2. **Physical Tuple Capacity Derivation**:
   For an 8KB page with 18-byte page header and 24-byte serialized heap tuples (16B header + 8B data `item_id + price`), the maximum row capacity per page is:
   $$\text{Max Tuples Per Page} = \left\lfloor \frac{8192 - 18}{24 + 4} \right\rfloor = \left\lfloor \frac{8174}{28} \right\rfloor = 291 \text{ tuples}$$

---

## 3. Class Diagram: HeapFile & Pager Relationship

```mermaid
classDiagram
    class HeapFile {
        -unique_ptr~Pager~ pager_
        -BufferPoolManager* bpm_
        +insert(record, xmin) CTID
        +update(old_ctid, record, tx_id) CTID
        +delete_tuple(target_ctid, tx_id) bool
        +get(ctid) optional~HeapTuple~
        +seq_scan() vector~pair~CTID, HeapTuple~~
        +num_pages() size_t
    }
    class Pager {
        -fstream file_
        -size_t num_pages_
        +read_page(page_id, buffer)
        +write_page(page_id, buffer)
    }
    class Page {
        -uint8_t* data_
        +header() PageHeader&
        +insert_tuple(data, len) slot_id_t
        +get_tuple_ptr(slot_id, out_len) uint8_t*
    }

    HeapFile *-- Pager
    HeapFile ..> Page : instantiates
```
