#pragma once

#include "pg/constants.h"
#include "pg/pager.h"
#include "pg/page.h"
#include "pg/tuple.h"
#include "pg/tx.h"
#include "pg/mvcc.h"
#include "pg/heap.h"
#include "pg/vacuum.h"
#include "pg/index.h"
#include "pg/btree.h"
#include "pg/disk_btree.h"
#include "pg/buffer_pool.h"
#include "pg/wal.h"
#include "pg/toast.h"
#include "pg/clog.h"
#include "pg/control.h"
#include "pg/session.h"
#include "pg/executor.h"
#include "pg/catalog.h"
#include <string>
#include <memory>
#include <optional>
#include <vector>
#include <unordered_map>

namespace pg {

// Unified PostgreSQL Database Engine
// Integrates all 10 core subsystems into a single cohesive interface
class Engine {
public:
    explicit Engine(const std::string& db_prefix = "pg_data");
    ~Engine();

    // True when the database came up after an unclean shutdown and replayed the log.
    bool recovered_at_startup() const { return recovered_at_startup_; }

    // Execute a statement on behalf of one session. Each connection owns its
    // transaction and snapshot, so two clients no longer share a transaction.
    std::string execute(const std::string& sql, Session& session);

    // Convenience overload using the engine's built-in session, for the CLI and
    // for tests that only ever have one client.
    std::string execute(const std::string& sql);

    // Hand out a fresh session for a new connection.
    Session new_session();

    Session& default_session() { return default_session_; }
    LockManager& locks() { return lock_mgr_; }

    // Transaction Control
    std::string begin_transaction();
    std::string commit_transaction();
    std::string rollback_transaction();

    // DML
    std::string insert_item(int32_t item_id, int32_t price);
    std::string insert_item_with_doc(int32_t item_id, int32_t price, const std::string& doc);
    std::string update_item(int32_t item_id, int32_t new_price);
    std::string select_all();
    std::string select_all(size_t limit, size_t offset);
    std::string select_by_id(int32_t item_id);
    std::string select_filtered(std::function<bool(const TupleTableSlot&)> pred, const std::string& desc, size_t limit = 0, size_t offset = 0);
    std::string select_doc_by_id(int32_t item_id);


    // Maintenance & Diagnostics
    std::string vacuum();
    std::string dump_page(page_id_t page_id);
    std::string status();
    std::string recover();
    std::string checkpoint();

    // DDL & System Catalog
    CatalogManager& catalog() { return *catalog_; }
    std::string create_table(const std::string& name, const std::vector<ColumnDef>& columns);
    std::string drop_table(const std::string& name);
    std::string show_tables();
    std::string describe_table(const std::string& name);

    // Direct Subsystem Access
    TransactionManager& tm() { return tm_; }
    HeapFile& heap() { return *heap_; }
    DiskBTree& index() { return *index_; }
    WALManager& wal() { return *wal_; }
    // The relation owns the pool; the engine just exposes it. There is
    // deliberately no second pool over the same file.
    BufferPoolManager& bpm() { return *heap_->bpm(); }
    ToastManager& toast() { return *toast_; }
    CLogManager& clog() { return *clog_; }

    // Reflect the session the last statement ran for.
    bool is_in_transaction() const { return sess_->current_tx.has_value(); }
    tx_id_t current_tx_id() const { return sess_->current_tx.value_or(0); }

private:
    std::string db_prefix_;
    std::unique_ptr<ControlFile> control_;
    std::unique_ptr<CLogManager> clog_;
    TransactionManager tm_;
    std::unique_ptr<HeapFile> heap_;
    std::unique_ptr<WALManager> wal_;
    std::unique_ptr<ToastManager> toast_;
    std::unique_ptr<DiskBTree> index_; // On-disk B-Tree index on items(item_id)
    std::unique_ptr<CatalogManager> catalog_;

    struct DynamicRelation {
        std::unique_ptr<HeapFile> heap;
        std::unique_ptr<DiskBTree> index;
    };
    std::unordered_map<std::string, DynamicRelation> dynamic_relations_;
    DynamicRelation* get_or_open_relation(const std::string& name);

    Session  default_session_;
    Session* sess_{&default_session_};   // The session the current statement runs for
    LockManager lock_mgr_;
    uint64_t next_session_id_{1};
    bool recovered_at_startup_{false};

    void ensure_transaction(bool is_read_only = false);
    std::string format_table(const std::vector<std::pair<CTID, HeapTuple>>& tuples, const std::string& scan_method);
    std::string format_join_table(const std::vector<TupleTableSlot>& slots, const std::string& scan_method);
};

} // namespace pg

