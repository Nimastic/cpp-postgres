# Pre-Build Study: Volcano Iterator Query Execution Engine (Finding 2.5)

**Date**: 2026-09-05  
**Topic**: Item 23 / Architecture Audit Finding 2.5  
**Scope**: `include/pg/executor.h`, `src/executor.cpp`, `src/engine.cpp`, `tests/test_executor.cpp`

---

## 1. The Core Architectural Defect

In the legacy implementation, query processing in `HeapFile::seq_scan()` (`src/heap.cpp:212, 236`) followed a batch materialization pattern:

```cpp
// LEGACY FLAW: Materializes the entire relation into heap memory!
std::vector<std::pair<CTID, HeapTuple>> HeapFile::seq_scan(const Snapshot& snapshot, const TransactionManager& tm) {
    std::vector<std::pair<CTID, HeapTuple>> visible_tuples;
    for (page_id_t pid = 0; pid < total_pages; ++pid) {
        // ... scans and pushes every visible tuple into vector ...
    }
    return visible_tuples;
}
```

### Consequences:
1. **$O(N)$ RAM Footprint**: A query scanning a 10 GB table requires allocating 10 GB of userspace memory in RAM, rapidly causing out-of-memory errors.
2. **Exhaustive I/O for Pipelined Queries**: A query like `SELECT * FROM items LIMIT 1` forced the engine to scan every single page and deserialize every tuple in the relation before returning the first result.
3. **Absence of Plan Trees**: The engine lacked an abstraction for composable query operators (`SeqScan`, `IndexScan`, `Filter`, `Limit`, `Sort`, `Join`).

---

## 2. PostgreSQL's Solution: The Volcano Demand-Driven Iterator Model

PostgreSQL's executor (`src/backend/executor/`) implements the classical **Volcano Iterator Model** (Graefe, 1994), characterized by:

1. **Standardized Currency (`TupleTableSlot`)**:
   Plan nodes do not pass raw pointers or vectors. They exchange a standardized `TupleTableSlot` (`include/executor/tuptable.h`). A slot holds:
   - Physical CTID (`page_id`, `slot_id`)
   - `HeapTuple` data & headers (`xmin`, `xmax`, `infomask`)
   - State flag (`is_empty`)
2. **Three-Method Lifecycle**:
   Every plan node in the plan tree (`PlanNode` / `PlanState`) implements:
   - `ExecInitNode` / `init()`: Allocates cursor state, opens relations, resets counters.
   - `ExecProcNode` / `next(slot)`: Demand-driven pull. Returns `true` with the slot populated, or `false` on EOF.
   - `ExecEndNode` / `end()`: Closes relations, releases buffer pins, tears down state.
3. **Pipelining & Streaming**:
   Parents pull from children one tuple at a time on demand.

```text
┌────────────────────────────────────────────────────────┐
│                      LimitNode                         │
│   (pulls up to N tuples from child, then signals EOF)  │
└───────────────────────────▲────────────────────────────┘
                            │ next(slot)
┌───────────────────────────┴────────────────────────────┐
│                     FilterNode                         │
│  (pulls until child tuple satisfies predicate func)   │
└───────────────────────────▲────────────────────────────┘
                            │ next(slot)
┌───────────────────────────┴────────────────────────────┐
│                    SeqScanNode                         │
│ (pins 1 page buffer at a time, checks MVCC visibility) │
└────────────────────────────────────────────────────────┘
```

---

## 3. Mathematical Invariants & Physical Guarantees

### Invariant 1: Single-Buffer Pinning During Table Scan ($O(1)$ Buffer Pin Invariant)
At any point during a sequential scan across an arbitrary number of pages $P \in [1, \infty)$:
$$\text{pinned\_frames} \le 1$$
When `SeqScanNode` finishes scanning the slots of Page $k$, it releases the pin on Frame $k$ before pinning Frame $k + 1$. This guarantees that sequential scans never starve the buffer pool of unpinned frames.

### Invariant 2: Optimal Early-Termination I/O Complexity
For a query with `LIMIT K` on a relation containing $P$ pages and $N$ tuples, where $T_{\text{page}} = \frac{N}{P}$ is the average tuple density per page:
$$\text{Pages Touched} = \min\left(P, \left\lceil \frac{K}{T_{\text{page}}} \right\rceil\right)$$
For `LIMIT 1`:
$$\text{Pages Touched} = 1 \ll P$$
Memory allocated:
$$\text{RAM Consumption} = O(1) \quad (\text{exactly 1 TupleTableSlot in flight})$$

---

## 4. 3-Depth Diagnostic Ladder

### Level 1 (Mechanics): How does execution state progress?
- `SeqScanNode` maintains `curr_page_id_` and `curr_slot_id_`.
- `curr_page_` holds an RAII `PinnedPage`.
- In `next(slot)`:
  1. If `curr_page_` is invalid and `curr_page_id_ < total_pages_`, pin page `curr_page_id_`.
  2. Examine slot `curr_slot_id_`:
     - If line pointer is `NORMAL` and satisfies `HeapTupleSatisfiesMVCC`, populate `slot` and advance `curr_slot_id_`. Return `true`.
     - Otherwise advance `curr_slot_id_`.
  3. If `curr_slot_id_ > page->num_slots()`, release `curr_page_`, advance `curr_page_id_++`, and loop to next page.
  4. If `curr_page_id_ >= total_pages_`, return `false` (EOF).

### Level 2 (Invariants): What happens if an invariant is violated?
- If `curr_page_` is not released when advancing to the next page, pin counts accumulate. In a buffer pool of size 32, the scan will throw an "all frames are pinned" exception after reading 32 pages.
- If MVCC evaluation is bypassed or evaluated on unpinned memory, concurrent writers could corrupt or tear the tuple data mid-read.

### Level 3 (Failure Modes): How does early termination handle resource cleanup?
- When `LimitNode` reaches its threshold, it returns `false` without exhausting `child->next()`.
- When the execution tree is destructed (`end()`), `SeqScanNode`'s destructor drops `curr_page_`, invoking `bpm->unpin_page()`. No pins or memory leak.
