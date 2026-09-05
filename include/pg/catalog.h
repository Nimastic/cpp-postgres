#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <memory>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace pg {

// Column Definition (matches PostgreSQL pg_attribute catalog)
struct ColumnDef {
    std::string name;
    std::string type{"INT"};
    int32_t len{4};
    int16_t num{1};
};

// Table Metadata (matches PostgreSQL pg_class catalog)
struct TableMetadata {
    uint32_t rel_id{0};
    std::string relname;
    std::vector<ColumnDef> columns;
    std::string heap_path;
    std::string index_path;

    const ColumnDef* get_column(const std::string& col_name) const {
        for (const auto& col : columns) {
            if (col.name == col_name) return &col;
        }
        return nullptr;
    }
};

// Dynamic System Catalog Manager
// Persists schema metadata in a human-readable and durable on-disk catalog format
class CatalogManager {
public:
    explicit CatalogManager(std::string catalog_path);
    ~CatalogManager() = default;

    // Load catalog from disk
    bool load();

    // Persist catalog to disk
    bool save() const;

    // Schema DDL operations
    bool create_table(const std::string& relname, const std::vector<ColumnDef>& columns,
                      const std::string& heap_path, const std::string& index_path);

    bool drop_table(const std::string& relname);

    // Schema inspection
    bool has_table(const std::string& relname) const;
    const TableMetadata* get_table(const std::string& relname) const;
    std::vector<TableMetadata> all_tables() const;

    uint32_t next_rel_id() const { return next_rel_id_; }

private:
    std::string catalog_path_;
    uint32_t next_rel_id_{16384}; // PostgreSQL first user object Oid
    std::unordered_map<std::string, TableMetadata> tables_;
};

} // namespace pg
