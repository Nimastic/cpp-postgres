# Pre-Build Study: Milestone 26 - Dynamic Catalogs & Multi-Table DDL (`pg_class`, `pg_attribute`, `CREATE TABLE`, `DROP TABLE`)

**Date**: 2026-09-05  
**Author**: Antigravity  
**Target Milestone**: Milestone 26 (Dynamic System Catalogs, Multi-Relation Storage Mapping, DDL Commands)  
**Status**: Pre-Build Exploration & Architectural Blueprint

---

## 1. Context & Motivation

Up to this point, our storage engine has operated under a single-relation assumption: all SQL statements, sequential scans, and B-Tree indexes implicitly target a fixed table named `items` with files `<prefix>_heap.db` and `<prefix>_index.db`.

In PostgreSQL, relations are not hard-coded. PostgreSQL uses **System Catalogs** (stored as standard relational tables themselves!) to track all schema objects:
- `pg_class` (`src/include/catalog/pg_class.h`): Tracks every relation, its type (`relkind`), file node identifier (`relfilenode`), page count, and tuple count.
- `pg_attribute` (`src/include/catalog/pg_attribute.h`): Tracks every column in every table, data types, lengths, and nullability.
- `Catalog Cache (syscache)`: In-memory cache of relation descriptors (`RelationData`) mapping table names to physical storage files and schema descriptors.

---

## 2. PostgreSQL Architectural Reference

In PostgreSQL:
- `Oid` (Object Identifier): 32-bit unsigned integer assigning unique identity to tables, attributes, and types.
- Relational mapping:
  A table with name `foo` and Oid `16384` is stored physically at `base/<database_oid>/<relfilenode>`.
- In our engine:
  We map tables dynamically to `<db_prefix>_<relname>_heap.db` (or `<db_prefix>_<relfilenode>_heap.db`) and manage their associated `HeapFile`, `DiskBTree`, and `FSM`.
- DDL Operations:
  - `CREATE TABLE <name> (<col1> <type>, ...);`:
    Allocates new relation ID, inserts entry into catalog, creates initial empty heap file and B-Tree index, registers relation in engine's relation cache.
  - `DROP TABLE <name>;`:
    Removes catalog entries, drops relation from relation cache, closes handles, and deletes underlying physical files.
  - Multi-table queries:
    `SELECT * FROM <name>` dynamically resolves `<name>` from catalog, scans the appropriate relation, and projects its schema columns.

---

## 3. Detailed Design for Milestone 26

### 3.1 Catalog Data Structures
```cpp
struct ColumnDef {
    std::string name;
    std::string type; // "INT", "TEXT", etc.
    int32_t len{4};
    int16_t num{1};
};

struct TableMetadata {
    uint32_t rel_id{0};
    std::string relname;
    std::vector<ColumnDef> columns;
    std::string heap_path;
    std::string index_path;
};

// Relation Descriptor held in memory
struct Relation {
    TableMetadata meta;
    std::unique_ptr<HeapFile> heap;
    std::unique_ptr<DiskBTree> index;
};
```

### 3.2 Catalog Manager (`CatalogManager`)
- Persisted in `<prefix>_catalog.db` or managed via dynamic `pg_class` / `pg_attribute` records.
- Provides:
  - `create_table(name, columns)`
  - `drop_table(name)`
  - `get_table(name) -> TableMetadata*`
  - `list_tables() -> std::vector<TableMetadata>`

### 3.3 Engine Integration
- `Engine` maintains `std::unordered_map<std::string, std::shared_ptr<Relation>> relations_`.
- On startup, loads catalog and opens relations. Default `items` table is automatically bootstrapped if not present, ensuring 100% backwards compatibility with all existing tests.
- Extends SQL parser for:
  - `CREATE TABLE <name> (col1 type, col2 type);`
  - `DROP TABLE <name>;`
  - `SELECT * FROM <name>;`
  - `INSERT INTO <name> VALUES (...);`

---

## 4. Verification & Testing Strategy

We will build `tests/test_catalog.cpp`:
1. DDL: Create tables `users`, `orders`, `products`.
2. Schema inspection: Verify columns, types, and file nodes.
3. Multi-table DML: Concurrently insert and query data across different tables without cross-talk.
4. Drops: Drop a table, verify catalog removal and physical file cleanup.
5. Persistence & Recovery: Reopen engine from disk, verify tables and schema persist intact.
