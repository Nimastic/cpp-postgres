# Pre-Build Study: Volcano Relational Joins (Nested Loop & Hash Join)

**Date**: 2026-09-05  
**Topic**: Item 24 / Relational Query Engine  
**Scope**: `include/pg/executor.h`, `src/executor.cpp`, `src/engine.cpp`, `tests/test_joins.cpp`

---

## 1. Relational Joins in the Volcano Execution Engine

A relational database engine becomes truly relational when it can combine tuples across relations (or relation aliases) according to join predicates. In PostgreSQL's Volcano execution engine (`src/backend/executor/`):

1. **Nested Loop Join (`nodeNestloop.c`)**:
   - For each tuple in the outer relation, rescans the inner relation looking for matching tuples satisfying the join condition.
   - Computational complexity: $O(|R| \times |S|)$ comparisons.
   - Memory footprint: $O(1)$ RAM overhead (only current outer tuple and current inner tuple in flight).
   - Ideal for: Small outer relations, queries with selective index scans on the inner relation, or queries with non-equi predicates (`a.price > b.price`).

2. **Hash Join (`nodeHashjoin.c`)**:
   - **Phase 1 (Build Phase)**: The inner relation is completely scanned and inserted into an in-memory hash table keyed by the join attribute.
   - **Phase 2 (Probe Phase)**: The outer relation is streamed one tuple at a time. Each outer tuple probes the hash table in $O(1)$ time to find matching inner tuples.
   - Computational complexity: $O(|R| + |S|)$ time.
   - Memory footprint: $O(|S|)$ RAM overhead for the inner hash table.
   - Ideal for: Large relations with equi-join conditions (`a.price = b.price`).

```text
┌──────────────────────────────────────────────────────────────┐
│                        HashJoinNode                          │
│                                                              │
│  Phase 1 (init): Build hash table from inner child S         │
│  Phase 2 (next): Stream outer child R, probe hash table O(1) │
└───────────────────────────────▲──────────────────────────────┘
                                │
        ┌───────────────────────┴───────────────────────┐
        │                                               │ next(slot)
┌───────┴──────────────┐                        ┌───────┴──────────────┐
│  Outer Child (Seq)   │                        │  Inner Child (Seq)   │
│  Relation R (probe)  │                        │  Relation S (build)  │
└──────────────────────┘                        └──────────────────────┘
```

---

## 2. Invariants & Mathematical Formulations

### Invariant 1: Join Result Cardinality Bound
For relations $R$ and $S$ with $|R|$ and $|S|$ visible tuples respectively, any join operator satisfies:
$$0 \le |\text{Join}(R, S)| \le |R| \times |S|$$
For an equi-join on a unique primary key attribute where $S$ is the primary key relation:
$$|\text{Join}(R, S)| \le |R|$$

### Invariant 2: Pipelined Streaming Probe Memory Invariant
In `HashJoinNode`:
$$\text{Memory Complexity} = O(|S|) \quad (\text{hash table size})$$
$$\text{Probe Complexity per Output Tuple} = O(1)$$
Once the hash table is constructed during `init()`, `next(slot)` streams results without materializing the outer relation or the joined output relation.

---

## 3. 3-Depth Diagnostic Ladder

### Level 1 (Mechanics): How does `NestedLoopJoinNode` maintain cursor state?
- `need_new_outer_` flag controls outer loop progression.
- When `need_new_outer_` is true:
  1. Call `outer_->next(outer_slot_)`. If false, return false (EOF).
  2. Call `inner_->init()` to rewind the inner child iterator to the beginning of the inner relation.
  3. Set `need_new_outer_ = false`.
- Call `inner_->next(inner_slot_)`:
  - If true: evaluate `predicate_(outer_slot_, inner_slot_)`. If satisfied, emit `slot.set_join(outer, inner)` and return true.
  - If false: inner relation exhausted for this outer tuple. Set `need_new_outer_ = true` and loop back to fetch next outer tuple.

### Level 2 (Invariants): What happens if the inner relation cannot be rewound?
- In a nested loop join, the inner relation MUST support `init()` rewind. Because our `SeqScanNode::init()` resets `curr_page_id_ = 0`, `curr_slot_ = 1`, and releases pins, the inner scan rewinds safely without buffer pool pin leaks.

### Level 3 (Failure Modes): What happens if the hash table overflows RAM?
- In PostgreSQL, multi-batch hybrid hash joins spill overflowing hash buckets into temporary files on disk (`BufFile`). In our in-memory hash join, `std::unordered_multimap` holds the inner relation. For relations fitting in memory, hash table lookup provides instant $O(1)$ equi-matching.
