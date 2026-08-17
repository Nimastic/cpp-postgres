#include "pg/btree.h"
#include <sstream>
#include <set>

namespace pg {

void BTreeIndex::insert_entry(index_key_t key, const CTID& ctid) {
    tree_.insert({key, ctid});
}

std::vector<CTID> BTreeIndex::find_entries(index_key_t key) const {
    std::vector<CTID> results;
    auto range = tree_.equal_range(key);
    for (auto it = range.first; it != range.second; ++it) {
        results.push_back(it->second);
    }
    return results;
}

bool BTreeIndex::remove_entry(index_key_t key, const CTID& ctid) {
    auto range = tree_.equal_range(key);
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second == ctid) {
            tree_.erase(it);
            return true;
        }
    }
    return false;
}

size_t BTreeIndex::num_entries() const {
    return tree_.size();
}

size_t BTreeIndex::num_unique_keys() const {
    std::set<index_key_t> keys;
    for (const auto& [k, v] : tree_) {
        keys.insert(k);
    }
    return keys.size();
}

std::vector<std::pair<index_key_t, CTID>> BTreeIndex::range_scan(index_key_t min_key, index_key_t max_key) const {
    std::vector<std::pair<index_key_t, CTID>> results;
    auto it_low = tree_.lower_bound(min_key);
    auto it_high = tree_.upper_bound(max_key);

    for (auto it = it_low; it != it_high; ++it) {
        results.push_back(*it);
    }
    return results;
}

void BTreeIndex::clear() {
    tree_.clear();
}

std::string BTreeIndex::dump() const {
    std::ostringstream oss;
    oss << "================== B-TREE INDEX DUMP ==================\n";
    oss << "Total Entries: " << tree_.size() << "\n";
    for (const auto& [k, v] : tree_) {
        oss << "  Key [" << k << "] -> CTID: " << v.to_string() << "\n";
    }
    oss << "=======================================================\n";
    return oss.str();
}

std::optional<std::pair<CTID, HeapTuple>> index_lookup(
    const BTreeIndex& index,
    HeapFile& heap,
    index_key_t key,
    const Snapshot& snapshot,
    const TransactionManager& tm
) {
    std::vector<CTID> candidate_ctids = index.find_entries(key);
    for (const auto& ctid : candidate_ctids) {
        auto tuple_opt = heap.get(ctid);
        if (tuple_opt.has_value()) {
            if (is_tuple_visible(tuple_opt->header, snapshot, tm)) {
                return std::make_pair(ctid, *tuple_opt);
            }
        }
    }
    return std::nullopt;
}

} // namespace pg
