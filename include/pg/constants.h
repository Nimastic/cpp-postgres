#pragma once

#include <cstdint>
#include <cstddef>
#include <limits>

namespace pg {

constexpr size_t PAGE_SIZE = 8192; // 8KB PostgreSQL standard page size
using page_id_t = uint32_t;
constexpr page_id_t INVALID_PAGE_ID = std::numeric_limits<page_id_t>::max();

using slot_id_t = uint16_t; // 1-based line pointer index on page
constexpr slot_id_t INVALID_SLOT_ID = 0;

} // namespace pg
