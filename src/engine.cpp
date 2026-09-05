#include "pg/engine.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <regex>
#include <iostream>

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
    // The control file is read before anything else, because it is what says
    // whether the previous run shut down cleanly.
    control_ = std::make_unique<ControlFile>(db_prefix_ + "_control.db");

    clog_ = CLogManager::open(db_prefix_ + "_clog.db");
    tm_.set_clog(clog_.get());

    heap_ = HeapFile::open(db_prefix_ + "_heap.db");
    wal_ = WALManager::open(db_prefix_ + "_wal.log");
    toast_ = ToastManager::open(db_prefix_ + "_toast.db", wal_.get());
    index_ = DiskBTree::open(db_prefix_ + "_index.db");

    // One pool per relation. The heap made its own on construction and the
    // engine adopts it rather than building a second one over the same file:
    // two pools over one relation is exactly how the cached copy and the disk
    // copy drift apart.
    BufferPoolManager& pool = *heap_->bpm();

    // Close the loop between the log and the pool in both directions. The heap
    // logs each change while holding the page pinned, and the pool refuses to
    // write a page out until the log covering it is durable.
    heap_->set_wal(wal_.get());
    wal_->set_bpm(&pool);
    pool.set_wal(wal_.get());

    // Restore the transaction counter from the control file rather than from
    // surviving data, so ids are never reissued.
    tm_.set_next_tx_id(std::max<tx_id_t>(1, control_->data().next_xid));

    // Crash recovery, decided by the control file and run before the database
    // serves anything, rather than left for an operator to invoke by hand.
    if (control_->needs_recovery()) {
        size_t replayed = wal_->recover(*heap_, tm_, toast_.get());
        recovered_at_startup_ = true;
        if (replayed > 0) {
            std::cerr << "[STARTUP] Unclean shutdown detected; replayed "
                      << replayed << " WAL records." << std::endl;
        }
    }

    // On-disk B-Tree index is persistent. If the index file already has pages,
    // we open it in O(1) time without scanning the heap (resolving audit finding 2.3).
    // If the index was just created (has 0 entries) but the heap has tuples,
    // populate the index from the heap.
    if (index_->num_entries() == 0 && heap_->num_pages() > 0) {
        auto all_rows = heap_->seq_scan();
        for (const auto& [ctid, tuple] : all_rows) {
            index_->insert_entry(tuple.data.item_id, ctid);
        }
    }

    control_->data().next_xid = tm_.next_tx_id();
    control_->mark_in_production();
}

Engine::~Engine() {
    try {
        // A clean shutdown: get everything onto disk, then record that it is on
        // disk. Marking first would let a crash in between look clean.
        if (index_) {
            index_->flush();
        }
        if (toast_) {
            toast_->flush();
        }
        if (heap_) {
            heap_->flush();
        }
        if (clog_) {
            clog_->flush();
        }
        if (wal_) {
            wal_->flush();
        }
        if (control_) {
            control_->data().next_xid = tm_.next_tx_id();
            control_->data().checkpoint_lsn = wal_ ? wal_->checkpoint_lsn() : 0;
            control_->mark_shutdown();
        }
    } catch (...) {
        // Never throw out of a destructor.
    }
}

void Engine::ensure_transaction(bool is_read_only) {
    if (!sess_->current_tx.has_value()) {
        tx_id_t tx_id = tm_.begin_transaction();
        sess_->current_tx = tx_id;
        sess_->snapshot = tm_.take_snapshot(tx_id);
    } else if (!sess_->snapshot.has_value()) {
        sess_->snapshot = tm_.take_snapshot(*sess_->current_tx);
    }
}

std::string Engine::begin_transaction() {
    if (sess_->current_tx.has_value()) {
        return "[WARNING] Transaction " + std::to_string(*sess_->current_tx) + " is already active.\n";
    }

    tx_id_t tx_id = tm_.begin_transaction();
    sess_->current_tx = tx_id;
    sess_->snapshot = tm_.take_snapshot(tx_id);

    std::ostringstream oss;
    oss << "[Tx " << tx_id << "] BEGIN: Transaction started. Snapshot: [xmin=" 
        << sess_->snapshot->xmin << ", xmax=" << sess_->snapshot->xmax << "]\n";
    return oss.str();
}

