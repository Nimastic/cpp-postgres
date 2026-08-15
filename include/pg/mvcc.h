#pragma once

#include "pg/tx.h"
#include "pg/tuple.h"

namespace pg {

// Core PostgreSQL MVCC tuple visibility evaluation
// Implements HeapTupleSatisfiesMVCC logic
bool is_tuple_visible(const TupleHeader& header, const Snapshot& snapshot, const TransactionManager& tm);

} // namespace pg
