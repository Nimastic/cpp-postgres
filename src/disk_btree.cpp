#include "pg/disk_btree.h"
#include <iostream>
#include <sstream>
#include <vector>
#include <stdexcept>
#include <cassert>

namespace pg {

DiskBTree::DiskBTree(std::unique_ptr<Pager> pager, BufferPoolManager* bpm)
    : pager_(std::move(pager)), bpm_(bpm)
{
    if (pager_ && bpm_ == nullptr) {
        bpm_owned_ = std::make_unique<BufferPoolManager>(*pager_, 16);
        bpm_ = bpm_owned_.get();
    }

    if (pager_ && pager_->num_pages() == 0) {
        // Initialize Page 0 as initial Leaf + Root node
        page_id_t root_pid = pager_->allocate_page();
        std::vector<uint8_t> page_buf(PAGE_SIZE, 0);

        auto* header = reinterpret_cast<BTreePageHeader*>(page_buf.data());
        header->level = 0;
        header->flags = BTREE_LEAF | BTREE_ROOT;
        header->num_keys = 0;
        header->parent = INVALID_PAGE_ID;
        header->right_sibling = INVALID_PAGE_ID;

        write_page(root_pid, page_buf.data());
        root_page_id_ = root_pid;
    } else if (pager_) {
        // Find existing root page by inspecting page headers
        std::vector<uint8_t> page_buf(PAGE_SIZE, 0);
        size_t total = pager_->num_pages();
        for (page_id_t pid = 0; pid < total; ++pid) {
            read_page(pid, page_buf.data());
            auto* header = reinterpret_cast<BTreePageHeader*>(page_buf.data());
            if (header->flags & BTREE_ROOT) {
                root_page_id_ = pid;
                break;
            }
        }
    }
}

DiskBTree::~DiskBTree() {
    try {
        flush();
    } catch (...) {
        // Destructor must never throw
    }
}

std::unique_ptr<DiskBTree> DiskBTree::open(const std::string& filepath, BufferPoolManager* bpm) {
    auto pager = Pager::open(filepath);
    return std::make_unique<DiskBTree>(std::move(pager), bpm);
}

void DiskBTree::flush() {
    if (bpm_owned_) {
        bpm_owned_->flush_all();
    }
    if (pager_) {
        pager_->sync();
    }
}

void DiskBTree::read_page(page_id_t page_id, void* buffer) const {
    if (bpm_) {
        Page* p = bpm_->fetch_page(page_id);
        if (p) {
            std::memcpy(buffer, p->data(), PAGE_SIZE);
            bpm_->unpin_page(page_id, false);
            return;
        }
    }
    pager_->read_page(page_id, buffer);
}

void DiskBTree::write_page(page_id_t page_id, const void* buffer) {
    if (bpm_) {
        Page* p = bpm_->fetch_page(page_id);
        if (p) {
            std::memcpy(p->data(), buffer, PAGE_SIZE);
            bpm_->unpin_page(page_id, true);
            return;
        }
    }
    pager_->write_page(page_id, buffer);
}

page_id_t DiskBTree::find_leaf_page(index_key_t key) const {
    page_id_t curr_pid = root_page_id_;
    std::vector<uint8_t> page_buf(PAGE_SIZE, 0);

    while (true) {
        read_page(curr_pid, page_buf.data());
        auto* header = reinterpret_cast<BTreePageHeader*>(page_buf.data());

        if (header->level == 0 || (header->flags & BTREE_LEAF)) {
            return curr_pid; // Reached leaf node
        }

        // Search internal node entries to select child pointer
        auto* entries = reinterpret_cast<BTreeInternalEntry*>(page_buf.data() + sizeof(BTreePageHeader));
        page_id_t next_pid = entries[0].child_page_id; // Default to leftmost child

        for (uint16_t i = 1; i < header->num_keys; ++i) {
            if (key >= entries[i].key) {
                next_pid = entries[i].child_page_id;
            } else {
                break;
            }
        }

        curr_pid = next_pid;
    }
}

void DiskBTree::insert_entry(index_key_t key, const CTID& ctid) {
    page_id_t leaf_pid = find_leaf_page(key);

    std::vector<uint8_t> page_buf(PAGE_SIZE, 0);
    read_page(leaf_pid, page_buf.data());

    auto* header = reinterpret_cast<BTreePageHeader*>(page_buf.data());
    auto* entries = reinterpret_cast<BTreeLeafEntry*>(page_buf.data() + sizeof(BTreePageHeader));

    // Find insertion index in sorted order
    uint16_t insert_idx = 0;
    while (insert_idx < header->num_keys && entries[insert_idx].key <= key) {
        insert_idx++;
    }

    // Shift elements right to make room
    for (int i = static_cast<int>(header->num_keys); i > static_cast<int>(insert_idx); --i) {
        entries[i] = entries[i - 1];
    }

    entries[insert_idx].key = key;
    entries[insert_idx].ctid = ctid;
    header->num_keys++;

    write_page(leaf_pid, page_buf.data());

    // Check if leaf needs splitting
    if (header->num_keys >= BTREE_MAX_LEAF_KEYS) {
        split_leaf_page(leaf_pid);
    }
}

void DiskBTree::split_leaf_page(page_id_t leaf_pid) {
    std::vector<uint8_t> left_buf(PAGE_SIZE, 0);
    read_page(leaf_pid, left_buf.data());

    auto* left_header = reinterpret_cast<BTreePageHeader*>(left_buf.data());
    auto* left_entries = reinterpret_cast<BTreeLeafEntry*>(left_buf.data() + sizeof(BTreePageHeader));

    uint16_t total_keys = left_header->num_keys;
    uint16_t mid = total_keys / 2;

    // Allocate right leaf page
    page_id_t right_pid = pager_->allocate_page();
    std::vector<uint8_t> right_buf(PAGE_SIZE, 0);

    auto* right_header = reinterpret_cast<BTreePageHeader*>(right_buf.data());
    auto* right_entries = reinterpret_cast<BTreeLeafEntry*>(right_buf.data() + sizeof(BTreePageHeader));

    right_header->level = 0;
    right_header->flags = BTREE_LEAF;
    right_header->num_keys = total_keys - mid;
    right_header->parent = left_header->parent;
    right_header->right_sibling = left_header->right_sibling;

    // Copy upper half of entries to right page
    for (uint16_t i = 0; i < right_header->num_keys; ++i) {
        right_entries[i] = left_entries[mid + i];
    }

    index_key_t promoted_key = right_entries[0].key;

    // Truncate left page
    left_header->num_keys = mid;
    left_header->right_sibling = right_pid;

    if (left_header->flags & BTREE_ROOT) {
        // Root is splitting -> Create new Root page
        page_id_t new_root_pid = pager_->allocate_page();
        std::vector<uint8_t> root_buf(PAGE_SIZE, 0);

        auto* root_header = reinterpret_cast<BTreePageHeader*>(root_buf.data());
        auto* root_entries = reinterpret_cast<BTreeInternalEntry*>(root_buf.data() + sizeof(BTreePageHeader));

        root_header->level = 1;
        root_header->flags = BTREE_ROOT;
        root_header->num_keys = 2;
        root_header->parent = INVALID_PAGE_ID;
        root_header->right_sibling = INVALID_PAGE_ID;

        // Entry 0: points to left child (all keys < promoted_key)
        root_entries[0].key = std::numeric_limits<index_key_t>::min();
        root_entries[0].child_page_id = leaf_pid;

        // Entry 1: points to right child (all keys >= promoted_key)
        root_entries[1].key = promoted_key;
        root_entries[1].child_page_id = right_pid;

        left_header->flags &= ~BTREE_ROOT;
        left_header->parent = new_root_pid;
        right_header->parent = new_root_pid;

        root_page_id_ = new_root_pid;

        write_page(leaf_pid, left_buf.data());
        write_page(right_pid, right_buf.data());
        write_page(new_root_pid, root_buf.data());
    } else {
        // Non-root leaf -> Insert promoted key into existing parent
        write_page(leaf_pid, left_buf.data());
        write_page(right_pid, right_buf.data());
        insert_into_internal(left_header->parent, promoted_key, right_pid);
    }
}

void DiskBTree::insert_into_internal(page_id_t internal_pid, index_key_t key, page_id_t right_child_pid) {
    std::vector<uint8_t> page_buf(PAGE_SIZE, 0);
    read_page(internal_pid, page_buf.data());

    auto* header = reinterpret_cast<BTreePageHeader*>(page_buf.data());
    auto* entries = reinterpret_cast<BTreeInternalEntry*>(page_buf.data() + sizeof(BTreePageHeader));

    uint16_t insert_idx = 0;
    while (insert_idx < header->num_keys && entries[insert_idx].key <= key) {
        insert_idx++;
    }

    for (int i = static_cast<int>(header->num_keys); i > static_cast<int>(insert_idx); --i) {
        entries[i] = entries[i - 1];
    }

    entries[insert_idx].key = key;
    entries[insert_idx].child_page_id = right_child_pid;
    header->num_keys++;

    write_page(internal_pid, page_buf.data());

    if (header->num_keys >= BTREE_MAX_INTERNAL_KEYS) {
        split_internal_page(internal_pid);
    }
}

void DiskBTree::split_internal_page(page_id_t internal_pid) {
    std::vector<uint8_t> left_buf(PAGE_SIZE, 0);
    read_page(internal_pid, left_buf.data());

    auto* left_header = reinterpret_cast<BTreePageHeader*>(left_buf.data());
    auto* left_entries = reinterpret_cast<BTreeInternalEntry*>(left_buf.data() + sizeof(BTreePageHeader));

    uint16_t total_keys = left_header->num_keys;
    uint16_t mid = total_keys / 2;

    page_id_t right_pid = pager_->allocate_page();
    std::vector<uint8_t> right_buf(PAGE_SIZE, 0);

    auto* right_header = reinterpret_cast<BTreePageHeader*>(right_buf.data());
    auto* right_entries = reinterpret_cast<BTreeInternalEntry*>(right_buf.data() + sizeof(BTreePageHeader));

    right_header->level = left_header->level;
    right_header->flags = 0; // Not leaf, not root
    right_header->num_keys = total_keys - mid;
    right_header->parent = left_header->parent;
    right_header->right_sibling = INVALID_PAGE_ID;

    for (uint16_t i = 0; i < right_header->num_keys; ++i) {
        right_entries[i] = left_entries[mid + i];
    }

    index_key_t promoted_key = right_entries[0].key;
    left_header->num_keys = mid;

    if (left_header->flags & BTREE_ROOT) {
        page_id_t new_root_pid = pager_->allocate_page();
        std::vector<uint8_t> root_buf(PAGE_SIZE, 0);

        auto* root_header = reinterpret_cast<BTreePageHeader*>(root_buf.data());
        auto* root_entries = reinterpret_cast<BTreeInternalEntry*>(root_buf.data() + sizeof(BTreePageHeader));

        root_header->level = left_header->level + 1;
        root_header->flags = BTREE_ROOT;
        root_header->num_keys = 2;
        root_header->parent = INVALID_PAGE_ID;
        root_header->right_sibling = INVALID_PAGE_ID;

        root_entries[0].key = std::numeric_limits<index_key_t>::min();
        root_entries[0].child_page_id = internal_pid;

        root_entries[1].key = promoted_key;
        root_entries[1].child_page_id = right_pid;

        left_header->flags &= ~BTREE_ROOT;
        left_header->parent = new_root_pid;
        right_header->parent = new_root_pid;

        root_page_id_ = new_root_pid;

        write_page(internal_pid, left_buf.data());
        write_page(right_pid, right_buf.data());
        write_page(new_root_pid, root_buf.data());
    } else {
        write_page(internal_pid, left_buf.data());
        write_page(right_pid, right_buf.data());
        insert_into_internal(left_header->parent, promoted_key, right_pid);
    }
}

std::vector<CTID> DiskBTree::find_entries(index_key_t key) {
    std::vector<CTID> results;
    page_id_t leaf_pid = find_leaf_page(key);
    std::vector<uint8_t> page_buf(PAGE_SIZE, 0);

    while (leaf_pid != INVALID_PAGE_ID) {
        read_page(leaf_pid, page_buf.data());
        auto* header = reinterpret_cast<BTreePageHeader*>(page_buf.data());
        auto* entries = reinterpret_cast<BTreeLeafEntry*>(page_buf.data() + sizeof(BTreePageHeader));

        bool found_any = false;
        bool past_key = false;

        for (uint16_t i = 0; i < header->num_keys; ++i) {
            if (entries[i].key == key) {
                results.push_back(entries[i].ctid);
                found_any = true;
            } else if (entries[i].key > key) {
                past_key = true;
                break;
            }
        }

        if (past_key || !found_any && results.size() > 0) {
            break; // Finished collecting all matching keys
        }

        leaf_pid = header->right_sibling;
    }

    return results;
}

std::vector<std::pair<index_key_t, CTID>> DiskBTree::range_scan(index_key_t min_key, index_key_t max_key) {
    std::vector<std::pair<index_key_t, CTID>> results;
    page_id_t leaf_pid = find_leaf_page(min_key);
    std::vector<uint8_t> page_buf(PAGE_SIZE, 0);

    while (leaf_pid != INVALID_PAGE_ID) {
        read_page(leaf_pid, page_buf.data());
        auto* header = reinterpret_cast<BTreePageHeader*>(page_buf.data());
        auto* entries = reinterpret_cast<BTreeLeafEntry*>(page_buf.data() + sizeof(BTreePageHeader));

        bool stop = false;
        for (uint16_t i = 0; i < header->num_keys; ++i) {
            if (entries[i].key >= min_key && entries[i].key <= max_key) {
                results.emplace_back(entries[i].key, entries[i].ctid);
            } else if (entries[i].key > max_key) {
                stop = true;
                break;
            }
        }

        if (stop) break;
        leaf_pid = header->right_sibling;
    }

    return results;
}

page_id_t DiskBTree::leftmost_leaf_page() const {
    page_id_t curr = root_page_id_;
    std::vector<uint8_t> page_buf(PAGE_SIZE, 0);

    while (true) {
        read_page(curr, page_buf.data());
        auto* header = reinterpret_cast<const BTreePageHeader*>(page_buf.data());
        if (header->level == 0 || (header->flags & BTREE_LEAF)) {
            return curr;
        }
        auto* entries = reinterpret_cast<const BTreeInternalEntry*>(page_buf.data() + sizeof(BTreePageHeader));
        if (header->num_keys == 0) return curr;
        curr = entries[0].child_page_id;
    }
}

bool DiskBTree::remove_entry(index_key_t key, const CTID& ctid) {
    page_id_t leaf_pid = find_leaf_page(key);
    std::vector<uint8_t> page_buf(PAGE_SIZE, 0);

    while (leaf_pid != INVALID_PAGE_ID) {
        read_page(leaf_pid, page_buf.data());
        auto* header = reinterpret_cast<BTreePageHeader*>(page_buf.data());
        auto* entries = reinterpret_cast<BTreeLeafEntry*>(page_buf.data() + sizeof(BTreePageHeader));

        bool past_key = false;
        for (uint16_t i = 0; i < header->num_keys; ++i) {
            if (entries[i].key == key && entries[i].ctid == ctid) {
                for (uint16_t j = i; j + 1 < header->num_keys; ++j) {
                    entries[j] = entries[j + 1];
                }
                header->num_keys--;
                write_page(leaf_pid, page_buf.data());
                return true;
            } else if (entries[i].key > key) {
                past_key = true;
                break;
            }
        }

        if (past_key) break;
        leaf_pid = header->right_sibling;
    }
    return false;
}

size_t DiskBTree::num_entries() const {
    page_id_t curr = leftmost_leaf_page();
    std::vector<uint8_t> page_buf(PAGE_SIZE, 0);
    size_t total = 0;

    while (curr != INVALID_PAGE_ID) {
        read_page(curr, page_buf.data());
        auto* header = reinterpret_cast<const BTreePageHeader*>(page_buf.data());
        total += header->num_keys;
        curr = header->right_sibling;
    }
    return total;
}

size_t DiskBTree::num_unique_keys() const {
    page_id_t curr = leftmost_leaf_page();
    std::vector<uint8_t> page_buf(PAGE_SIZE, 0);
    size_t unique_count = 0;
    bool first = true;
    index_key_t prev_key = 0;

    while (curr != INVALID_PAGE_ID) {
        read_page(curr, page_buf.data());
        auto* header = reinterpret_cast<const BTreePageHeader*>(page_buf.data());
        auto* entries = reinterpret_cast<const BTreeLeafEntry*>(page_buf.data() + sizeof(BTreePageHeader));

        for (uint16_t i = 0; i < header->num_keys; ++i) {
            if (first || entries[i].key != prev_key) {
                unique_count++;
                prev_key = entries[i].key;
                first = false;
            }
        }
        curr = header->right_sibling;
    }
    return unique_count;
}

std::string DiskBTree::dump() const {
    std::ostringstream oss;
    oss << "================== ON-DISK B-TREE INDEX DUMP ==================\n";
    oss << "Root Page ID: " << root_page_id_ << ", Total Disk Pages: " << pager_->num_pages() << "\n";
    oss << "Total Entries: " << num_entries() << ", Unique Keys: " << num_unique_keys() << "\n";

    page_id_t curr = leftmost_leaf_page();
    std::vector<uint8_t> page_buf(PAGE_SIZE, 0);
    while (curr != INVALID_PAGE_ID) {
        read_page(curr, page_buf.data());
        auto* header = reinterpret_cast<const BTreePageHeader*>(page_buf.data());
        auto* entries = reinterpret_cast<const BTreeLeafEntry*>(page_buf.data() + sizeof(BTreePageHeader));
        oss << "  Leaf Page " << curr << " (" << header->num_keys << " keys, next: " 
            << (header->right_sibling == INVALID_PAGE_ID ? "none" : std::to_string(header->right_sibling)) << "):\n";
        for (uint16_t i = 0; i < header->num_keys; ++i) {
            oss << "    [" << entries[i].key << "] -> " << entries[i].ctid.to_string() << "\n";
        }
        curr = header->right_sibling;
    }
    oss << "===============================================================\n";
    return oss.str();
}

} // namespace pg
