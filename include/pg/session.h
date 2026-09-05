#pragma once

#include "pg/tuple.h"
#include "pg/tx.h"
#include "pg/lock.h"
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <string>

namespace pg {

// Per-connection state.
//
// Transaction state belongs to a session, not to the engine. When the engine
// holds a single current transaction, two clients share it: one client's BEGIN
// puts the other inside its transaction and one client's COMMIT commits the
// other's writes. It also means no two snapshots ever coexist, so the MVCC
// visibility rules never meet the concurrency they exist for.
struct Session {
    uint64_t                id{0};
    std::optional<tx_id_t>  current_tx;
    std::optional<Snapshot> snapshot;

    // A transaction block that hit an error rejects everything until ROLLBACK.
    // This is the 'E' state the wire protocol reports and psql shows as '!'.
    bool failed{false};

    bool in_transaction() const { return current_tx.has_value(); }

    void clear_transaction() {
        current_tx.reset();
        snapshot.reset();
        failed = false;
    }
};

} // namespace pg