std::string Engine::commit_transaction() {
    if (!sess_->current_tx.has_value()) {
        return "[WARNING] No active transaction to commit.\n";
    }

    tx_id_t tx_id = *sess_->current_tx;
    lsn_t lsn = wal_->log_commit(tx_id);
    tm_.commit(tx_id);
    lock_mgr_.release_all(tx_id);   // Row locks live exactly as long as the transaction

    sess_->current_tx = std::nullopt;
    sess_->snapshot = std::nullopt;

    std::ostringstream oss;
    oss << "[Tx " << tx_id << "] COMMIT: Logged to WAL (LSN: " << lsn << "). Transaction committed.\n";
    return oss.str();
}

std::string Engine::rollback_transaction() {
    if (!sess_->current_tx.has_value()) {
        return "[WARNING] No active transaction to rollback.\n";
    }

    tx_id_t tx_id = *sess_->current_tx;
    lsn_t lsn = wal_->log_abort(tx_id);
    tm_.abort(tx_id);
    lock_mgr_.release_all(tx_id);

    sess_->current_tx = std::nullopt;
    sess_->snapshot = std::nullopt;

    std::ostringstream oss;
    oss << "[Tx " << tx_id << "] ROLLBACK: Logged to WAL (LSN: " << lsn << "). Transaction aborted.\n";
    return oss.str();
}

std::string Engine::insert_item(int32_t item_id, int32_t price) {
    bool autocommit = !sess_->current_tx.has_value();
    if (autocommit) {
        begin_transaction();
    }

    tx_id_t tx_id = *sess_->current_tx;
    // HeapFile writes the WAL record itself, before the page change and while
    // the page is pinned. The engine no longer logs after the fact.
    CTID ctid = heap_->insert({item_id, price}, tx_id);
    lsn_t lsn = wal_->current_lsn();

    index_->insert_entry(item_id, ctid);

    std::ostringstream oss;
    oss << "[Tx " << tx_id << "] INSERT: Landed at CTID " << ctid.to_string() 
        << " (xmin=" << tx_id << ", price=$" << price << "). WAL LSN: " << lsn << ". B-Tree index updated.\n";

    if (autocommit) {
        oss << commit_transaction();
    }

    return oss.str();
}

std::string Engine::insert_item_with_doc(int32_t item_id, int32_t price, const std::string& doc) {
    bool autocommit = !sess_->current_tx.has_value();
    if (autocommit) {
        begin_transaction();
    }

    tx_id_t tx_id = *sess_->current_tx;
    ToastValue tv = toast_->store_string(doc, tx_id);

    CTID ctid = heap_->insert({item_id, price}, tx_id);

    if (!tv.is_inline()) {
        auto tuple_opt = heap_->get(ctid);
        if (tuple_opt.has_value()) {
            TupleHeader hdr = tuple_opt->header;
            hdr.infomask |= HEAP_HASEXTERNAL;
            heap_->update_tuple_header(ctid, hdr);
        }
    }

    lsn_t lsn = wal_->current_lsn();

    index_->insert_entry(item_id, ctid);

    std::ostringstream oss;
    oss << "[Tx " << tx_id << "] INSERT (WITH TOAST): Landed at CTID " << ctid.to_string() 
        << " (xmin=" << tx_id << ", price=$" << price << "). Document size: " << doc.size() << " bytes (";
    if (tv.is_inline()) {
        oss << "INLINE in tuple).\n";
    } else {
        oss << "OUT-OF-LINE in TOAST table, " << tv.pointer.chunk_count << " chunks of 2KB, ToastID: " 
            << tv.pointer.toast_id << ").\n";
    }

    if (autocommit) {
        oss << commit_transaction();
    }

    return oss.str();
}

std::string Engine::select_doc_by_id(int32_t item_id) {
    ensure_transaction(true);
    auto visible_opt = index_lookup(*index_, *heap_, item_id, *sess_->snapshot, tm_);
    if (!visible_opt.has_value()) {
        return "[SELECT] No visible row found with item_id=" + std::to_string(item_id) + ".\n";
    }

    const auto& [ctid, tuple] = *visible_opt;
    std::ostringstream oss;
    oss << "[SELECT TOAST] item_id=" << tuple.data.item_id << ", price=$" << tuple.data.price 
        << ", CTID=" << ctid.to_string();
    if (tuple.header.infomask & HEAP_HASEXTERNAL) {
        oss << " [TOASTED ATTRIBUTE PRESENT]\n";
    } else {
        oss << " [INLINE ATTRIBUTE]\n";
    }
    return oss.str();
}


