#include "pg/mvcc.h"

namespace pg {

bool is_tuple_visible(const TupleHeader& header, const Snapshot& snapshot, const TransactionManager& tm) {
    tx_id_t xmin = header.xmin;
    tx_id_t xmax = header.xmax;

    // =========================================================================
    // PHASE 1: Evaluate XMIN (Creation Visibility)
    // =========================================================================
    if (xmin == snapshot.current_tx_id && snapshot.current_tx_id != 0) {
        // Created by current transaction itself -> Proceed to check xmax
    } else {
        TransactionStatus xmin_status = tm.get_status(xmin);

        if (xmin_status == TransactionStatus::ABORTED) {
            return false; // Inserter transaction aborted -> Invisible
        }

        if (xmin_status == TransactionStatus::IN_PROGRESS) {
            return false; // Inserter transaction is still uncommitted -> Invisible
        }

        // Inserter is COMMITTED -> Check snapshot horizon
        if (xmin >= snapshot.xmax) {
            return false; // Inserted in the future after this snapshot was taken -> Invisible
        }

        if (snapshot.is_active(xmin)) {
            return false; // Inserter was in-progress when snapshot was taken -> Invisible
        }
    }

    // =========================================================================
    // PHASE 2: Evaluate XMAX (Deletion / Update Visibility)
    // =========================================================================
    if (xmax == 0 || xmax == INVALID_TX_ID) {
        // Tuple has never been deleted or updated -> Visible
        return true;
    }

    if (xmax == snapshot.current_tx_id && snapshot.current_tx_id != 0) {
        // Deleted/Updated by current transaction itself -> Invisible
        return false;
    }

    TransactionStatus xmax_status = tm.get_status(xmax);

    if (xmax_status == TransactionStatus::ABORTED) {
        // Deleter transaction aborted -> Deletion rolled back -> Visible
        return true;
    }

    if (xmax_status == TransactionStatus::IN_PROGRESS) {
        // Deleter transaction is still running (uncommitted) -> Visible to us
        return true;
    }

    // Deleter is COMMITTED -> Check snapshot horizon
    if (xmax >= snapshot.xmax) {
        // Deleted in the future after this snapshot was taken -> Visible in this snapshot
        return true;
    }

    if (snapshot.is_active(xmax)) {
        // Deleter was still active when this snapshot started -> Visible in this snapshot
        return true;
    }

    // Deleter committed BEFORE our snapshot was taken -> Invisible
    return false;
}

} // namespace pg
