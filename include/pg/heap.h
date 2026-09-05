#pragma once

#include "pg/pager.h"
#include "pg/page.h"
#include "pg/tuple.h"
#include "pg/tx.h"
#include "pg/mvcc.h"
#include "pg/buffer_pool.h"
#include <memory>
#include <vector>
#include <optional>
#include <string>

namespace pg {

// Forward declare to avoid circular dependency
class BufferPoolManager;
class WALManager;

class HeapFile {
public:
    explicit HeapFile(std::unique_ptr<Pager> pager, BufferPoolManager* bpm = nullptr);
    ~HeapFile() = default;

    // Disable copy
    HeapFile(const HeapFile&) = delete;
    HeapFile& operator=(const HeapFile&) = delete;

    // Move support
    HeapFile(HeapFile&&) noexcept = default;
    HeapFile& operator=(HeapFile&&) noexcept = default;

    // Factory method
    static std::unique_ptr<HeapFile> open(const std::string& filepath, BufferPoolManager* bpm = nullptr);

    // Insert a new ItemRecord into the heap. Returns the assigned CTID.
    // If all existing pages are full, a new 8KB page is automatically allocated.
    CTID insert(const ItemRecord& record, tx_id_t xmin = 0);

    // Non-in-place MVCC update: stamps xmax on old tuple and inserts new tuple with xmin
    CTID update(const CTID& old_ctid, const ItemRecord& new_record, tx_id_t tx_id);

    // HOT (Heap-Only Tuple) update: places new version on SAME page as old version.
    // Prerequisites: (1) indexed columns did not change, (2) same page has free space.
    // Returns the new CTID on the same page, or std::nullopt if HOT is not possible.
    // On success, the old tuple gets HEAP_HOT_UPDATED and the new tuple gets HEAP_ONLY_TUPLE.
    // NO index writes are needed!
    std::optional<CTID> hot_update(const CTID& old_ctid, const ItemRecord& new_record, tx_id_t tx_id);

    // MVCC delete: stamps xmax on target tuple
    bool delete_tuple(const CTID& target_ctid, tx_id_t tx_id);

    // Fetch a single tuple by its physical CTID (page_id, slot_id)
    std::optional<HeapTuple> get(const CTID& ctid);

    // Walk a HOT chain from the CTID an index entry points at and return the
    // first version visible to the snapshot, with the CTID it was found at.
    //
    // This is PostgreSQL's heap_hot_search_buffer. Two things make it more than
    // a simple fetch. The root line pointer may be an LP_REDIRECT, left behind
    // when pruning removed the root tuple's storage but had to keep the slot
    // alive for the index entry that points at it; the redirect names the next
    // slot rather than a byte offset. And a live version further down the chain
    // is a heap-only tuple that no index references, so it is reachable only by
    // following t_ctid from the root.
    std::optional<std::pair<CTID, HeapTuple>> hot_search(
        const CTID& root, const Snapshot& snapshot, const TransactionManager& tm);

    // Update the tuple header at a specific CTID (used for setting xmax / t_ctid)
    bool update_tuple_header(const CTID& ctid, const TupleHeader& new_header);

    // Physical Sequential Scan: Returns all physical tuples in heap (unfiltered)
    std::vector<std::pair<CTID, HeapTuple>> seq_scan();

    // MVCC Snapshot Sequential Scan: Returns only tuples visible to the given snapshot
    std::vector<std::pair<CTID, HeapTuple>> seq_scan(const Snapshot& snapshot, const TransactionManager& tm);

    // Pager inspection
    Pager& pager() { return *pager_; }
    const Pager& pager() const { return *pager_; }
    size_t num_pages() const { return pager_->num_pages(); }

    // Buffer pool access. A relation always has exactly one pool: there is no
    // mode in which pages are reached around it, because two paths to the same
    // page means the two copies can disagree and the later flush silently wins.
    BufferPoolManager* bpm() { return bpm_; }

    // Adopt an externally owned pool, replacing the one this relation made for
    // itself. Still exactly one pool per relation.
    void set_bpm(BufferPoolManager* bpm);

    // Write-ahead logging. The heap owns the log calls rather than the caller,
    // because the record and the page change have to happen together, under the
    // same pin -- exactly as PostgreSQL emits the record inside heap_insert()
    // while still holding the buffer's exclusive content lock.
    void set_wal(WALManager* wal) { wal_ = wal; }
    WALManager* wal() const { return wal_; }

private:
    std::unique_ptr<Pager> pager_;
    std::unique_ptr<BufferPoolManager> owned_bpm_; // Used unless a pool is injected
    BufferPoolManager* bpm_{nullptr};              // Always non-null after construction
    WALManager* wal_{nullptr};

    // Pin a page for read-modify-write. Requires a buffer pool.
    PinnedPage pin(page_id_t page_id);

    // Full-page image before the first change to a page after a checkpoint, so
    // recovery can heal a torn write rather than replaying deltas onto rubble.
    void maybe_log_fpi(Page& page, page_id_t page_id);
};

} // namespace pg
