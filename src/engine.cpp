#include "pg/engine.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <regex>

namespace pg {

// Helper to trim leading/trailing whitespace
static std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n;");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n;");
    return str.substr(first, (last - first + 1));
}

// Helper to convert to uppercase for case-insensitive matching
static std::string to_upper(const std::string& str) {
    std::string upper = str;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    return upper;
}

Engine::Engine(const std::string& db_prefix) : db_prefix_(db_prefix) {
    heap_ = HeapFile::open(db_prefix_ + "_heap.db");
    wal_ = WALManager::open(db_prefix_ + "_wal.log");
    bpm_ = std::make_unique<BufferPoolManager>(heap_->pager(), 16);
    toast_ = ToastManager::open(db_prefix_ + "_toast.db");

    // Wire buffer pool into heap: all page I/O now goes through shared_buffers
    heap_->set_bpm(bpm_.get());

    // Populate B-Tree index from existing table data
    auto all_rows = heap_->seq_scan();
    for (const auto& [ctid, tuple] : all_rows) {
        index_.insert_entry(tuple.data.item_id, ctid);
    }
}

void Engine::ensure_transaction(bool is_read_only) {
    if (!current_tx_.has_value()) {
        tx_id_t tx_id = tm_.begin_transaction();
        current_tx_ = tx_id;
        current_snapshot_ = tm_.take_snapshot(tx_id);
    } else if (!current_snapshot_.has_value()) {
        current_snapshot_ = tm_.take_snapshot(*current_tx_);
    }
}

std::string Engine::begin_transaction() {
    if (current_tx_.has_value()) {
        return "[WARNING] Transaction " + std::to_string(*current_tx_) + " is already active.\n";
    }

    tx_id_t tx_id = tm_.begin_transaction();
    current_tx_ = tx_id;
    current_snapshot_ = tm_.take_snapshot(tx_id);

    std::ostringstream oss;
    oss << "[Tx " << tx_id << "] BEGIN: Transaction started. Snapshot: [xmin=" 
        << current_snapshot_->xmin << ", xmax=" << current_snapshot_->xmax << "]\n";
    return oss.str();
}

std::string Engine::commit_transaction() {
    if (!current_tx_.has_value()) {
        return "[WARNING] No active transaction to commit.\n";
    }

    tx_id_t tx_id = *current_tx_;
    lsn_t lsn = wal_->log_commit(tx_id);
    tm_.commit(tx_id);

    current_tx_ = std::nullopt;
    current_snapshot_ = std::nullopt;

    std::ostringstream oss;
    oss << "[Tx " << tx_id << "] COMMIT: Logged to WAL (LSN: " << lsn << "). Transaction committed.\n";
    return oss.str();
}

std::string Engine::rollback_transaction() {
    if (!current_tx_.has_value()) {
        return "[WARNING] No active transaction to rollback.\n";
    }

    tx_id_t tx_id = *current_tx_;
    lsn_t lsn = wal_->log_abort(tx_id);
    tm_.abort(tx_id);

    current_tx_ = std::nullopt;
    current_snapshot_ = std::nullopt;

    std::ostringstream oss;
    oss << "[Tx " << tx_id << "] ROLLBACK: Logged to WAL (LSN: " << lsn << "). Transaction aborted.\n";
    return oss.str();
}

std::string Engine::insert_item(int32_t item_id, int32_t price) {
    bool autocommit = !current_tx_.has_value();
    if (autocommit) {
        begin_transaction();
    }

    tx_id_t tx_id = *current_tx_;
    CTID ctid = heap_->insert({item_id, price}, tx_id);

    auto tuple_opt = heap_->get(ctid);
    lsn_t lsn = 0;
    if (tuple_opt.has_value()) {
        lsn = wal_->log_insert(tx_id, ctid.page, ctid.slot, *tuple_opt);
    }

    index_.insert_entry(item_id, ctid);

    std::ostringstream oss;
    oss << "[Tx " << tx_id << "] INSERT: Landed at CTID " << ctid.to_string() 
        << " (xmin=" << tx_id << ", price=$" << price << "). WAL LSN: " << lsn << ". B-Tree index updated.\n";

    if (autocommit) {
        oss << commit_transaction();
    }

    return oss.str();
}

