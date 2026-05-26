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
#include <chrono>
#include <functional>

#include "../../common/include/AST.h"
#include "../../common/include/Parser.h"
#include "Temporal.h"
#include "StringPool.h"
#include "types.h"
#include "StorageNode.h"

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
    std::unordered_map<size_t, std::unique_ptr<PageBasedIndex<std::string>>> unique_indexes_;
    std::unique_ptr<TemporalManager> temporal_;

public:
    static DBValue astToDBValue(const Value& v) {
        if (std::holds_alternative<std::nullptr_t>(v))
            return DBValue::null_of(DataType::INT);
        if (std::holds_alternative<int>(v))
            return DBValue::of_int(std::get<int>(v));
        if (std::holds_alternative<std::string>(v))
            return DBValue::of_str(std::get<std::string>(v));
        return DBValue::null_of(DataType::INT);
    }

    Table(const std::string& db_path, const TableMetadata& metadata)
        : metadata_(metadata),
          page_manager_(std::make_unique<PageManager>(db_path + "/" + metadata.name + ".tbl")),
          storage_(*page_manager_),
          temporal_(std::make_unique<TemporalManager>(db_path + "/" + metadata.name + ".log")) {
        initIndexedColumns();
        rebuildIndexesFromStorage();
        saveMetadata();
    }

    const TableMetadata& metadata() const { return metadata_; }

    enum class InsertError {
        OK, DUPLICATE_KEY, NOT_NULL_VIOLATION, TYPE_MISMATCH, UNKNOWN
    };

    std::pair<bool, InsertError> insertRow(const std::vector<DBValue>& input_row) {
        auto row_opt = normalizeRow(input_row);
        if (!row_opt) return {false, InsertError::UNKNOWN};
        const auto& row = *row_opt;
        if (!validateRowTypesAndConstraints(row)) {
            for (size_t i = 0; i < row.size(); ++i) {
                if (metadata_.columns[i].constraint == ColumnConstraint::NOT_NULL && row[i].is_null)
                    return {false, InsertError::NOT_NULL_VIOLATION};
                if (!isValueCompatibleWithColumn(row[i], metadata_.columns[i]))
                    return {false, InsertError::TYPE_MISMATCH};
            }
            return {false, InsertError::UNKNOWN};
        }
        if (!checkUniqueConstraints(row, std::nullopt))
            return {false, InsertError::DUPLICATE_KEY};

        uint64_t row_id = metadata_.row_count++;
        std::string serialized = serializeRow(row);
        storage_.insert_string(row_id, serialized);
        insertIntoUniqueIndexes(row, row_id);
        temporal_->logInsert(row_id, serialized);
        saveMetadata();
        return {true, InsertError::OK};
    }

    std::vector<std::vector<DBValue>> selectRows(const Expr* condition = nullptr) {
        std::vector<std::vector<DBValue>> result;
        for (uint64_t i = 0; i < metadata_.row_count; ++i) {
            auto rowData = storage_.find_string(i);
            if (!rowData) continue;
            auto row = deserializeRow(*rowData);
            if (!condition || evaluateCondition(condition, row))
                result.push_back(std::move(row));
        }
        return result;
    }

    size_t deleteRows(const Expr* condition = nullptr) {
        size_t deleted = 0;
        for (uint64_t i = 0; i < metadata_.row_count; ++i) {
            auto rowData = storage_.find_string(i);
            if (!rowData) continue;
            auto row = deserializeRow(*rowData);
            if (!condition || evaluateCondition(condition, row)) {
                temporal_->logDelete(i, *rowData);
                removeFromUniqueIndexes(row);
                storage_.remove(i);
                ++deleted;
            }
        }
        return deleted;
    }

    size_t updateRows(const std::vector<std::pair<std::string, DBValue>>& assignments,
                      const Expr* condition = nullptr) {
        size_t updated = 0;
        for (uint64_t i = 0; i < metadata_.row_count; ++i) {
            auto oldData = storage_.find_string(i);
            if (!oldData) continue;
            auto row = deserializeRow(*oldData);
            if (condition && !evaluateCondition(condition, row)) continue;

            auto new_row = row;
            bool assignment_failed = false;
            for (const auto& [col, val] : assignments) {
                int idx = getColumnIndex(col);
                if (idx < 0) continue;
                if (!isValueCompatibleWithColumn(val, metadata_.columns[idx])) {
                    assignment_failed = true;
                    break;
                }
                new_row[idx] = val;
            }
            if (assignment_failed) continue;
            if (!validateRowTypesAndConstraints(new_row)) continue;
            if (!checkUniqueConstraints(new_row, i)) continue;

            std::string newSerialized = serializeRow(new_row);
            temporal_->logUpdate(i, *oldData, newSerialized);
            removeFromUniqueIndexes(row);
            storage_.insert_string(i, newSerialized);
            insertIntoUniqueIndexes(new_row, i);
            ++updated;
        }
        return updated;
    }

    bool revertTo(const std::chrono::system_clock::time_point& tp) {
        return temporal_->revertTo(tp, [this](const LogRecord& undo) -> bool {
            switch (undo.type) {
                case LogOpType::INSERT: {
                    auto row = deserializeRow(undo.new_data);
                    uint64_t rid = undo.row_id;
                    storage_.insert_string(rid, serializeRow(row));
                    insertIntoUniqueIndexes(row, rid);
                    if (rid >= metadata_.row_count) metadata_.row_count = rid + 1;
                    break;
                }
                case LogOpType::DELETE:
                    storage_.remove(undo.row_id);
                    break;
                case LogOpType::UPDATE:
                    storage_.insert_string(undo.row_id, undo.new_data);
                    break;
            }
            saveMetadata();
            return true;
        });
    }

    void saveMetadata() {
        std::string meta_path = page_manager_->getFilePath() + ".meta";
        std::ofstream meta_file(meta_path, std::ios::binary);
        if (!meta_file.is_open()) {
            std::cerr << "Failed to save metadata for table: " << metadata_.name << std::endl;
            return;
        }
        auto writePod = [&](const auto& v) { meta_file.write(reinterpret_cast<const char*>(&v), sizeof(v)); };
        auto writeString = [&](const std::string& s) {
            size_t len = s.size();
            writePod(len);
            meta_file.write(s.data(), len);
        };
        /*
		auto writeDBValue = [&](const DBValue& v) {
            uint8_t tag = 0;
            if (v.is_null) tag = 0;
            else if (v.type == DataType::INT) tag = 1;
            else if (v.type == DataType::STRING) tag = 2;
            writePod(tag);
            if (tag == 1) writePod(v.ival);
            else if (tag == 2) writeString(v.getString());
        };
		*/

        writeString(metadata_.name);
        size_t col_count = metadata_.columns.size();
        writePod(col_count);
        for (const auto& col : metadata_.columns) {
            writeString(col.name);
            writeString(col.type);
            uint8_t constraint = static_cast<uint8_t>(col.constraint);
            writePod(constraint);
            // DEFAULT values temporarily disabled to avoid type conversion issues
            bool has_default = false;
            writePod(has_default);
            // if (has_default) writeDBValue(astToDBValue(*col.default_value));
        }
        writePod(metadata_.row_count);
        meta_file.close();
    }

    static std::optional<TableMetadata> loadMetadata(const std::string& meta_path) {
        std::ifstream meta_file(meta_path, std::ios::binary);
        if (!meta_file.is_open()) return std::nullopt;
        TableMetadata metadata;
        auto readPod = [&](auto& v) { meta_file.read(reinterpret_cast<char*>(&v), sizeof(v)); };
        auto readString = [&]() -> std::string {
            size_t len = 0;
            readPod(len);
            std::string s(len, '\0');
            if (len > 0) meta_file.read(s.data(), len);
            return s;
        };
        metadata.name = readString();
        size_t col_count = 0;
        readPod(col_count);
        for (size_t i = 0; i < col_count; ++i) {
            ColumnDef col;
            col.name = readString();
            col.type = readString();
            uint8_t constraint = 0;
            readPod(constraint);
            col.constraint = static_cast<ColumnConstraint>(constraint);
            bool has_default = false;
            readPod(has_default);
            if (has_default) {
                // Skip default value reading for now
                uint8_t tag; readPod(tag);
                if (tag == 1) { int32_t dummy; readPod(dummy); }
                else if (tag == 2) { size_t len; readPod(len); meta_file.seekg(len, std::ios::cur); }
            }
            metadata.columns.push_back(std::move(col));
        }
        readPod(metadata.row_count);
        meta_file.close();
        return metadata;
    }

