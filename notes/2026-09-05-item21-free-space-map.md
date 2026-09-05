# Item 21: Free Space Map (FSM) with On-Disk Binary Max-Heap Tree

**Date:** 2026-09-05  
**Topic:** PostgreSQL Free Space Map Architecture (`storage/freespace/`), 8KB `FsmPage` Binary Max-Heap Tree Layout, Categorical Discretization, $O(\log N)$ Page Allocation, and VACUUM Reclamation  
**Status:** Pre-Build Study & Specification  

---

## 1. Directory Structure & Attached Code Files

- **Free Space Map Header**: [`include/pg/fsm.h`](../include/pg/fsm.h)
- **Free Space Map Implementation**: [`src/fsm.cpp`](../src/fsm.cpp)
- **Heap Integration**: [`src/heap.cpp`](../src/heap.cpp), [`include/pg/heap.h`](../include/pg/heap.h)
- **VACUUM Integration**: [`src/vacuum.cpp`](../src/vacuum.cpp)
- **Unit Test Suite**: [`tests/test_fsm.cpp`](../tests/test_fsm.cpp)

---

## 2. The Problem Solved

In naive storage engines, inserting a tuple either:
1. **Always appends to the last page**, leaving holes reclaimed by VACUUM in prior pages permanently wasted; or
2. **Scans backwards linearly from the last page**, causing $O(N)$ page reads and buffer pool thrashing in large relations.

In real PostgreSQL (`src/backend/storage/freespace/`), every table maintains an auxiliary **Free Space Map (FSM)** companion fork (`<relfilenode>_fsm`). 

The FSM tracks the free space of every heap page using a single byte (category $0 \dots 255$). Within an 8KB page, these categories are arranged into a **complete binary max-heap tree**:

```text
                           [ Root Node: Max Category across 4,096 pages ]
                                            /               \
                          [ Left Half: Pages 0-2047 ]   [ Right Half: Pages 2048-4095 ]
                                      /                                 \
                                     ...                               ...
                 [ Leaf 0: Page 0 ] [ Leaf 1: Page 1 ] ... [ Leaf 4095: Page 4095 ]
```

When an insertion arrives:
- Instead of scanning pages, the engine traverses the binary tree from the root down to a leaf in $O(\log \text{leaf\_count})$ time ($< 12$ binary comparisons).
- If the root category is smaller than needed, the relation knows instantly that no page has space, without inspecting a single heap page.

---

## 3. Mathematical Foundations & Binary Layout

### A. Mathematical Derivation of FSM Tree Capacity

In an 8,192-byte page (`PAGE_SIZE`):
- Let depth $D = 12$.
- Number of leaf nodes: $L = 2^D = 2^{12} = 4,096$ leaves.
- Number of internal nodes: $I = 2^D - 1 = 4,095$ internal nodes.
- Total tree nodes:
  $$N_{\text{nodes}} = L + I = 4,096 + 4,095 = 8,191 \text{ bytes}$$
- Remaining space:
  $$8,192 - 8,191 = 1 \text{ byte (reserved)}$$

Every 8KB FSM page tracks **exactly 4,096 heap pages** (32 MB of table data) with zero wasted space!

### B. Categorical Free Space Quantization

An 8KB page has between $0$ and $8,192$ bytes of free space. Quantizing into 1 byte (`uint8_t`):
$$\text{category} = \min\left(255, \left\lfloor \frac{\text{free\_bytes}}{32} \right\rfloor\right)$$
- Category 0: 0 – 31 bytes available.
- Category 1: 32 – 63 bytes available.
- Category 255: 8,160 – 8,192 bytes available.

Given a required allocation size $S$ (e.g. `sizeof(HeapTuple) + sizeof(LinePointer)`):
$$\text{target\_cat} = \min\left(255, \left\lceil \frac{S}{32} \right\rceil\right)$$

### C. Complete Binary Tree Struct Layout

```cpp
#pragma pack(push, 1)
struct FsmPage {
    uint8_t nodes[8191]; // Binary max-heap: nodes[0] is root, nodes[4095..8190] are leaves
    uint8_t reserved{0}; // Pad to exactly 8192 bytes
};
#pragma pack(pop)
static_assert(sizeof(FsmPage) == 8192, "FsmPage must be exactly 8192 bytes");
```

---

## 4. Algorithms

### A. Tree Search ($O(\log M)$ Descent)
```text
1. If nodes[0] < target_cat: return INVALID_PAGE_ID (no page has room).
2. Start at index i = 0.
3. While i < 4095 (not a leaf):
     left  = 2 * i + 1
     right = 2 * i + 2
     If nodes[left] >= target_cat:
         i = left
     Else:
         i = right
4. slot = i - 4095
5. return (fsm_page_id * 4096) + slot
```

### B. Tree Update ($O(\log M)$ Bubble-Up)
```text
1. slot = page_id % 4096
2. leaf = 4095 + slot
3. nodes[leaf] = new_category
4. i = leaf
5. While i > 0:
     parent  = (i - 1) / 2
     sibling = (i % 2 == 1) ? i + 1 : i - 1
     new_max = max(nodes[i], nodes[sibling])
     If nodes[parent] == new_max: break
     nodes[parent] = new_max
     i = parent
```

---

## 5. Attacking Common Industry Misconceptions

- **Misconception 1: "Relational engines find space by maintaining a linked list of free blocks."**
  - *Reality*: Linked lists cause terrible random I/O and pointer chasing. PostgreSQL uses an array-based binary max-heap stored inside a single page, providing $O(1)$ verification of space and $O(\log N)$ descent without any pointer chasing or memory allocation.
- **Misconception 2: "FSM must be WAL-logged on every change."**
  - *Reality*: PostgreSQL does **not** WAL-log FSM changes! The FSM is an unlogged cache of heap page space. If a crash occurs, FSM pages can be dirty or stale; if an FSM lookup directs an insert to a page that turns out to be full, the engine simply updates the FSM and picks another. VACUUM repopulates the FSM accurately during maintenance.

---

## 6. The 3-Depth Diagnostic Ladder

1. **Level 1 (Mechanics)**: A single byte change at `nodes[4095 + slot]` triggers at most 12 parent comparisons up to `nodes[0]`.
2. **Level 2 (Invariants)**: The root node `nodes[0]` is guaranteed to be $\max_{k=0}^{4095} (\text{category}(k))$. If `nodes[0] < target`, scanning leaves is provably unnecessary.
3. **Level 3 (Failure Modes)**: If power cuts off before FSM writes reach disk, heap data remains completely crash-safe because the heap relation has write-ahead logging and ARIES recovery. The FSM is updated immediately upon the next write or VACUUM run.
