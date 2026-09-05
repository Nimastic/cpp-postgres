#pragma once

#include "pg/tuple.h"
#include "pg/heap.h"
#include "pg/index.h"
#include "pg/tx.h"
#include "pg/mvcc.h"
#include <memory>
#include <functional>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <sstream>
#include <iomanip>

namespace pg {

// Standardized Tuple Slot (matches PostgreSQL's TupleTableSlot in include/executor/tuptable.h)
// Holds in-flight tuples, decoupling plan nodes from storage and memory layouts.
// Supports both single-relation scans and composite joined relations (outer + inner).
struct TupleTableSlot {
    bool is_empty{true};
    CTID ctid{};
    HeapTuple tuple{};

    // Relational join fields
    bool has_inner{false};
    CTID inner_ctid{};
    HeapTuple inner_tuple{};

    void set(const CTID& c, const HeapTuple& t) {
        ctid = c;
        tuple = t;
        has_inner = false;
        inner_ctid = CTID{};
        inner_tuple = HeapTuple{};
        is_empty = false;
    }

    void set_join(const CTID& c_out, const HeapTuple& t_out,
                  const CTID& c_in, const HeapTuple& t_in) {
        ctid = c_out;
        tuple = t_out;
        has_inner = true;
        inner_ctid = c_in;
        inner_tuple = t_in;
        is_empty = false;
    }

    void clear() {
        is_empty = true;
        has_inner = false;
        ctid = CTID{};
        tuple = HeapTuple{};
        inner_ctid = CTID{};
        inner_tuple = HeapTuple{};
    }

    bool empty() const { return is_empty; }
    explicit operator bool() const { return !is_empty; }
};

// Abstract Plan Node (matches PostgreSQL's PlanState & ExecProcNode)
// Implements the classical Volcano Demand-Driven Iterator Model:
// 1. init(): Initializes execution state and opens cursors
// 2. next(slot): Pulls the next qualifying tuple on demand into slot; returns false on EOF
// 3. end(): Cleans up resources, releases buffer pins, closes cursors
class PlanNode {
public:
    virtual ~PlanNode() = default;

    // Volcano lifecycle
    virtual void init() = 0;
    virtual bool next(TupleTableSlot& slot) = 0;
    virtual void end() = 0;

    // Diagnostic plan tree inspection
    virtual std::string explain(int indent = 0) const = 0;

    // Execution metrics
    size_t tuples_produced() const { return tuples_produced_; }
    uint64_t total_time_us() const { return total_time_us_; }

protected:
    size_t tuples_produced_{0};
    uint64_t total_time_us_{0};
};

// Streaming Sequential Scan Node
// Invariant: At most ONE page frame is pinned in shared buffers at any time (O(1) buffer pin guarantee)
class SeqScanNode : public PlanNode {
public:
    SeqScanNode(HeapFile& heap, const Snapshot& snapshot, const TransactionManager& tm);
    ~SeqScanNode() override;

    void init() override;
    bool next(TupleTableSlot& slot) override;
    void end() override;

    std::string explain(int indent = 0) const override;

    size_t pages_scanned() const { return pages_scanned_; }

private:
    HeapFile& heap_;
    const Snapshot& snapshot_;
    const TransactionManager& tm_;

    page_id_t curr_page_id_{0};
    slot_id_t curr_slot_id_{1};
    PinnedPage curr_page_;
    size_t total_pages_{0};
    size_t pages_scanned_{0};
};

// B-Tree Index Point/Range Scan Node
class IndexScanNode : public PlanNode {
public:
    IndexScanNode(Index& index, HeapFile& heap, index_key_t key,
                  const Snapshot& snapshot, const TransactionManager& tm);
    ~IndexScanNode() override;

    void init() override;
    bool next(TupleTableSlot& slot) override;
    void end() override;

    std::string explain(int indent = 0) const override;

private:
    Index& index_;
    HeapFile& heap_;
    index_key_t key_;
    const Snapshot& snapshot_;
    const TransactionManager& tm_;

    std::vector<CTID> candidate_ctids_;
    std::vector<CTID> visited_ctids_;
    size_t cursor_{0};
};

// Selection Filter Node (evaluates row predicates on the fly without materialization)
class FilterNode : public PlanNode {
public:
    using Predicate = std::function<bool(const TupleTableSlot&)>;

    FilterNode(std::unique_ptr<PlanNode> child, Predicate predicate, std::string predicate_desc);
    ~FilterNode() override = default;