private:
    void initIndexedColumns() {
        for (size_t i = 0; i < metadata_.columns.size(); ++i) {
            if (metadata_.columns[i].constraint == ColumnConstraint::INDEXED) {
                unique_indexes_[i] = std::make_unique<PageBasedIndex<std::string>>(*page_manager_);
            }
        }
    }

    void rebuildIndexesFromStorage() {
        for (auto& [idx, idx_ptr] : unique_indexes_) idx_ptr->clear();
        for (uint64_t rid = 0; rid < metadata_.row_count; ++rid) {
            auto data = storage_.find_string(rid);
            if (!data) continue;
            auto row = deserializeRow(*data);
            for (const auto& [col_idx, idx_ptr] : unique_indexes_) {
                if (col_idx >= row.size()) continue;
                const DBValue& v = row[col_idx];
                if (v.is_null) continue;
                std::string key = indexKey(v);
                if (idx_ptr->contains(key))
                    throw std::runtime_error("Duplicate value rebuilding INDEXED in " + metadata_.name);
                idx_ptr->insert_string(key, std::to_string(rid));
            }
        }
    }

    std::optional<std::vector<DBValue>> normalizeRow(const std::vector<DBValue>& input_row) const {
        if (input_row.size() > metadata_.columns.size()) return std::nullopt;
        auto row = input_row;
        row.resize(metadata_.columns.size(), DBValue::null_of(DataType::INT));
        // DEFAULT values temporarily disabled
        // for (size_t i = 0; i < metadata_.columns.size(); ++i) {
        //     if (i >= input_row.size() || row[i].is_null) {
        //         if (metadata_.columns[i].default_value.has_value()) {
        //             row[i] = astToDBValue(*metadata_.columns[i].default_value);
        //         }
        //     }
        // }
        return row;
    }

    bool isNullable(const ColumnDef& col) const {
        return !(col.constraint == ColumnConstraint::NOT_NULL || col.constraint == ColumnConstraint::INDEXED);
    }

    bool isIntType(const std::string& t) const {
        std::string u = upper(t);
        return u == "INT" || u == "INTEGER";
    }
    bool isStringType(const std::string& t) const {
        std::string u = upper(t);
        return u == "STRING" || u == "TEXT" || u == "VARCHAR";
    }
    std::string upper(std::string s) const {
        for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }

    bool isValueCompatibleWithColumn(const DBValue& v, const ColumnDef& col) const {
        if (v.is_null) return isNullable(col);
        if (isIntType(col.type)) return v.type == DataType::INT;
        if (isStringType(col.type)) return v.type == DataType::STRING;
        return !v.is_null;
    }

    bool validateRowTypesAndConstraints(const std::vector<DBValue>& row) const {
        if (row.size() != metadata_.columns.size()) return false;
        for (size_t i = 0; i < row.size(); ++i) {
            if (!isValueCompatibleWithColumn(row[i], metadata_.columns[i])) return false;
            if (metadata_.columns[i].constraint == ColumnConstraint::INDEXED && row[i].is_null) return false;
        }
        return true;
    }

    std::string indexKey(const DBValue& v) const {
        if (v.type == DataType::INT) return "INT:" + std::to_string(v.ival);
        if (v.type == DataType::STRING) return "STR:" + v.getString();
        throw std::runtime_error("INDEXED column cannot be NULL");
    }

    bool checkUniqueConstraints(const std::vector<DBValue>& row, std::optional<uint64_t> current_rid) const {
        for (const auto& [col_idx, idx_ptr] : unique_indexes_) {
            if (col_idx >= row.size()) continue;
            const DBValue& v = row[col_idx];
            if (v.is_null) return false;
            std::string key = indexKey(v);
            auto existing = idx_ptr->find_string(key);
            if (!existing) continue;
            uint64_t existing_rid = std::stoull(*existing);
            if (!current_rid || existing_rid != *current_rid) return false;
        }
        return true;
    }

    void insertIntoUniqueIndexes(const std::vector<DBValue>& row, uint64_t rid) {
        for (const auto& [col_idx, idx_ptr] : unique_indexes_) {
            if (col_idx >= row.size()) continue;
            const DBValue& v = row[col_idx];
            if (v.is_null) continue;
            idx_ptr->insert_string(indexKey(v), std::to_string(rid));
        }
    }

    void removeFromUniqueIndexes(const std::vector<DBValue>& row) {
        for (const auto& [col_idx, idx_ptr] : unique_indexes_) {
            if (col_idx >= row.size()) continue;
            const DBValue& v = row[col_idx];
            if (v.is_null) continue;
            idx_ptr->remove(indexKey(v));
        }
    }

    int getColumnIndex(const std::string& name) const {
        for (size_t i = 0; i < metadata_.columns.size(); ++i)
            if (metadata_.columns[i].name == name) return static_cast<int>(i);
        return -1;
    }

    std::string serializeValue(const DBValue& v) const {
        if (v.is_null) return "NULL";
        if (v.type == DataType::INT) return "INT:" + std::to_string(v.ival);
        return "STR:" + v.getString();
    }

    DBValue deserializeValue(const std::string& s) const {
        if (s == "NULL") return DBValue::null_of(DataType::INT);
        if (s.starts_with("INT:")) return DBValue::of_int(std::stoi(s.substr(4)));
        if (s.starts_with("STR:")) return DBValue::of_str(s.substr(4));
        return DBValue::null_of(DataType::INT);
    }

    std::string serializeRow(const std::vector<DBValue>& row) const {
        std::string result;
        for (size_t i = 0; i < row.size(); ++i) {
            result += serializeValue(row[i]);
            if (i+1 < row.size()) result += "|";
        }
        return result;
    }

    std::vector<DBValue> deserializeRow(const std::string& data) const {
        std::vector<DBValue> row;
        std::string cur;
        for (char c : data) {
            if (c == '|') {
                row.push_back(deserializeValue(cur));
                cur.clear();
            } else cur += c;
        }
        if (!cur.empty()) row.push_back(deserializeValue(cur));
        return row;
    }

    bool compareValues(const DBValue& left, const DBValue& right, ComparisonOp op) const {
        if (left.is_null || right.is_null) return false;
        if (left.type != right.type) return false;
        if (left.type == DataType::INT) {
            int l = left.ival, r = right.ival;
            switch (op) {
                case ComparisonOp::EQUAL: return l == r;
                case ComparisonOp::NOT_EQUAL: return l != r;
                case ComparisonOp::LESS: return l < r;
                case ComparisonOp::GREATER: return l > r;
                case ComparisonOp::LESS_OR_EQUAL: return l <= r;
                case ComparisonOp::GREATER_OR_EQUAL: return l >= r;
            }
        } else {
            const std::string& l = left.getString();
            const std::string& r = right.getString();
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

    bool evaluateCondition(const Expr* expr, const std::vector<DBValue>& row) const {
        if (!expr) return true;
        switch (expr->type) {
            case Expr::COMPARISON: {
                DBValue left = astToDBValue(expr->comparison.left);
                DBValue right = astToDBValue(expr->comparison.right);
                return compareValues(left, right, expr->comparison.op);
            }
            case Expr::AND:
                return evaluateCondition(expr->left.get(), row) && evaluateCondition(expr->right.get(), row);
            case Expr::OR:
                return evaluateCondition(expr->left.get(), row) || evaluateCondition(expr->right.get(), row);
            case Expr::NOT:
                return !evaluateCondition(expr->left.get(), row);
            case Expr::BETWEEN: {
                DBValue val = astToDBValue(expr->between.val);
                DBValue start = astToDBValue(expr->between.start);
                DBValue end = astToDBValue(expr->between.end);
                if (val.type == DataType::INT && start.type == DataType::INT && end.type == DataType::INT)
                    return val.ival >= start.ival && val.ival <= end.ival;
                return false;
            }
            case Expr::LIKE: {
                DBValue val = astToDBValue(expr->like.val);
                DBValue pat = astToDBValue(expr->like.pattern);
                if (val.type == DataType::STRING && pat.type == DataType::STRING) {
                    const std::string& v = val.getString();
                    std::string p = pat.getString();
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

// ==================== Database ====================

class Database {
public:
    Database(const std::string& root_path, const std::string& name)
        : root_path_(root_path), name_(name) {
        db_path_ = root_path_ + "/" + name_;
        if (!fs::exists(db_path_)) {
            fs::create_directories(db_path_);
            metadata_.created_at = getCurrentTimestamp();
            metadata_.updated_at = metadata_.created_at;
            saveDatabaseMetadata();
        } else {
            if (!loadDatabaseMetadata()) {
                metadata_.created_at = getCurrentTimestamp();
                metadata_.updated_at = metadata_.created_at;
                saveDatabaseMetadata();
                loadTables();
            }
        }
    }
    ~Database() { flush(); }

    const std::string& name() const { return name_; }

    bool createTable(const CreateTableStmt& stmt) {
        if (tables_.contains(stmt.table.name)) return false;
        TableMetadata meta;
        meta.name = stmt.table.name;
        for (const auto& colDef : stmt.columns) {
            ColumnDef col;
            col.name = colDef.name;
            col.type = colDef.type;
            col.constraint = colDef.constraint;
            // default values not supported yet
            meta.columns.push_back(col);
        }
        auto table = std::make_shared<Table>(db_path_, meta);
        tables_[meta.name] = table;
        metadata_.updated_at = getCurrentTimestamp();
        saveDatabaseMetadata();
        return true;
    }

    bool dropTable(const std::string& table_name) {
        auto it = tables_.find(table_name);
        if (it == tables_.end()) return false;
        tables_.erase(it);
        std::string file = db_path_ + "/" + table_name + ".tbl";
        std::string meta_file = file + ".meta";
        std::string log_file = db_path_ + "/" + table_name + ".log";
        if (fs::exists(file)) fs::remove(file);
        if (fs::exists(meta_file)) fs::remove(meta_file);
        if (fs::exists(log_file)) fs::remove(log_file);
        metadata_.updated_at = getCurrentTimestamp();
        saveDatabaseMetadata();
        return true;
    }

    std::shared_ptr<Table> getTable(const std::string& name) {
        auto it = tables_.find(name);
        return it == tables_.end() ? nullptr : it->second;
    }

    const std::string& getCreatedAt() const { return metadata_.created_at; }
    const std::string& getUpdatedAt() const { return metadata_.updated_at; }

    void flush() {
        for (auto& [name, table] : tables_) table->saveMetadata();
        metadata_.updated_at = getCurrentTimestamp();
        saveDatabaseMetadata();
    }

private:
    struct DatabaseMetadata {
        std::string version = "1.0";
        std::string created_at;
        std::string updated_at;
    };

    std::string getCurrentTimestamp() {
        auto now = std::time(nullptr);
        std::string timestamp = std::ctime(&now);
        if (!timestamp.empty() && timestamp.back() == '\n') timestamp.pop_back();
        return timestamp;
    }

    bool saveDatabaseMetadata() {
        std::string meta_path = db_path_ + "/database.meta";
        std::ofstream meta_file(meta_path, std::ios::binary);
        if (!meta_file.is_open()) return false;
        auto writePod = [&](const auto& v) { meta_file.write(reinterpret_cast<const char*>(&v), sizeof(v)); };
        auto writeString = [&](const std::string& s) {
            size_t len = s.size();
            writePod(len);
            if (len > 0) meta_file.write(s.data(), len);
        };
        const char signature[4] = {'D', 'B', 'M', 'T'};
        meta_file.write(signature, 4);
        uint32_t format_version = 1;
        writePod(format_version);
        writeString(metadata_.version);
        writeString(name_);
        writeString(metadata_.created_at);
        writeString(metadata_.updated_at);
        size_t table_count = tables_.size();
        writePod(table_count);
        for (const auto& [table_name, table] : tables_) {
            writeString(table_name);
            writePod(table->metadata().row_count);
            writePod(table->metadata().columns.size());
            for (const auto& col : table->metadata().columns) {
                writeString(col.name);
                writeString(col.type);
                uint8_t constraint = static_cast<uint8_t>(col.constraint);
                writePod(constraint);
            }
        }
        meta_file.close();
        return true;
    }

    bool loadDatabaseMetadata() {
        std::string meta_path = db_path_ + "/database.meta";
        std::ifstream meta_file(meta_path, std::ios::binary);
        if (!meta_file.is_open()) return false;
        auto readPod = [&](auto& v) { meta_file.read(reinterpret_cast<char*>(&v), sizeof(v)); };
        auto readString = [&]() -> std::string {
            size_t len = 0;
            readPod(len);
            std::string s(len, '\0');
            if (len > 0) meta_file.read(s.data(), len);
            return s;
        };
        char signature[4];
        meta_file.read(signature, 4);
        if (signature[0] != 'D' || signature[1] != 'B' || signature[2] != 'M' || signature[3] != 'T') return false;
        uint32_t format_version = 0;
        readPod(format_version);
        if (format_version != 1) return false;
        metadata_.version = readString();
        std::string stored_name = readString();
        if (stored_name != name_) std::cerr << "Warning: Database name mismatch in metadata" << std::endl;
        metadata_.created_at = readString();
        metadata_.updated_at = readString();
        size_t table_count = 0;
        readPod(table_count);
        for (size_t i = 0; i < table_count; ++i) {
            readString(); // table_name
            uint64_t dummy_row_count = 0;
            readPod(dummy_row_count);
            size_t dummy_col_count = 0;
            readPod(dummy_col_count);
            for (size_t j = 0; j < dummy_col_count; ++j) {
                readString(); // col_name
                readString(); // col_type
                uint8_t dummy_constraint = 0;
                readPod(dummy_constraint);
            }
        }
        meta_file.close();
        loadTables();
        return true;
    }

    void loadTables() {
        for (const auto& entry : fs::directory_iterator(db_path_)) {
            if (entry.path().extension() == ".meta" && entry.path().filename() != "database.meta") {
                auto metadata_opt = Table::loadMetadata(entry.path().string());
                if (metadata_opt) {
                    auto table = std::make_shared<Table>(db_path_, *metadata_opt);
                    tables_[metadata_opt->name] = table;
                    std::cout << "Loaded table: " << metadata_opt->name
                              << " (" << metadata_opt->row_count << " rows)" << std::endl;
                }
            }
        }
    }

    std::string root_path_;
    std::string name_;
    std::string db_path_;
    std::unordered_map<std::string, std::shared_ptr<Table>> tables_;
    DatabaseMetadata metadata_;
};

// ==================== DBMS ====================

class DBMS {
public:
    explicit DBMS(const std::string& root_dir)
        : root_dir_(root_dir) {
        if (!fs::exists(root_dir_)) {
            fs::create_directories(root_dir_);
            metadata_.created_at = getCurrentTimestamp();
            metadata_.updated_at = metadata_.created_at;
            saveDBMSMetadata();
        } else {
            if (!loadDBMSMetadata()) {
                metadata_.created_at = getCurrentTimestamp();
                metadata_.updated_at = metadata_.created_at;
                saveDBMSMetadata();
                loadDatabases();
            }
        }
    }

    ~DBMS() { flushAll(); }

    bool createDatabase(const std::string& name) {
        if (databases_.contains(name)) return false;
        databases_[name] = std::make_shared<Database>(root_dir_, name);
        metadata_.updated_at = getCurrentTimestamp();
        saveDBMSMetadata();
        return true;
    }

    bool dropDatabase(const std::string& name) {
        auto it = databases_.find(name);
        if (it == databases_.end()) return false;
        if (current_db_ && current_db_->name() == name) current_db_.reset();
        databases_.erase(it);
        fs::remove_all(root_dir_ + "/" + name);
        metadata_.updated_at = getCurrentTimestamp();
        saveDBMSMetadata();
        return true;
    }

    bool useDatabase(const std::string& name) {
        auto it = databases_.find(name);
        if (it == databases_.end()) return false;
        current_db_ = it->second;
        if (current_db_) metadata_.current_database = current_db_->name();
        else metadata_.current_database.clear();
        metadata_.updated_at = getCurrentTimestamp();
        saveDBMSMetadata();
        return true;
    }

    std::shared_ptr<Database> getDatabase(const std::string& name) const {
        auto it = databases_.find(name);
        return it == databases_.end() ? nullptr : it->second;
    }

    std::shared_ptr<Database> currentDatabase() const { return current_db_; }
    const std::string& getRootDir() const { return root_dir_; }

    void flushAll() {
        for (auto& [name, db] : databases_) db->flush();
        metadata_.updated_at = getCurrentTimestamp();
        saveDBMSMetadata();
    }

private:
    struct DBMSMetadata {
        std::string version = "1.0";
        std::string created_at;
        std::string updated_at;
        std::string current_database;
    };

    std::string getCurrentTimestamp() {
        auto now = std::time(nullptr);
        std::string timestamp = std::ctime(&now);
        if (!timestamp.empty() && timestamp.back() == '\n') timestamp.pop_back();
        return timestamp;
    }

    bool saveDBMSMetadata() {
        std::string meta_path = root_dir_ + "/dbms.meta";
        std::ofstream meta_file(meta_path, std::ios::binary);
        if (!meta_file.is_open()) return false;
        auto writePod = [&](const auto& v) { meta_file.write(reinterpret_cast<const char*>(&v), sizeof(v)); };
        auto writeString = [&](const std::string& s) {
            size_t len = s.size();
            writePod(len);
            if (len > 0) meta_file.write(s.data(), len);
        };
        const char signature[4] = {'D', 'B', 'M', 'S'};
        meta_file.write(signature, 4);
        uint32_t format_version = 1;
        writePod(format_version);
        writeString(metadata_.version);
        writeString(metadata_.created_at);
        writeString(metadata_.updated_at);
        writeString(root_dir_);
        std::string current_db = current_db_ ? current_db_->name() : "";
        writeString(current_db);
        size_t db_count = databases_.size();
        writePod(db_count);
        for (const auto& [db_name, db] : databases_) {
            writeString(db_name);
            writeString(db->getCreatedAt());
            writeString(db->getUpdatedAt());
        }
        meta_file.close();
        return true;
    }

    bool loadDBMSMetadata() {
        std::string meta_path = root_dir_ + "/dbms.meta";
        std::ifstream meta_file(meta_path, std::ios::binary);
        if (!meta_file.is_open()) return false;
        auto readPod = [&](auto& v) { meta_file.read(reinterpret_cast<char*>(&v), sizeof(v)); };
        auto readString = [&]() -> std::string {
            size_t len = 0;
            readPod(len);
            std::string s(len, '\0');
            if (len > 0) meta_file.read(s.data(), len);
            return s;
        };
        char signature[4];
        meta_file.read(signature, 4);
        if (signature[0] != 'D' || signature[1] != 'B' || signature[2] != 'M' || signature[3] != 'S') return false;
        uint32_t format_version = 0;
        readPod(format_version);
        if (format_version != 1) return false;
        metadata_.version = readString();
        metadata_.created_at = readString();
        metadata_.updated_at = readString();
        std::string stored_root = readString();
        if (stored_root != root_dir_) std::cerr << "Warning: Root directory mismatch in metadata" << std::endl;
        metadata_.current_database = readString();
        size_t db_count = 0;
        readPod(db_count);
        for (size_t i = 0; i < db_count; ++i) {
            readString(); // db_name
            readString(); // created_at
            readString(); // updated_at
        }
        meta_file.close();
        loadDatabases();
        if (!metadata_.current_database.empty()) {
            auto it = databases_.find(metadata_.current_database);
            if (it != databases_.end()) current_db_ = it->second;
        }
        return true;
    }

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
    DBMSMetadata metadata_;
};

// ==================== SQL Executor ====================

class SQLExecutor {
public:
    explicit SQLExecutor(DBMS& dbms) : dbms_(dbms) {}

    void execute(const Statement& stmt) {
        std::visit([this](auto&& arg) { executeStatement(arg); }, stmt);
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
        if (!db) { std::cout << "No database selected\n"; return; }
        bool ok = db->createTable(stmt);
        std::cout << (ok ? "Table created\n" : "Failed to create table\n");
    }
    void executeStatement(const DropTableStmt& stmt) {
        auto db = dbms_.currentDatabase();
        if (!db) { std::cout << "No database selected\n"; return; }
        bool ok = db->dropTable(stmt.table.name);
        std::cout << (ok ? "Table dropped\n" : "Failed to drop table\n");
    }

    void executeStatement(const InsertStmt& stmt) {
        auto db = dbms_.currentDatabase();
        if (!db) { std::cout << "Error: No database selected\n"; return; }
        auto table = db->getTable(stmt.table.name);
        if (!table) { std::cout << "Error: Table not found\n"; return; }
        size_t inserted = 0;
        for (size_t i = 0; i < stmt.values.size(); ++i) {
            std::vector<DBValue> dbRow;
            for (const auto& astVal : stmt.values[i]) {
                dbRow.push_back(Table::astToDBValue(astVal));
            }
            auto [ok, error] = table->insertRow(dbRow);
            if (!ok) {
                switch (error) {
                    case Table::InsertError::DUPLICATE_KEY:
                        std::cout << "Error: Duplicate key value at row " << (i+1) << "\n"; break;
                    case Table::InsertError::NOT_NULL_VIOLATION:
                        std::cout << "Error: NOT NULL constraint failed at row " << (i+1) << "\n"; break;
                    case Table::InsertError::TYPE_MISMATCH:
                        std::cout << "Error: Type mismatch at row " << (i+1) << "\n"; break;
                    default:
                        std::cout << "Error: Insert failed at row " << (i+1) << "\n";
                }
                return;
            }
			//StringPool::instance().dumpStats(); // DEBUG ДЕБАГ
            ++inserted;
        }
        std::cout << "Inserted " << inserted << " rows\n";
    }

    void executeStatement(const SelectStmt& stmt) {
        auto db = dbms_.currentDatabase();
        if (!db) { std::cout << "No database selected\n"; return; }
        auto table = db->getTable(stmt.table.name);
        if (!table) { std::cout << "Table not found\n"; return; }
        auto rows = table->selectRows(stmt.condition.get());
        for (const auto& row : rows) {
            for (size_t i = 0; i < row.size(); ++i) {
                if (i > 0) std::cout << " | ";
                std::cout << row[i].to_display();
            }
            std::cout << std::endl;
        }
        std::cout << rows.size() << " rows returned\n";
    }

    void executeStatement(const UpdateStmt& stmt) {
        auto db = dbms_.currentDatabase();
        if (!db) { std::cout << "No database selected\n"; return; }
        auto table = db->getTable(stmt.table.name);
        if (!table) { std::cout << "Table not found\n"; return; }
        std::vector<std::pair<std::string, DBValue>> dbAssignments;
        for (const auto& [col, val] : stmt.assignments) {
            dbAssignments.emplace_back(col, Table::astToDBValue(val));
        }
        size_t updated = table->updateRows(dbAssignments, stmt.condition.get());
        std::cout << "Updated " << updated << " rows\n";
    }

    void executeStatement(const DeleteStmt& stmt) {
        auto db = dbms_.currentDatabase();
        if (!db) { std::cout << "No database selected\n"; return; }
        auto table = db->getTable(stmt.table.name);
        if (!table) { std::cout << "Table not found\n"; return; }
        size_t deleted = table->deleteRows(stmt.condition.get());
        std::cout << "Deleted " << deleted << " rows\n";
    }

    void executeStatement(const RevertStmt& stmt) {
        std::shared_ptr<Database> db;
        if (stmt.table.database.empty())
            db = dbms_.currentDatabase();
        else
            db = dbms_.getDatabase(stmt.table.database);
        if (!db) {
            std::cout << "Database not found\n";
            return;
        }
        auto table = db->getTable(stmt.table.name);
        if (!table) {
            std::cout << "Table not found\n";
            return;
        }
        if (table->revertTo(stmt.timestamp))
            std::cout << "Table '" << stmt.table.name << "' reverted successfully\n";
        else
            std::cout << "Revert failed\n";
    }
};

#endif // DB_ENGINE_H