std::string Engine::update_item(int32_t item_id, int32_t new_price) {
    bool autocommit = !current_tx_.has_value();
    if (autocommit) {
        begin_transaction();
    }

    tx_id_t tx_id = *current_tx_;
    ensure_transaction();

    // 1. Locate current visible tuple via B-Tree index
    auto visible_opt = index_lookup(index_, *heap_, item_id, *current_snapshot_, tm_);
    if (!visible_opt.has_value()) {
        if (autocommit) commit_transaction();
        return "[Tx " + std::to_string(tx_id) + "] UPDATE: No visible row found with item_id=" + std::to_string(item_id) + ".\n";
    }

    CTID old_ctid = visible_opt->first;

    // 2. Try HOT Update first (same page, 0 index writes)
    auto hot_res = heap_->hot_update(old_ctid, {item_id, new_price}, tx_id);

    std::ostringstream oss;
    if (hot_res.has_value()) {
        CTID new_ctid = *hot_res;
        auto new_tuple_opt = heap_->get(new_ctid);
        lsn_t lsn = 0;
        if (new_tuple_opt.has_value()) {
            lsn = wal_->log_update(tx_id, old_ctid, new_ctid, *new_tuple_opt);
        }

        oss << "[Tx " << tx_id << "] UPDATE: HOT-update successful! Placed at " 
            << new_ctid.to_string() << " on SAME page (WAL LSN: " << lsn 
            << "). ZERO index writes!\n";
    } else {
        // Fallback to standard non-in-place update
        CTID new_ctid = heap_->update(old_ctid, {item_id, new_price}, tx_id);
        auto new_tuple_opt = heap_->get(new_ctid);
        lsn_t lsn = 0;
        if (new_tuple_opt.has_value()) {
            lsn = wal_->log_update(tx_id, old_ctid, new_ctid, *new_tuple_opt);
        }

        index_.insert_entry(item_id, new_ctid);
        oss << "[Tx " << tx_id << "] UPDATE: Standard update placed at " 
            << new_ctid.to_string() << " (WAL LSN: " << lsn 
            << "). B-Tree index entry added.\n";
    }

    if (autocommit) {
        oss << commit_transaction();
    }

    return oss.str();
}

std::string Engine::select_all() {
    bool autocommit = !current_tx_.has_value();
    if (autocommit) {
        begin_transaction();
    }

    ensure_transaction();
    auto visible = heap_->seq_scan(*current_snapshot_, tm_);

    std::string table = format_table(visible, "Sequential Table Scan");

    if (autocommit) {
        commit_transaction();
    }

    return table;
}

std::string Engine::select_by_id(int32_t item_id) {
    bool autocommit = !current_tx_.has_value();
    if (autocommit) {
        begin_transaction();
    }

    ensure_transaction();
    auto visible_opt = index_lookup(index_, *heap_, item_id, *current_snapshot_, tm_);

    std::vector<std::pair<CTID, HeapTuple>> results;
    if (visible_opt.has_value()) {
        results.push_back(*visible_opt);
    }

    std::string table = format_table(results, "B-Tree Index Scan (Key: " + std::to_string(item_id) + ")");

    if (autocommit) {
        commit_transaction();
    }

    return table;
}

std::string Engine::vacuum() {
    auto stats = Vacuum::run(*heap_, tm_);

    std::ostringstream oss;
    oss << "[VACUUM] Garbage collection complete (Cutoff oldest_active_xmin=" << tm_.oldest_active_xmin() 
        << "). Reclaimed " << stats.dead_tuples_reclaimed << " dead tuples (" 
        << stats.bytes_reclaimed << " bytes) across " 
        << stats.pages_scanned << " pages.\n";
    return oss.str();
}

