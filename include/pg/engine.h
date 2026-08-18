#pragma once

#include "pg/constants.h"
#include "pg/pager.h"
#include "pg/page.h"
#include "pg/tuple.h"
#include "pg/tx.h"
#include "pg/mvcc.h"
#include "pg/heap.h"
#include "pg/vacuum.h"
#include "pg/btree.h"
#include "pg/buffer_pool.h"
#include "pg/wal.h"
#include "pg/toast.h"
#include "pg/clog.h"
#include <string>
#include <memory>
#include <optional>
#include <vector>

namespace pg {

// Unified PostgreSQL Database Engine
// Integrates all 10 core subsystems into a single cohesive interface
class Engine {
public:
    explicit Engine(const std::string& db_prefix = "pg_data");
    ~Engine() = default;

    // Execute an arbitrary SQL / REPL command string and return formatted result output
    std::string execute(const std::string& sql);

    // Transaction Control
    std::string begin_transaction();
    std::string commit_transaction();
    std::string rollback_transaction();

    // DML
    std::string insert_item(int32_t item_id, int32_t price);
    std::string update_item(int32_t item_id, int32_t new_price);
    std::string select_all();
    std::string select_by_id(int32_t item_id);

    // Maintenance & Diagnostics
    std::string vacuum();
    std::string dump_page(page_id_t page_id);
    std::string status();
    std::string recover();

    // Direct Subsystem Access
    TransactionManager& tm() { return tm_; }
    HeapFile& heap() { return *heap_; }
    BTreeIndex& index() { return index_; }
    WALManager& wal() { return *wal_; }
    BufferPoolManager& bpm() { return *bpm_; }
    ToastManager& toast() { return *toast_; }
    CLogManager& clog() { return *clog_; }

    bool is_in_transaction() const { return current_tx_.has_value(); }
    tx_id_t current_tx_id() const { return current_tx_.value_or(0); }

private:
    std::string db_prefix_;
    std::unique_ptr<CLogManager> clog_;
    TransactionManager tm_;
    std::unique_ptr<HeapFile> heap_;
    std::unique_ptr<WALManager> wal_;
    std::unique_ptr<BufferPoolManager> bpm_;
    std::unique_ptr<ToastManager> toast_;
    BTreeIndex index_; // Secondary B-Tree index on items(item_id)

    std::optional<tx_id_t>  current_tx_;
    std::optional<Snapshot> current_snapshot_;

    void ensure_transaction(bool is_read_only = false);
    std::string format_table(const std::vector<std::pair<CTID, HeapTuple>>& tuples, const std::string& scan_method);
};

} // namespace pg
