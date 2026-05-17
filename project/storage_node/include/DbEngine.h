#ifndef DB_ENGINE_H
#define DB_ENGINE_H

#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>

#include "../../common/include/AST.h"
#include "../../common/include/Json.h"
#include "../../common/include/Parser.h"

#include "StorageNode.h"
using Row = std::vector<Value>;

namespace fs = std::filesystem;

struct TableMetadata {
    std::string name;
    std::vector<ColumnDef> columns;
    uint64_t row_count = 0;
};

class Table {
private:
    TableMetadata metadata_;
    std::unique_ptr<PageManager> page_manager_;
    PageBasedIndex<uint64_t> storage_;
    
public:
    Table(const std::string& db_path, const TableMetadata& metadata) 
        : metadata_(metadata),
          page_manager_(std::make_unique<PageManager>(db_path + "/" + metadata.name + ".tbl")),
          storage_(*page_manager_) {
        saveMetadata();
    }

    const TableMetadata& metadata() const { 
        return metadata_; 
    }

    void insertRow(const Row& row) {
        uint64_t row_id = metadata_.row_count++;
        std::string serialized = serializeRow(row);
        storage_.insert_string(row_id, serialized);
        saveMetadata();
    }

    std::vector<Row> selectRows(const Expr* condition = nullptr) {
        std::vector<Row> result;
        
        for (uint64_t i = 0; i < metadata_.row_count; ++i) {
            auto rowData = storage_.find_string(i);
            if (!rowData.has_value()) {
                continue;
            }
            
            Row row = deserializeRow(*rowData);
            if (!condition || evaluateCondition(condition, row)) {
                result.push_back(row);
            }
        }
        
        return result;
    }

    size_t deleteRows(const Expr* condition = nullptr) {
        size_t deleted = 0;
        
        for (uint64_t i = 0; i < metadata_.row_count; ++i) {
            auto rowData = storage_.find_string(i);
            if (!rowData.has_value()) {
                continue;
            }
            
            Row row = deserializeRow(*rowData);
            if (!condition || evaluateCondition(condition, row)) {
                storage_.remove(i);
                ++deleted;
            }
        }
        
        return deleted;
    }

    size_t updateRows(const std::vector<std::pair<std::string, Value>>& assignments,
                      const Expr* condition = nullptr) {
        size_t updated = 0;
        
        for (uint64_t i = 0; i < metadata_.row_count; ++i) {
            auto rowData = storage_.find_string(i);
            if (!rowData.has_value()) {
                continue;
            }
            
            Row row = deserializeRow(*rowData);
            if (!condition || evaluateCondition(condition, row)) {
                for (const auto& [column, value] : assignments) {
                    int idx = getColumnIndex(column);
                    if (idx >= 0) {
                        row[idx] = value;
                    }
                }
                
                std::string serialized = serializeRow(row);
                storage_.insert_string(i, serialized);
                ++updated;
            }
        }
        
        return updated;
    }

    void saveMetadata() {
        std::string meta_path = page_manager_->getFilePath() + ".meta";
        std::ofstream meta_file(meta_path, std::ios::binary);
        if (!meta_file.is_open()) {
            std::cerr << "Failed to save metadata for table: " << metadata_.name << std::endl;
            return;
        }
        
        // Save table name
        size_t name_len = metadata_.name.size();
        meta_file.write(reinterpret_cast<const char*>(&name_len), sizeof(name_len));
        meta_file.write(metadata_.name.c_str(), name_len);
        
        // Save columns
        size_t col_count = metadata_.columns.size();
        meta_file.write(reinterpret_cast<const char*>(&col_count), sizeof(col_count));
        for (const auto& col : metadata_.columns) {
            // Save column name
            size_t col_name_len = col.name.size();
            meta_file.write(reinterpret_cast<const char*>(&col_name_len), sizeof(col_name_len));
            meta_file.write(col.name.c_str(), col_name_len);
            
            // Save column type
            size_t type_len = col.type.size();
            meta_file.write(reinterpret_cast<const char*>(&type_len), sizeof(type_len));
            meta_file.write(col.type.c_str(), type_len);
        }
        
        // Save row count
        meta_file.write(reinterpret_cast<const char*>(&metadata_.row_count), sizeof(metadata_.row_count));
        
        meta_file.close();
    }