std::string Engine::dump_page(page_id_t page_id) {
    if (page_id >= heap_->num_pages()) {
        return "[ERROR] Invalid page_id " + std::to_string(page_id) + ". Table has " + std::to_string(heap_->num_pages()) + " pages.\n";
    }

    // Flush dirty buffer pool pages to disk so we read fresh data
    if (bpm_) {
        bpm_->flush_all();
    }

    std::vector<uint8_t> page_buffer(PAGE_SIZE, 0);
    heap_->pager().read_page(page_id, page_buffer.data());
    Page page(page_buffer.data());

    std::ostringstream oss;
    oss << "\n====================== PAGE " << page_id << " LAYOUT DUMP ======================\n";
    oss << "Header Size   : " << sizeof(PageHeaderData) << " bytes\n";
    oss << "pd_lsn        : " << page.header().pd_lsn << "\n";
    oss << "pd_lower      : " << page.header().pd_lower << " (end of line pointers)\n";
    oss << "pd_upper      : " << page.header().pd_upper << " (start of youngest tuple)\n";
    oss << "Free Space    : " << page.free_space() << " bytes\n";
    oss << "Slot Count    : " << page.num_slots() << " items\n";
    oss << "--------------------------------------------------------------\n";

    for (slot_id_t s = 1; s <= page.num_slots(); ++s) {
        auto lp = page.get_line_pointer(s);
        if (lp.has_value()) {
            oss << " Slot " << std::setw(2) << s << ": offset=" << std::setw(4) << lp->lp_offset
                << ", len=" << std::setw(3) << lp->length() << ", flags=";
            
            if (lp->flags() == ItemFlags::UNUSED) {
                oss << "UNUSED (Dead Hole)\n";
            } else if (lp->flags() == ItemFlags::REDIRECT) {
                oss << "REDIRECT -> Slot " << lp->lp_offset << "\n";
            } else {
                size_t len = 0;
                const uint8_t* ptr = page.get_tuple_ptr(s, &len);
                if (ptr != nullptr && len >= sizeof(HeapTuple)) {
                    HeapTuple tuple = HeapTuple::deserialize(ptr, len);
                    oss << "NORMAL [xmin=" << tuple.header.xmin << ", xmax=" << tuple.header.xmax
                        << ", item_id=" << tuple.data.item_id << ", price=$" << tuple.data.price
                        << ", t_ctid=" << tuple.header.t_ctid.to_string();
                    if (tuple.header.infomask & HEAP_HOT_UPDATED) oss << ", HOT_UPDATED";
                    if (tuple.header.infomask & HEAP_ONLY_TUPLE)  oss << ", HEAP_ONLY";
                    oss << "]\n";
                } else {
                    oss << "NORMAL [raw bytes len=" << len << "]\n";
                }
            }
        }
    }
    oss << "==============================================================\n";
    return oss.str();
}

std::string Engine::status() {
    std::ostringstream oss;
    oss << "\n================== POSTGRES ENGINE STATUS ==================\n";
    oss << "Active Tx        : " << (current_tx_.has_value() ? std::to_string(*current_tx_) : "None (Autocommit)") << "\n";
    oss << "Oldest Active XID: " << tm_.oldest_active_xmin() << "\n";
    oss << "Total Heap Pages : " << heap_->num_pages() << " (File size: " << (heap_->num_pages() * 8) << " KB)\n";
    oss << "Buffer Pool Size : " << bpm_->pool_size() << " frames (" << bpm_->resident_pages() << " resident in RAM)\n";
    oss << "WAL Flushed LSN  : " << wal_->flushed_lsn() << " bytes\n";
    oss << "Index Entries    : " << index_.num_entries() << " candidate CTIDs\n";
    oss << "============================================================\n";
    return oss.str();
}

std::string Engine::recover() {
    size_t replayed = wal_->recover(*heap_, tm_);
    std::ostringstream oss;
    oss << "[REDO RECOVERY] Successfully scanned WAL and replayed " << replayed 
        << " committed log records into heap table.\n";
    return oss.str();
}

std::string Engine::format_table(const std::vector<std::pair<CTID, HeapTuple>>& tuples, const std::string& scan_method) {
    std::ostringstream oss;
    oss << "\n+---------+-------+-------+-------+--------+\n";
    oss << "| item_id | price | xmin  | xmax  | CTID   |\n";
    oss << "+---------+-------+-------+-------+--------+\n";

    if (tuples.empty()) {
        oss << "| (0 rows returned)                       |\n";
    } else {
        for (const auto& [ctid, tuple] : tuples) {
            oss << "| " << std::setw(7) << tuple.data.item_id
                << " | $" << std::setw(5) << tuple.data.price
                << " | " << std::setw(5) << tuple.header.xmin
                << " | " << std::setw(5) << tuple.header.xmax
                << " | " << std::setw(6) << ctid.to_string() << " |\n";
        }
    }
    oss << "+---------+-------+-------+-------+--------+\n";
    oss << "(" << tuples.size() << " " << (tuples.size() == 1 ? "row" : "rows") 
        << " returned via " << scan_method << ")\n";
    return oss.str();
}