    void init() override;
    bool next(TupleTableSlot& slot) override;
    void end() override;

    std::string explain(int indent = 0) const override;

    const PlanNode* child() const { return child_.get(); }

private:
    std::unique_ptr<PlanNode> child_;
    Predicate predicate_;
    std::string predicate_desc_;
};

// Cardinality Limiter & Pipelined Early Termination Node
// Invariant: Immediately halts child plan node once limit_count is reached, avoiding unneeded page I/O
class LimitNode : public PlanNode {
public:
    LimitNode(std::unique_ptr<PlanNode> child, size_t limit_count, size_t offset_count = 0);
    ~LimitNode() override = default;

    void init() override;
    bool next(TupleTableSlot& slot) override;
    void end() override;

    std::string explain(int indent = 0) const override;

    const PlanNode* child() const { return child_.get(); }
    size_t limit_count() const { return limit_count_; }
    size_t offset_count() const { return offset_count_; }

private:
    std::unique_ptr<PlanNode> child_;
    size_t limit_count_;
    size_t offset_count_;
    size_t returned_count_{0};
};

// Demand-Driven Nested-Loop Relational Join Node
// Outer relation is streamed tuple-by-tuple. For each outer tuple, the inner relation is
// rewound via init() and scanned to completion. Zero extra memory footprint (O(1) memory).
class NestedLoopJoinNode : public PlanNode {
public:
    using JoinPredicate = std::function<bool(const TupleTableSlot& outer, const TupleTableSlot& inner)>;

    NestedLoopJoinNode(std::unique_ptr<PlanNode> outer,
                       std::unique_ptr<PlanNode> inner,
                       JoinPredicate predicate = nullptr,
                       std::string predicate_desc = "");
    ~NestedLoopJoinNode() override = default;

    void init() override;
    bool next(TupleTableSlot& slot) override;
    void end() override;

    std::string explain(int indent = 0) const override;

    const PlanNode* outer() const { return outer_.get(); }
    const PlanNode* inner() const { return inner_.get(); }

private:
    std::unique_ptr<PlanNode> outer_;
    std::unique_ptr<PlanNode> inner_;
    JoinPredicate predicate_;
    std::string predicate_desc_;

    TupleTableSlot current_outer_{};
    bool need_new_outer_{true};
    bool outer_exhausted_{false};
};

// Two-Phase Classical Hash Join Node
// Phase 1 (init): Materializes inner relation into an in-memory hash table (std::unordered_multimap).
// Phase 2 (next): Streams outer relation tuple-by-tuple and probes the hash table in O(1) time per tuple.
class HashJoinNode : public PlanNode {
public:
    using KeyExtractor = std::function<int32_t(const TupleTableSlot&)>;

    HashJoinNode(std::unique_ptr<PlanNode> outer,
                 std::unique_ptr<PlanNode> inner,
                 KeyExtractor outer_key_fn,
                 KeyExtractor inner_key_fn,
                 std::string join_desc = "");
    ~HashJoinNode() override = default;

    void init() override;
    bool next(TupleTableSlot& slot) override;
    void end() override;

    std::string explain(int indent = 0) const override;

    const PlanNode* outer() const { return outer_.get(); }
    const PlanNode* inner() const { return inner_.get(); }
    size_t hash_entries() const { return hash_entries_; }

private:
    std::unique_ptr<PlanNode> outer_;
    std::unique_ptr<PlanNode> inner_;
    KeyExtractor outer_key_fn_;
    KeyExtractor inner_key_fn_;
    std::string join_desc_;

    using HashTable = std::unordered_multimap<int32_t, std::pair<CTID, HeapTuple>>;
    HashTable hash_table_;
    size_t hash_entries_{0};
    TupleTableSlot current_outer_{};
    HashTable::const_iterator probe_current_{};
    HashTable::const_iterator probe_end_{};
    bool outer_exhausted_{false};
};

// Volcano Query Execution Engine Utilities
class ExecutionEngine {
public:
    // Execute a plan tree and collect visible tuples
    static std::vector<std::pair<CTID, HeapTuple>> execute(PlanNode& plan);

    // Execute a plan tree and collect visible tuple slots (preserves composite join slots)
    static std::vector<TupleTableSlot> execute_slots(PlanNode& plan);

    // Count qualifying tuples without materializing into an in-memory vector
    static size_t count(PlanNode& plan);

    // Format EXPLAIN or EXPLAIN ANALYZE tree
    static std::string explain(PlanNode& plan, bool analyze = false);
};

} // namespace pg