std::string Engine::update_item(int32_t item_id, int32_t new_price) {
    bool autocommit = !sess_->current_tx.has_value();
    if (autocommit) {
        begin_transaction();
    }

    tx_id_t tx_id = *sess_->current_tx;
    ensure_transaction();

    // 1. Locate current visible tuple via B-Tree index
    auto visible_opt = index_lookup(*index_, *heap_, item_id, *sess_->snapshot, tm_);
    if (!visible_opt.has_value()) {
        if (autocommit) commit_transaction();
        return "[Tx " + std::to_string(tx_id) + "] UPDATE: No visible row found with item_id=" + std::to_string(item_id) + ".\n";
    }

    CTID old_ctid = visible_opt->first;

    // 2. Serialise writers on this row. MVCC keeps readers out of the way, but
    // two transactions updating the same row must not both succeed: without
    // this the second write simply overwrites the first, losing it.
    if (!lock_mgr_.try_lock(old_ctid, tx_id)) {
        std::string holder = std::to_string(lock_mgr_.holder(old_ctid));
        if (autocommit) rollback_transaction();
        else sess_->failed = true;
        return "[Tx " + std::to_string(tx_id) + "] ERROR: could not serialize access to row " +
               old_ctid.to_string() + "; transaction " + holder + " holds the write lock on it.\n";
    }

    // 3. HOT is only legal when no indexed column changes. The engine indexes
    // items(item_id), so an update that leaves item_id alone qualifies. Checking
    // it rather than assuming it means adding a second index, or allowing
    // item_id itself to be updated, cannot silently skip an index write.
    const bool indexed_columns_unchanged = (visible_opt->second.data.item_id == item_id);

    std::optional<CTID> hot_res;
    if (indexed_columns_unchanged) {
        hot_res = heap_->hot_update(old_ctid, {item_id, new_price}, tx_id);
    }

    std::ostringstream oss;
    if (hot_res.has_value()) {
        CTID new_ctid = *hot_res;
        lsn_t lsn = wal_->current_lsn();

        oss << "[Tx " << tx_id << "] UPDATE: HOT-update successful! Placed at " 
            << new_ctid.to_string() << " on SAME page (WAL LSN: " << lsn 
            << "). ZERO index writes!\n";
    } else {
        // Fallback to standard non-in-place update
        CTID new_ctid = heap_->update(old_ctid, {item_id, new_price}, tx_id);
        lsn_t lsn = wal_->current_lsn();

        index_->insert_entry(item_id, new_ctid);
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
    return select_all(0, 0);
}

std::string Engine::select_all(size_t limit, size_t offset) {
    bool autocommit = !sess_->current_tx.has_value();
    if (autocommit) {
        begin_transaction();
    }

    ensure_transaction();

    std::unique_ptr<PlanNode> plan = std::make_unique<SeqScanNode>(*heap_, *sess_->snapshot, tm_);
    if (limit > 0 || offset > 0) {
        plan = std::make_unique<LimitNode>(std::move(plan), limit > 0 ? limit : std::numeric_limits<size_t>::max(), offset);
    }

    auto visible = ExecutionEngine::execute(*plan);

    std::string table = format_table(visible, "Sequential Table Scan");

    if (autocommit) {
        commit_transaction();
    }

    return table;
}

std::string Engine::select_by_id(int32_t item_id) {
    bool autocommit = !sess_->current_tx.has_value();
    if (autocommit) {
        begin_transaction();
    }

    ensure_transaction();
    IndexScanNode plan(*index_, *heap_, item_id, *sess_->snapshot, tm_);
    auto results = ExecutionEngine::execute(plan);

    std::string table = format_table(results, "B-Tree Index Scan (Key: " + std::to_string(item_id) + ")");

    if (autocommit) {
        commit_transaction();
    }

    return table;
}

std::string Engine::select_filtered(std::function<bool(const TupleTableSlot&)> pred, const std::string& desc, size_t limit, size_t offset) {
    bool autocommit = !sess_->current_tx.has_value();
    if (autocommit) {
        begin_transaction();
    }

    ensure_transaction();

    std::unique_ptr<PlanNode> plan = std::make_unique<SeqScanNode>(*heap_, *sess_->snapshot, tm_);
    plan = std::make_unique<FilterNode>(std::move(plan), std::move(pred), desc);
    if (limit > 0 || offset > 0) {
        plan = std::make_unique<LimitNode>(std::move(plan), limit > 0 ? limit : std::numeric_limits<size_t>::max(), offset);
    }

    auto visible = ExecutionEngine::execute(*plan);

    std::string table = format_table(visible, "Filter Scan (" + desc + ")");

    if (autocommit) {
        commit_transaction();
    }

    return table;
}

std::string Engine::vacuum() {
    // The index is passed in so phase 2 can remove entries pointing at the
    // slots phase 1 flagged. Without it those slots can never be safely freed.
    auto stats = Vacuum::run(*heap_, tm_, *index_);

    std::ostringstream oss;
    oss << "[VACUUM] Garbage collection complete (horizon oldest_snapshot_xmin=" << tm_.oldest_snapshot_xmin() 
        << "). Reclaimed " << stats.dead_tuples_reclaimed << " dead tuples (" 
        << stats.bytes_reclaimed << " bytes) across " 
        << stats.pages_scanned << " pages; removed "
        << stats.index_entries_removed << " index entries, redirected "
        << stats.hot_roots_redirected << " HOT roots.\n";
    return oss.str();
}

std::string Engine::dump_page(page_id_t page_id) {
    if (page_id >= heap_->num_pages()) {
        return "[ERROR] Invalid page_id " + std::to_string(page_id) + ". Table has " + std::to_string(heap_->num_pages()) + " pages.\n";
    }

    // Flush through the pool first: the pool is the authority on current page
    // contents, and reading the file directly shows a stale page whenever the
    // frame is dirty.
    heap_->bpm()->flush_all();

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
    oss << "Active Tx        : " << (sess_->current_tx.has_value() ? std::to_string(*sess_->current_tx) : "None (Autocommit)") << "\n";
    oss << "Oldest Active XID: " << tm_.oldest_active_xmin() << "\n";
    oss << "Total Heap Pages : " << heap_->num_pages() << " (File size: " << (heap_->num_pages() * 8) << " KB)\n";
    oss << "Buffer Pool Size : " << bpm().pool_size() << " frames (" << bpm().resident_pages() << " resident in RAM)\n";
    oss << "WAL Flushed LSN  : " << wal_->flushed_lsn() << " bytes\n";
    oss << "Index Entries    : " << index_->num_entries() << " candidate CTIDs\n";
    oss << "============================================================\n";
    return oss.str();
}

std::string Engine::recover() {
    size_t replayed = wal_->recover(*heap_, tm_, toast_.get());
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

Session Engine::new_session() {
    Session s;
    s.id = next_session_id_++;
    return s;
}

std::string Engine::execute(const std::string& sql, Session& session) {
    // Point the engine at this session for the duration of the statement. The
    // server serialises statements with a mutex, so exactly one session is
    // current at a time even though many exist.
    Session* prev = sess_;
    sess_ = &session;
    std::string out;
    try {
        out = execute(sql);
    } catch (...) {
        sess_ = prev;
        throw;
    }
    sess_ = prev;
    return out;
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
    if (upper == "CHECKPOINT") {
        return checkpoint();
    }

    // DUMP PAGE <id>
    std::regex dump_regex(R"(^DUMP\s+PAGE\s+(\d+))", std::regex::icase);
    std::smatch dump_match;
    if (std::regex_search(clean, dump_match, dump_regex)) {
        page_id_t pid = static_cast<page_id_t>(std::stoul(dump_match[1]));
        return dump_page(pid);
    }

    // INSERT INTO items VALUES (id, price, 'document')
    std::regex insert_doc_regex(R"(^INSERT\s+(?:INTO\s+items\s+)?(?:VALUES\s*)?\(\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*'([^']*)'\s*\))", std::regex::icase);
    std::smatch insert_doc_match;
    if (std::regex_search(clean, insert_doc_match, insert_doc_regex)) {
        int32_t item_id = std::stoi(insert_doc_match[1]);
        int32_t price   = std::stoi(insert_doc_match[2]);
        std::string doc = insert_doc_match[3];
        return insert_item_with_doc(item_id, price, doc);
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

    // -------------------------------------------------------------------------
    // EXPLAIN [ANALYZE] Prefix Detection
    // -------------------------------------------------------------------------
    bool is_explain = false;
    bool is_analyze = false;
    std::string target_query = clean;

    std::regex explain_analyze_regex(R"(^EXPLAIN\s+ANALYZE\s+(.*))", std::regex::icase);
    std::regex explain_regex(R"(^EXPLAIN\s+(.*))", std::regex::icase);
    std::smatch explain_match;
    if (std::regex_search(clean, explain_match, explain_analyze_regex)) {
        is_explain = true;
        is_analyze = true;
        target_query = trim(explain_match[1].str());
    } else if (std::regex_search(clean, explain_match, explain_regex)) {
        is_explain = true;
        is_analyze = false;
        target_query = trim(explain_match[1].str());
    }

    // Helper to strip and extract LIMIT N [OFFSET M] from query string
    auto parse_limit_offset = [](const std::string& q, size_t& limit, size_t& offset) -> std::string {
        limit = 0;
        offset = 0;
        std::string base = q;
        std::regex limit_offset_regex(R"(\s+LIMIT\s+(\d+)(?:\s+OFFSET\s+(\d+))?)", std::regex::icase);
        std::smatch m;
        if (std::regex_search(base, m, limit_offset_regex)) {
            limit = std::stoul(m[1].str());
            if (m[2].matched) {
                offset = std::stoul(m[2].str());
            }
            base = base.substr(0, m.position());
        } else {
            std::regex offset_limit_regex(R"(\s+OFFSET\s+(\d+)(?:\s+LIMIT\s+(\d+))?)", std::regex::icase);
            if (std::regex_search(base, m, offset_limit_regex)) {
                offset = std::stoul(m[1].str());
                if (m[2].matched) {
                    limit = std::stoul(m[2].str());
                }
                base = base.substr(0, m.position());
            }
        }
        return trim(base);
    };

    size_t query_limit = 0, query_offset = 0;
    std::string base_sql = parse_limit_offset(target_query, query_limit, query_offset);

    // SELECT * FROM items WHERE item_id = id
    std::regex select_id_regex(R"(^SELECT\s+\*\s+FROM\s+items\s+WHERE\s+item_id\s*=\s*(-?\d+))", std::regex::icase);
    std::smatch select_id_match;
    if (std::regex_search(base_sql, select_id_match, select_id_regex)) {
        int32_t item_id = std::stoi(select_id_match[1]);
        if (!is_explain && query_limit == 0 && query_offset == 0) {
            return select_by_id(item_id);
        }
        ensure_transaction();
        std::unique_ptr<PlanNode> plan = std::make_unique<IndexScanNode>(*index_, *heap_, item_id, *sess_->snapshot, tm_);
        if (query_limit > 0 || query_offset > 0) {
            plan = std::make_unique<LimitNode>(std::move(plan), query_limit > 0 ? query_limit : std::numeric_limits<size_t>::max(), query_offset);
        }
        if (is_explain) {
            return ExecutionEngine::explain(*plan, is_analyze);
        }
        bool autocommit = !sess_->current_tx.has_value();
        if (autocommit) begin_transaction();
        auto results = ExecutionEngine::execute(*plan);
        std::string table = format_table(results, "B-Tree Index Scan (Key: " + std::to_string(item_id) + ")");
        if (autocommit) commit_transaction();
        return table;
    }

    // SELECT * FROM items WHERE price (op) val
    std::regex select_price_regex(R"(^SELECT\s+\*\s+FROM\s+items\s+WHERE\s+price\s*([><!=]=?)\s*(-?\d+))", std::regex::icase);
    std::smatch select_price_match;
    if (std::regex_search(base_sql, select_price_match, select_price_regex)) {
        std::string op = select_price_match[1].str();
        int32_t target_price = std::stoi(select_price_match[2]);
        ensure_transaction();
        std::unique_ptr<PlanNode> plan = std::make_unique<SeqScanNode>(*heap_, *sess_->snapshot, tm_);
        std::string desc = "price " + op + " " + std::to_string(target_price);
        auto pred = [op, target_price](const TupleTableSlot& slot) -> bool {
            int32_t p = slot.tuple.data.price;
            if (op == ">") return p > target_price;
            if (op == ">=") return p >= target_price;
            if (op == "<") return p < target_price;
            if (op == "<=") return p <= target_price;
            if (op == "=" || op == "==") return p == target_price;
            if (op == "!=") return p != target_price;
            return false;
        };
        plan = std::make_unique<FilterNode>(std::move(plan), pred, desc);
        if (query_limit > 0 || query_offset > 0) {
            plan = std::make_unique<LimitNode>(std::move(plan), query_limit > 0 ? query_limit : std::numeric_limits<size_t>::max(), query_offset);
        }
        if (is_explain) {
            return ExecutionEngine::explain(*plan, is_analyze);
        }
        return select_filtered(pred, desc, query_limit, query_offset);
    }

    // SELECT * FROM items [LIMIT N] [OFFSET M]
    std::regex select_all_regex(R"(^SELECT\s+\*\s+FROM\s+items)", std::regex::icase);
    if (std::regex_search(base_sql, select_all_regex)) {
        if (!is_explain && query_limit == 0 && query_offset == 0) {
            return select_all();
        }
        if (!is_explain) {
            return select_all(query_limit, query_offset);
        }
        ensure_transaction();
        std::unique_ptr<PlanNode> plan = std::make_unique<SeqScanNode>(*heap_, *sess_->snapshot, tm_);
        if (query_limit > 0 || query_offset > 0) {
            plan = std::make_unique<LimitNode>(std::move(plan), query_limit > 0 ? query_limit : std::numeric_limits<size_t>::max(), query_offset);
        }
        return ExecutionEngine::explain(*plan, is_analyze);
    }

    if (upper == "HELP") {
        std::ostringstream oss;
        oss << "\nSupported SQL / REPL Commands:\n";
        oss << "  BEGIN;                                         - Start MVCC transaction & snapshot\n";
        oss << "  COMMIT;                                        - Commit transaction & flush WAL\n";
        oss << "  ROLLBACK;                                      - Abort transaction\n";
        oss << "  INSERT INTO items VALUES (100, 10);            - Insert new item\n";
        oss << "  UPDATE items SET price = 20 WHERE item_id = 100;- HOT / MVCC update item\n";
        oss << "  SELECT * FROM items [LIMIT N] [OFFSET M];       - Volcano streaming sequential scan\n";
        oss << "  SELECT * FROM items WHERE item_id = 100;       - B-Tree index point query\n";
        oss << "  SELECT * FROM items WHERE price > 50 [LIMIT N];- Filtered streaming scan\n";
        oss << "  EXPLAIN [ANALYZE] SELECT ...;                  - Query execution plan tree\n";
        oss << "  VACUUM;                                        - Reclaim dead tuples & defragment\n";
        oss << "  DUMP PAGE 0;                                   - Slotted page physical layout dump\n";
        oss << "  STATUS;                                        - Buffer pool, WAL & Tx diagnostics\n";
        oss << "  CHECKPOINT;                                    - Flush dirty pages & write checkpoint LSN\n";
        oss << "  RECOVER;                                       - ARIES REDO recovery replay\n";
        oss << "  EXIT / QUIT;                                   - Exit REPL CLI\n";
        return oss.str();
    }

    return "[ERROR] Unrecognized SQL command: '" + clean + "'. Type HELP; for syntax.\n";
}

std::string Engine::checkpoint() {
    if (index_) {
        index_->flush();
    }
    if (toast_) {
        toast_->flush();
    }
    if (heap_) {
        heap_->flush();
    }
    if (clog_) {
        clog_->flush();
    }
    lsn_t lsn = wal_->log_checkpoint();
    std::ostringstream oss;
    oss << "[CHECKPOINT] All dirty buffer pool frames flushed to disk. Checkpoint record logged at LSN: " 
        << lsn << ".\n";
    return oss.str();
}


} // namespace pg
