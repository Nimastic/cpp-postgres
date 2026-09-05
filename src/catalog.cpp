#include "pg/catalog.h"
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;

namespace pg {

CatalogManager::CatalogManager(std::string catalog_path)
    : catalog_path_(std::move(catalog_path)) {
    load();
}

bool CatalogManager::load() {
    tables_.clear();
    if (!fs::exists(catalog_path_)) {
        return false;
    }

    std::ifstream file(catalog_path_);
    if (!file.is_open()) {
        return false;
    }

    std::string line;
    TableMetadata* current_table = nullptr;

    while (std::getline(file, line)) {
        // Trim line
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos || line[start] == '#') continue;
        line = line.substr(start);

        std::istringstream iss(line);
        std::string tag;
        iss >> tag;

        if (tag == "NEXT_REL_ID") {
            iss >> next_rel_id_;
        } else if (tag == "TABLE") {
            uint32_t rel_id = 0;
            std::string relname, heap_path, index_path;
            iss >> rel_id >> relname >> heap_path >> index_path;
            TableMetadata meta;
            meta.rel_id = rel_id;
            meta.relname = relname;
            meta.heap_path = heap_path;
            meta.index_path = index_path;
            tables_[relname] = meta;
            current_table = &tables_[relname];
        } else if (tag == "COLUMN" && current_table != nullptr) {
            ColumnDef col;
            iss >> col.name >> col.type >> col.len >> col.num;
            current_table->columns.push_back(col);
        }
    }

    return true;
}

bool CatalogManager::save() const {
    std::ofstream file(catalog_path_);
    if (!file.is_open()) {
        return false;
    }

    file << "# PostgreSQL System Catalog Metadata\n";
    file << "NEXT_REL_ID " << next_rel_id_ << "\n\n";

    for (const auto& [name, meta] : tables_) {
        file << "TABLE " << meta.rel_id << " " << meta.relname << " "
             << meta.heap_path << " " << meta.index_path << "\n";
        for (const auto& col : meta.columns) {
            file << "  COLUMN " << col.name << " " << col.type << " "
                 << col.len << " " << col.num << "\n";
        }
        file << "\n";
    }

    return true;
}

bool CatalogManager::create_table(const std::string& relname,
                                  const std::vector<ColumnDef>& columns,
                                  const std::string& heap_path,
                                  const std::string& index_path) {
    if (tables_.find(relname) != tables_.end()) {
        return false; // Table already exists
    }

    TableMetadata meta;
    meta.rel_id = next_rel_id_++;
    meta.relname = relname;
    meta.columns = columns;
    meta.heap_path = heap_path;
    meta.index_path = index_path;

    tables_[relname] = meta;
    save();
    return true;
}

bool CatalogManager::drop_table(const std::string& relname) {
    auto it = tables_.find(relname);
    if (it == tables_.end()) {
        return false;
    }

    tables_.erase(it);
    save();
    return true;
}

bool CatalogManager::has_table(const std::string& relname) const {
    return tables_.find(relname) != tables_.end();
}

const TableMetadata* CatalogManager::get_table(const std::string& relname) const {
    auto it = tables_.find(relname);
    if (it != tables_.end()) {
        return &it->second;
    }
    return nullptr;
}

std::vector<TableMetadata> CatalogManager::all_tables() const {
    std::vector<TableMetadata> result;
    for (const auto& [name, meta] : tables_) {
        result.push_back(meta);
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.rel_id < b.rel_id;
    });
    return result;
}

} // namespace pg