std::string Engine::execute(const std::string& sql) {
    std::string clean = trim(sql);
    if (clean.empty()) return "";

    std::string upper = to_upper(clean);

    if (upper == "BEGIN" || upper == "START TRANSACTION") {
        return begin_transaction();
    }
    if (upper == "COMMIT" || upper == "END") {
        return commit_transaction();
    }
    if (upper == "ROLLBACK" || upper == "ABORT") {
        return rollback_transaction();
    }
    if (upper == "VACUUM") {
        return vacuum();
    }
    if (upper == "STATUS") {
        return status();
    }
    if (upper == "RECOVER") {
        return recover();
    }

    // DUMP PAGE <id>
    std::regex dump_regex(R"(^DUMP\s+PAGE\s+(\d+))", std::regex::icase);
    std::smatch dump_match;
    if (std::regex_search(clean, dump_match, dump_regex)) {
        page_id_t pid = static_cast<page_id_t>(std::stoul(dump_match[1]));
        return dump_page(pid);
    }

    // INSERT INTO items VALUES (id, price)
    std::regex insert_regex(R"(^INSERT\s+(?:INTO\s+items\s+)?(?:VALUES\s*)?\(\s*(-?\d+)\s*,\s*(-?\d+)\s*\))", std::regex::icase);
    std::smatch insert_match;
    if (std::regex_search(clean, insert_match, insert_regex)) {
        int32_t item_id = std::stoi(insert_match[1]);
        int32_t price   = std::stoi(insert_match[2]);
        return insert_item(item_id, price);
    }

    // UPDATE items SET price = val WHERE item_id = id
    std::regex update_regex(R"(^UPDATE\s+(?:items\s+)?SET\s+price\s*=\s*(-?\d+)\s+WHERE\s+item_id\s*=\s*(-?\d+))", std::regex::icase);
    std::smatch update_match;
    if (std::regex_search(clean, update_match, update_regex)) {
        int32_t price   = std::stoi(update_match[1]);
        int32_t item_id = std::stoi(update_match[2]);
        return update_item(item_id, price);
    }

    // SELECT * FROM items WHERE item_id = id
    std::regex select_id_regex(R"(^SELECT\s+\*\s+FROM\s+items\s+WHERE\s+item_id\s*=\s*(-?\d+))", std::regex::icase);
    std::smatch select_id_match;
    if (std::regex_search(clean, select_id_match, select_id_regex)) {
        int32_t item_id = std::stoi(select_id_match[1]);
        return select_by_id(item_id);
    }

    // SELECT * FROM items
    std::regex select_all_regex(R"(^SELECT\s+\*\s+FROM\s+items)", std::regex::icase);
    if (std::regex_search(clean, select_all_regex)) {
        return select_all();
    }

    if (upper == "HELP") {
        std::ostringstream oss;
        oss << "\nSupported SQL / REPL Commands:\n";
        oss << "  BEGIN;                                         - Start MVCC transaction & snapshot\n";
        oss << "  COMMIT;                                        - Commit transaction & flush WAL\n";
        oss << "  ROLLBACK;                                      - Abort transaction\n";
        oss << "  INSERT INTO items VALUES (100, 10);            - Insert new item\n";
        oss << "  UPDATE items SET price = 20 WHERE item_id = 100;- HOT / MVCC update item\n";
        oss << "  SELECT * FROM items;                           - Sequential table scan\n";
        oss << "  SELECT * FROM items WHERE item_id = 100;       - B-Tree index point query\n";
        oss << "  VACUUM;                                        - Reclaim dead tuples & defragment\n";
        oss << "  DUMP PAGE 0;                                   - Slotted page physical layout dump\n";
        oss << "  STATUS;                                        - Buffer pool, WAL & Tx diagnostics\n";
        oss << "  RECOVER;                                       - ARIES REDO recovery replay\n";
        oss << "  EXIT / QUIT;                                   - Exit REPL CLI\n";
        return oss.str();
    }

    return "[ERROR] Unrecognized SQL command: '" + clean + "'. Type HELP; for syntax.\n";
}

} // namespace pg
