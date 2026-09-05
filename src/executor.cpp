#include "pg/executor.h"
#include <iostream>
#include <sstream>

namespace pg {

// =============================================================================
// SeqScanNode Implementation
// =============================================================================

SeqScanNode::SeqScanNode(HeapFile& heap, const Snapshot& snapshot, const TransactionManager& tm)
    : heap_(heap), snapshot_(snapshot), tm_(tm) {}

SeqScanNode::~SeqScanNode() {
    end();
}

void SeqScanNode::init() {
    curr_page_.release();
    curr_page_id_ = 0;
    curr_slot_id_ = 1;
    total_pages_ = heap_.num_pages();
    pages_scanned_ = 0;
    tuples_produced_ = 0;
    total_time_us_ = 0;
}

bool SeqScanNode::next(TupleTableSlot& slot) {
    auto start = std::chrono::steady_clock::now();
    slot.clear();

    while (true) {
        if (!curr_page_.valid()) {
            if (curr_page_id_ >= total_pages_) {
                auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start).count();
                total_time_us_ += elapsed;
                return false; // End of relation
            }
            // Pin exactly 1 frame in the shared buffer pool
            curr_page_ = heap_.pin(curr_page_id_);
            pages_scanned_++;
            curr_slot_id_ = 1;
        }

        size_t num_slots = curr_page_->num_slots();
        while (curr_slot_id_ <= num_slots) {
            slot_id_t s = curr_slot_id_++;
            auto lp = curr_page_->get_line_pointer(s);
            if (!lp.has_value() || lp->flags() != ItemFlags::NORMAL) {
                continue;
            }

            size_t len = 0;
            const uint8_t* ptr = curr_page_->get_tuple_ptr(s, &len);
            if (ptr == nullptr || len < sizeof(HeapTuple)) {
                continue;
            }

            HeapTuple tuple = HeapTuple::deserialize(ptr, len);
            if (is_tuple_visible(tuple.header, snapshot_, tm_)) {
                slot.set(CTID(curr_page_id_, s), tuple);
                tuples_produced_++;
                auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start).count();
                total_time_us_ += elapsed;
                return true;
            }
        }

        // Current page slots exhausted: release pin before fetching next page
        curr_page_.release();
        curr_page_id_++;
    }
}

void SeqScanNode::end() {
    curr_page_.release();
}

std::string SeqScanNode::explain(int indent) const {
    std::string pad(indent, ' ');
    std::ostringstream oss;
    oss << pad << "->  Seq Scan on items (scanned_pages=" << pages_scanned_
        << ", produced_tuples=" << tuples_produced_ << ")";
    return oss.str();
}

// =============================================================================
// IndexScanNode Implementation
// =============================================================================

IndexScanNode::IndexScanNode(Index& index, HeapFile& heap, index_key_t key,
                             const Snapshot& snapshot, const TransactionManager& tm)
    : index_(index), heap_(heap), key_(key), snapshot_(snapshot), tm_(tm) {}

IndexScanNode::~IndexScanNode() {
    end();
}

void IndexScanNode::init() {
    candidate_ctids_ = index_.find_entries(key_);
    visited_ctids_.clear();
    cursor_ = 0;
    tuples_produced_ = 0;
    total_time_us_ = 0;
}

