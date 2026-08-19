# Item 5: VACUUM Engine & In-Place Page Defragmentation

**Confidence**: `verified`  
**Citations**: [include/pg/vacuum.h:1-35](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/include/pg/vacuum.h), [src/vacuum.cpp:1-120](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/src/vacuum.cpp), [tests/test_vacuum.cpp:1-115](file:///c:/Users/jerie/Documents/GitHub/cpp-postgres/tests/test_vacuum.cpp)

---

## 1. Dead Tuple Reclamation Mechanics

When rows are updated or deleted under MVCC, old tuple versions become **dead** once their `xmax` falls below the oldest active transaction in the entire cluster:

$$\text{Dead Condition: } \text{xmax} > 0 \ \land \ \text{status}(\text{xmax}) == \text{COMMITTED} \ \land \ \text{xmax} < \text{oldest\_active\_xmin}$$

```mermaid
flowchart TD
    subgraph SlottedPageDefrag["In-Place Page Compaction"]
        BEFORE["Before VACUUM:\n[Slot 1: LIVE @ 8168]\n[Slot 2: DEAD @ 8144]\n[Slot 3: LIVE @ 8120]\npd_upper = 8120 (Fragmented)"]
        PROCESS["VACUUM Pass:\n1. Mark Slot 2 UNUSED (offset=0, len=0)\n2. Shift Slot 3 data up to 8144\n3. Update Slot 3 LinePointer offset\n4. Set pd_upper = 8144 (Reclaimed 24B)"]
        AFTER["After VACUUM:\n[Slot 1: LIVE @ 8168]\n[Slot 2: UNUSED]\n[Slot 3: LIVE @ 8144]\npd_upper = 8144 (Contiguous Free Space)"]
    end

    BEFORE --> PROCESS --> AFTER
```

---

## 2. Invariants & CTID Stability

1. **CTID Slot Stability**: VACUUM never shifts or renumbers existing `slot_id` line pointer indexes. Slot 3 remains `(page, 3)`, guaranteeing that foreign references and secondary index entries are not corrupted (`[src/page.cpp:125]`).
2. **Compact Contiguous Allocation**: Surviving tuple payloads are slid upward towards the end of the 8KB page (`PAGE_SIZE = 8192`), coalescing all freed memory into a single contiguous block between `pd_lower` and `pd_upper`.

---

## 3. Sequence Diagram: Vacuum Page Lifecycle

```mermaid
sequenceDiagram
    autonumber
    participant Admin as Maintenance Coordinator
    participant Vac as VacuumEngine (src/vacuum.cpp)
    participant TM as TransactionManager
    participant Heap as Heap Storage
    participant Page as Slotted Page (RAM)

    Admin->>Vac: vacuum(heap, tm)
    Vac->>TM: oldest_active_xmin() -> Cutoff XID (e.g. 50)
    loop For each page_id in 0 .. num_pages-1
        Vac->>Heap: read_page(page_id, page_buf)
        loop For each slot_id in 1 .. num_slots
            Vac->>Page: check tuple header (xmin, xmax)
            alt xmax != 0 and xmax < cutoff and committed
                Vac->>Page: mark line_pointer[slot_id] = UNUSED
            end
        end
        Vac->>Page: compact_and_defragment()
        Vac->>Heap: write_page(page_id, page_buf)
    end
    Vac-->>Admin: VacuumResult {reclaimed_tuples, reclaimed_bytes, pages_vacuumed}
```