    static std::optional<TableMetadata> loadMetadata(const std::string& meta_path) {
        std::ifstream meta_file(meta_path, std::ios::binary);
        if (!meta_file.is_open()) {
            return std::nullopt;
        }
        
        TableMetadata metadata;
        
        // Load table name
        size_t name_len;
        meta_file.read(reinterpret_cast<char*>(&name_len), sizeof(name_len));
        metadata.name.resize(name_len);
        meta_file.read(&metadata.name[0], name_len);
        
        // Load columns
        size_t col_count;
        meta_file.read(reinterpret_cast<char*>(&col_count), sizeof(col_count));
        for (size_t i = 0; i < col_count; ++i) {
            ColumnDef col;
            
            // Load column name
            size_t col_name_len;
            meta_file.read(reinterpret_cast<char*>(&col_name_len), sizeof(col_name_len));
            col.name.resize(col_name_len);
            meta_file.read(&col.name[0], col_name_len);
            
            // Load column type
            size_t type_len;
            meta_file.read(reinterpret_cast<char*>(&type_len), sizeof(type_len));
            col.type.resize(type_len);
            meta_file.read(&col.type[0], type_len);
            
            metadata.columns.push_back(col);
        }
        
        // Load row count
        meta_file.read(reinterpret_cast<char*>(&metadata.row_count), sizeof(metadata.row_count));
        
        meta_file.close();
        return metadata;
    }

private:
    int getColumnIndex(const std::string& column) const {
        for (size_t i = 0; i < metadata_.columns.size(); ++i) {
            if (metadata_.columns[i].name == column) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    std::string serializeValue(const Value& value) {
        if (std::holds_alternative<int>(value)) {
            return "INT:" + std::to_string(std::get<int>(value));
        }
        if (std::holds_alternative<std::string>(value)) {
            return "STR:" + std::get<std::string>(value);
        }
        if (std::holds_alternative<std::nullptr_t>(value)) {
            return "NULL";
        }
        return "UNKNOWN";
    }

    Value deserializeValue(const std::string& str) {
        if (str.starts_with("INT:")) {
            return std::stoi(str.substr(4));
        }
        if (str.starts_with("STR:")) {
            return str.substr(4);
        }
        return nullptr;
    }

    std::string serializeRow(const Row& row) {
        std::string result;
        for (size_t i = 0; i < row.size(); ++i) {
            result += serializeValue(row[i]);
            if (i + 1 < row.size()) {
                result += "|";
            }
        }
        return result;
    }

    Row deserializeRow(const std::string& data) {
        Row row;
        std::string current;
        
        for (char c : data) {
            if (c == '|') {
                row.push_back(deserializeValue(current));
                current.clear();
            } else {
                current += c;
            }
        }
        
        if (!current.empty()) {
            row.push_back(deserializeValue(current));
        }
        
        return row;
    }

    Value resolveValue(const Value& val, const Row& row) {
        if (std::holds_alternative<ColumnRef>(val)) {
            ColumnRef ref = std::get<ColumnRef>(val);
            int idx = getColumnIndex(ref.column);
            if (idx >= 0 && idx < static_cast<int>(row.size())) {
                return row[idx];
            }
            return nullptr;
        }
        return val;
    }

    bool compareValues(const Value& left, const Value& right, ComparisonOp op) {
        if (std::holds_alternative<int>(left) && std::holds_alternative<int>(right)) {
            int l = std::get<int>(left);
            int r = std::get<int>(right);
            
            switch (op) {
                case ComparisonOp::EQUAL: return l == r;
                case ComparisonOp::NOT_EQUAL: return l != r;
                case ComparisonOp::LESS: return l < r;
                case ComparisonOp::GREATER: return l > r;
                case ComparisonOp::LESS_OR_EQUAL: return l <= r;
                case ComparisonOp::GREATER_OR_EQUAL: return l >= r;
            }
        }
        
        if (std::holds_alternative<std::string>(left) && std::holds_alternative<std::string>(right)) {
            std::string l = std::get<std::string>(left);
            std::string r = std::get<std::string>(right);
            
            switch (op) {
                case ComparisonOp::EQUAL: return l == r;
                case ComparisonOp::NOT_EQUAL: return l != r;
                case ComparisonOp::LESS: return l < r;
                case ComparisonOp::GREATER: return l > r;
                case ComparisonOp::LESS_OR_EQUAL: return l <= r;
                case ComparisonOp::GREATER_OR_EQUAL: return l >= r;
            }
        }
        
        return false;
    }

    bool evaluateCondition(const Expr* expr, const Row& row) {
        if (!expr) return true;
        
        switch (expr->type) {
            case Expr::COMPARISON: {
                Value left = resolveValue(expr->comparison.left, row);
                Value right = resolveValue(expr->comparison.right, row);
                return compareValues(left, right, expr->comparison.op);
            }
            
            case Expr::AND:
                return evaluateCondition(expr->left.get(), row) &&
                       evaluateCondition(expr->right.get(), row);
            
            case Expr::OR:
                return evaluateCondition(expr->left.get(), row) ||
                       evaluateCondition(expr->right.get(), row);
            
            case Expr::NOT:
                return !evaluateCondition(expr->left.get(), row);
            
            case Expr::BETWEEN: {
                Value val = resolveValue(expr->between.val, row);
                Value start = resolveValue(expr->between.start, row);
                Value end = resolveValue(expr->between.end, row);
                
                if (std::holds_alternative<int>(val) && 
                    std::holds_alternative<int>(start) && 
                    std::holds_alternative<int>(end)) {
                    int v = std::get<int>(val);
                    int s = std::get<int>(start);
                    int e = std::get<int>(end);
                    return v >= s && v <= e;
                }
                return false;
            }
            
            case Expr::LIKE: {
                Value val = resolveValue(expr->like.val, row);
                Value pattern = resolveValue(expr->like.pattern, row);
                
                if (std::holds_alternative<std::string>(val) && 
                    std::holds_alternative<std::string>(pattern)) {
                    std::string v = std::get<std::string>(val);
                    std::string p = std::get<std::string>(pattern);
                    
                    if (p.ends_with("%")) {
                        p.pop_back();
                        return v.starts_with(p);
                    }
                    return v == p;
                }
                return false;
            }
        }
        
        return false;
    }
};

// ==================== Database Implementation ====================

class Database {
public:
    Database(const std::string& root_path, const std::string& name)
        : root_path_(root_path), name_(name) {
        db_path_ = root_path_ + "/" + name_;
        
        if (!fs::exists(db_path_)) {
            fs::create_directories(db_path_);
        } else {
            loadTables();
        }
    }
    
    ~Database() {
        flush();
    }

    const std::string& name() const {
        return name_;
    }

    bool createTable(const CreateTableStmt& stmt) {
        if (tables_.contains(stmt.table.name)) {
            return false;
        }

        TableMetadata meta;
        meta.name = stmt.table.name;
        meta.columns = stmt.columns;

        auto table = std::make_shared<Table>(db_path_, meta);
        tables_[meta.name] = table;
        return true;
    }

    bool dropTable(const std::string& table_name) {
        auto it = tables_.find(table_name);
        if (it == tables_.end()) {
            return false;
        }

        tables_.erase(it);
        
        std::string file = db_path_ + "/" + table_name + ".tbl";
        std::string meta_file = file + ".meta";
        
        if (fs::exists(file)) {
            fs::remove(file);
        }
        if (fs::exists(meta_file)) {
            fs::remove(meta_file);
        }
        
        return true;
    }

    std::shared_ptr<Table> getTable(const std::string& name) {
        auto it = tables_.find(name);
        if (it == tables_.end()) {
            return nullptr;
        }
        return it->second;
    }
    
    void flush() {
        for (auto& [name, table] : tables_) {
            table->saveMetadata();
        }
    }

private:
    void loadTables() {
        for (const auto& entry : fs::directory_iterator(db_path_)) {
            if (entry.path().extension() == ".meta") {
                auto metadata_opt = Table::loadMetadata(entry.path().string());
                if (metadata_opt) {
                    auto table = std::make_shared<Table>(db_path_, *metadata_opt);
                    tables_[metadata_opt->name] = table;
                    std::cout << "Loaded table: " << metadata_opt->name << std::endl;
                }
            }
        }
    }

    std::string root_path_;
    std::string name_;
    std::string db_path_;
    std::unordered_map<std::string, std::shared_ptr<Table>> tables_;
};

// ==================== DBMS Implementation ====================

class DBMS {
public:
    explicit DBMS(const std::string& root_dir)
        : root_dir_(root_dir) {
        if (!fs::exists(root_dir_)) {
            fs::create_directories(root_dir_);
        } else {
            loadDatabases();
        }
    }
    
    ~DBMS() {
        flushAll();
    }

    bool createDatabase(const std::string& name) {
        if (databases_.contains(name)) {
            return false;
        }
        databases_[name] = std::make_shared<Database>(root_dir_, name);
        return true;
    }

    bool dropDatabase(const std::string& name) {
        auto it = databases_.find(name);
        if (it == databases_.end()) {
            return false;
        }

        databases_.erase(it);
        
        std::string path = root_dir_ + "/" + name;
        if (fs::exists(path)) {
            fs::remove_all(path);
        }
        
        return true;
    }

    bool useDatabase(const std::string& name) {
        auto it = databases_.find(name);
        if (it == databases_.end()) {
            return false;
        }
        current_db_ = it->second;
        return true;
    }

    std::shared_ptr<Database> currentDatabase() {
        return current_db_;
    }
    
    void flushAll() {
        for (auto& [name, db] : databases_) {
            db->flush();
        }
    }

private:
    void loadDatabases() {
        for (const auto& entry : fs::directory_iterator(root_dir_)) {
            if (entry.is_directory()) {
                std::string db_name = entry.path().filename().string();
                databases_[db_name] = std::make_shared<Database>(root_dir_, db_name);
                std::cout << "Loaded database: " << db_name << std::endl;
            }
        }
    }

    std::string root_dir_;
    std::unordered_map<std::string, std::shared_ptr<Database>> databases_;
    std::shared_ptr<Database> current_db_;
};

// ==================== SQL Executor ====================

class SQLExecutor {
public:
    explicit SQLExecutor(DBMS& dbms) : dbms_(dbms) {}

    void execute(const Statement& stmt) {
        std::visit([this](auto&& arg) {
            executeStatement(arg);
        }, stmt);
    }

private:
    DBMS& dbms_;

    void executeStatement(const CreateDatabaseStmt& stmt) {
        bool ok = dbms_.createDatabase(stmt.name);
        std::cout << (ok ? "Database created\n" : "Failed to create database\n");
    }

    void executeStatement(const DropDatabaseStmt& stmt) {
        bool ok = dbms_.dropDatabase(stmt.name);
        std::cout << (ok ? "Database dropped\n" : "Failed to drop database\n");
    }

    void executeStatement(const UseStmt& stmt) {
        bool ok = dbms_.useDatabase(stmt.name);
        std::cout << (ok ? "Using database\n" : "Database not found\n");
    }

    void executeStatement(const CreateTableStmt& stmt) {
        auto db = dbms_.currentDatabase();
        if (!db) {
            std::cout << "No database selected\n";
            return;
        }
        
        bool ok = db->createTable(stmt);
        std::cout << (ok ? "Table created\n" : "Failed to create table\n");
    }

    void executeStatement(const DropTableStmt& stmt) {
        auto db = dbms_.currentDatabase();
        if (!db) {
            std::cout << "No database selected\n";
            return;
        }
        
        bool ok = db->dropTable(stmt.table.name);
        std::cout << (ok ? "Table dropped\n" : "Failed to drop table\n");
    }

    void executeStatement(const InsertStmt& stmt) {
        auto db = dbms_.currentDatabase();
        if (!db) {
            std::cout << "No database selected\n";
            return;
        }
        
        auto table = db->getTable(stmt.table.name);
        if (!table) {
            std::cout << "Table not found\n";
            return;
        }
        
        for (const auto& row : stmt.values) {
            table->insertRow(row);
        }
        
        std::cout << "Inserted " << stmt.values.size() << " rows\n";
    }

    void executeStatement(const SelectStmt& stmt) {
        auto db = dbms_.currentDatabase();
        if (!db) {
            std::cout << "No database selected\n";
            return;
        }
        
        auto table = db->getTable(stmt.table.name);
        if (!table) {
            std::cout << "Table not found\n";
            return;
        }
        
        auto rows = table->selectRows(stmt.condition.get());
        
        // Print results
        for (const auto& row : rows) {
            for (size_t i = 0; i < row.size(); ++i) {
                if (i > 0) std::cout << " | ";
                
                if (std::holds_alternative<int>(row[i])) {
                    std::cout << std::get<int>(row[i]);
                } else if (std::holds_alternative<std::string>(row[i])) {
                    std::cout << std::get<std::string>(row[i]);
                } else {
                    std::cout << "NULL";
                }
            }
            std::cout << std::endl;
        }
        
        std::cout << rows.size() << " rows returned\n";
    }

    void executeStatement(const UpdateStmt& stmt) {
        auto db = dbms_.currentDatabase();
        if (!db) {
            std::cout << "No database selected\n";
            return;
        }
        
        auto table = db->getTable(stmt.table.name);
        if (!table) {
            std::cout << "Table not found\n";
            return;
        }
        
        size_t updated = table->updateRows(stmt.assignments, stmt.condition.get());
        std::cout << "Updated " << updated << " rows\n";
    }

    void executeStatement(const DeleteStmt& stmt) {
        auto db = dbms_.currentDatabase();
        if (!db) {
            std::cout << "No database selected\n";
            return;
        }
        
        auto table = db->getTable(stmt.table.name);
        if (!table) {
            std::cout << "Table not found\n";
            return;
        }
        
        size_t deleted = table->deleteRows(stmt.condition.get());
        std::cout << "Deleted " << deleted << " rows\n";
    }
};

#endif // DB_ENGINE_H