bool IndexScanNode::next(TupleTableSlot& slot) {
    auto start = std::chrono::steady_clock::now();
    slot.clear();

    while (cursor_ < candidate_ctids_.size()) {
        CTID root_ctid = candidate_ctids_[cursor_++];
        auto hit = heap_.hot_search(root_ctid, snapshot_, tm_);
        if (hit.has_value()) {
            if (std::find(visited_ctids_.begin(), visited_ctids_.end(), hit->first) != visited_ctids_.end()) {
                continue; // Already produced this visible tuple version
            }
            visited_ctids_.push_back(hit->first);
            slot.set(hit->first, hit->second);
            tuples_produced_++;
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count();
            total_time_us_ += elapsed;
            return true;
        }
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
    total_time_us_ += elapsed;
    return false;
}

void IndexScanNode::end() {
    candidate_ctids_.clear();
    visited_ctids_.clear();
    cursor_ = 0;
}

std::string IndexScanNode::explain(int indent) const {
    std::string pad(indent, ' ');
    std::ostringstream oss;
    oss << pad << "->  Index Scan using items_pkey on items (Key: " << key_
        << ", produced_tuples=" << tuples_produced_ << ")";
    return oss.str();
}

// =============================================================================
// FilterNode Implementation
// =============================================================================

FilterNode::FilterNode(std::unique_ptr<PlanNode> child, Predicate predicate, std::string predicate_desc)
    : child_(std::move(child)), predicate_(std::move(predicate)), predicate_desc_(std::move(predicate_desc)) {}

void FilterNode::init() {
    if (child_) child_->init();
    tuples_produced_ = 0;
    total_time_us_ = 0;
}

bool FilterNode::next(TupleTableSlot& slot) {
    auto start = std::chrono::steady_clock::now();
    while (child_ && child_->next(slot)) {
        if (predicate_(slot)) {
            tuples_produced_++;
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count();
            total_time_us_ += elapsed;
            return true;
        }
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
    total_time_us_ += elapsed;
    return false;
}

void FilterNode::end() {
    if (child_) child_->end();
}

std::string FilterNode::explain(int indent) const {
    std::string pad(indent, ' ');
    std::ostringstream oss;
    oss << pad << "->  Filter: (" << predicate_desc_
        << ", passed_tuples=" << tuples_produced_ << ")\n";
    if (child_) {
        oss << child_->explain(indent + 4);
    }
    return oss.str();
}

// =============================================================================
// LimitNode Implementation
// =============================================================================

LimitNode::LimitNode(std::unique_ptr<PlanNode> child, size_t limit_count, size_t offset_count)
    : child_(std::move(child)), limit_count_(limit_count), offset_count_(offset_count) {}

void LimitNode::init() {
    if (child_) child_->init();
    returned_count_ = 0;
    tuples_produced_ = 0;
    total_time_us_ = 0;

    // Discard offset tuples
    TupleTableSlot discard;
    for (size_t i = 0; i < offset_count_ && child_; ++i) {
        if (!child_->next(discard)) break;
    }
}

bool LimitNode::next(TupleTableSlot& slot) {
    auto start = std::chrono::steady_clock::now();

    // Early termination: stop immediately once limit_count tuples have been returned
    if (returned_count_ >= limit_count_) {
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        total_time_us_ += elapsed;
        return false;
    }

    if (child_ && child_->next(slot)) {
        returned_count_++;
        tuples_produced_++;
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        total_time_us_ += elapsed;
        return true;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
    total_time_us_ += elapsed;
    return false;
}

void LimitNode::end() {
    if (child_) child_->end();
}

std::string LimitNode::explain(int indent) const {
    std::string pad(indent, ' ');
    std::ostringstream oss;
    oss << pad << "->  Limit: (count=" << limit_count_;
    if (offset_count_ > 0) {
        oss << ", offset=" << offset_count_;
    }
    oss << ", produced_tuples=" << tuples_produced_ << ")\n";
    if (child_) {
        oss << child_->explain(indent + 4);
    }
    return oss.str();
}

// =============================================================================
// ExecutionEngine Utility Implementation
// =============================================================================

std::vector<std::pair<CTID, HeapTuple>> ExecutionEngine::execute(PlanNode& plan) {
    std::vector<std::pair<CTID, HeapTuple>> results;
    plan.init();
    TupleTableSlot slot;
    while (plan.next(slot)) {
        results.emplace_back(slot.ctid, slot.tuple);
    }
    plan.end();
    return results;
}

size_t ExecutionEngine::count(PlanNode& plan) {
    size_t count = 0;
    plan.init();
    TupleTableSlot slot;
    while (plan.next(slot)) {
        count++;
    }
    plan.end();
    return count;
}

std::string ExecutionEngine::explain(PlanNode& plan, bool analyze) {
    std::ostringstream oss;
    if (analyze) {
        auto start = std::chrono::steady_clock::now();
        plan.init();
        TupleTableSlot slot;
        size_t rows = 0;
        while (plan.next(slot)) {
            rows++;
        }
        plan.end();
        auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();

        oss << "QUERY PLAN (ANALYZE: actual time=" << std::fixed << std::setprecision(3)
            << (total_us / 1000.0) << " ms, rows=" << rows << "):\n";
        oss << plan.explain(0) << "\n";
    } else {
        oss << "QUERY PLAN:\n";
        oss << plan.explain(0) << "\n";
    }
    return oss.str();
}

} // namespace pg
