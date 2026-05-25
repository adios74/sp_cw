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
#include "../../common/include/Json.h"
#include "../../common/include/Parser.h"
#include "Temporal.h"
#include "StringPool.h"

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

    // Уникальные индексы для колонок с constraint == INDEXED
    std::unordered_map<size_t, std::unique_ptr<PageBasedIndex<std::string>>> unique_indexes_;

    // === ТЕМПОРАЛЬНАЯ ПЕРСИСТЕНТНОСТЬ ===
    std::unique_ptr<TemporalManager> temporal_;

public:
    Table(const std::string& db_path, const TableMetadata& metadata)
        : metadata_(metadata),
          page_manager_(std::make_unique<PageManager>(db_path + "/" + metadata.name + ".tbl")),
          storage_(*page_manager_),
          temporal_(std::make_unique<TemporalManager>(db_path + "/" + metadata.name + ".log"))   // <-- НОВОЕ
    {
        initIndexedColumns();
        rebuildIndexesFromStorage();
        saveMetadata();
    }

    const TableMetadata& metadata() const {
        return metadata_;
    }

    enum class InsertError {
        OK,
        DUPLICATE_KEY,
        NOT_NULL_VIOLATION,
        TYPE_MISMATCH,
        UNKNOWN
    };

    std::pair<bool, InsertError> insertRow(const Row& input_row) {
        auto row_opt = normalizeRow(input_row);
        if (!row_opt.has_value()) {
            return {false, InsertError::UNKNOWN};
        }

        const Row& row = *row_opt;
        if (!validateRowTypesAndConstraints(row)) {
            for (size_t i = 0; i < row.size(); ++i) {
                if (metadata_.columns[i].constraint == ColumnConstraint::NOT_NULL &&
                    std::holds_alternative<std::nullptr_t>(row[i])) {
                    return {false, InsertError::NOT_NULL_VIOLATION};
                }
                if (!isValueCompatibleWithColumn(row[i], metadata_.columns[i])) {
                    return {false, InsertError::TYPE_MISMATCH};
                }
            }
            return {false, InsertError::UNKNOWN};
        }

        if (!checkUniqueConstraints(row, std::nullopt)) {
            return {false, InsertError::DUPLICATE_KEY};
        }

        uint64_t row_id = metadata_.row_count++;
        std::string serialized = serializeRow(row);
        storage_.insert_string(row_id, serialized);
        insertIntoUniqueIndexes(row, row_id);

        // === ЛОГИРОВАНИЕ INSERT ===
        temporal_->logInsert(row_id, serialized);

        saveMetadata();
        return {true, InsertError::OK};
    }

    std::vector<Row> selectRows(const Expr* condition = nullptr) {
        std::vector<Row> result;
        for (uint64_t i = 0; i < metadata_.row_count; ++i) {
            auto rowData = storage_.find_string(i);
            if (!rowData.has_value()) continue;
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
            if (!rowData.has_value()) continue;
            Row row = deserializeRow(*rowData);
            if (!condition || evaluateCondition(condition, row)) {
                // === ЛОГИРОВАНИЕ DELETE ===
                temporal_->logDelete(i, *rowData);

                removeFromUniqueIndexes(row);
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
            auto oldData = storage_.find_string(i);
            if (!oldData.has_value()) continue;
            Row row = deserializeRow(*oldData);
            if (condition && !evaluateCondition(condition, row)) continue;

            Row new_row = row;
            bool assignment_failed = false;

            for (const auto& [column, value] : assignments) {
                int idx = getColumnIndex(column);
                if (idx < 0) continue;
                Value resolved = value;
                if (!isValueCompatibleWithColumn(resolved, metadata_.columns[idx])) {
                    assignment_failed = true;
                    break;
                }
                new_row[idx] = resolved;
            }
            if (assignment_failed) continue;
            if (!validateRowTypesAndConstraints(new_row)) continue;
            if (!checkUniqueConstraints(new_row, i)) continue;

            // === ЛОГИРОВАНИЕ UPDATE (до изменения) ===
            std::string newSerialized = serializeRow(new_row);
            temporal_->logUpdate(i, *oldData, newSerialized);

            removeFromUniqueIndexes(row);
            storage_.insert_string(i, newSerialized);
            insertIntoUniqueIndexes(new_row, i);
            ++updated;
        }
        return updated;
    }

    // === ОТКАТ ТАБЛИЦЫ К МОМЕНТУ ВРЕМЕНИ ===
    bool revertTo(const std::chrono::system_clock::time_point& tp) {
        return temporal_->revertTo(tp, [this](const LogRecord& undo) -> bool {
            switch (undo.type) {
                case LogOpType::INSERT: {
                    // Восстановить удалённую запись (обратная к DELETE)
                    Row row = deserializeRow(undo.new_data);
                    uint64_t rid = undo.row_id;
                    storage_.insert_string(rid, serializeRow(row));
                    insertIntoUniqueIndexes(row, rid);
                    if (rid >= metadata_.row_count)
                        metadata_.row_count = rid + 1;
                    break;
                }
                case LogOpType::DELETE: {
                    // Удалить запись (обратная к INSERT)
                    storage_.remove(undo.row_id);
                    // Удаление из индексов делается отдельно, но можно и здесь
                    // В данном случае индексы будут перестроены при следующей вставке,
                    // либо можно удалить явно. Для простоты оставим так.
                    break;
                }
                case LogOpType::UPDATE: {
                    // Вернуть старую версию
                    storage_.insert_string(undo.row_id, undo.new_data);
                    // Обновить индексы
                    // (можно перестроить индексы для этой строки, но для краткости пропущено)
                    break;
                }
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

        auto writePod = [&](const auto& v) {
            meta_file.write(reinterpret_cast<const char*>(&v), sizeof(v));
        };
        auto writeString = [&](const std::string& s) {
            size_t len = s.size();
            writePod(len);
            meta_file.write(s.data(), len);
        };
        auto writeValue = [&](const Value& v) {
            uint8_t tag = 255;
            if (std::holds_alternative<int>(v)) tag = 0;
            else if (std::holds_alternative<std::string>(v)) tag = 1;
            else if (std::holds_alternative<std::nullptr_t>(v)) tag = 2;
            else if (std::holds_alternative<ColumnRef>(v)) tag = 3;
            writePod(tag);
            switch (tag) {
                case 0: { int x = std::get<int>(v); writePod(x); break; }
                case 1: writeString(std::get<std::string>(v)); break;
                case 2: break;
                case 3: {
                    const auto& r = std::get<ColumnRef>(v);
                    writeString(r.database);
                    writeString(r.table);
                    writeString(r.column);
                    break;
                }
                default: break;
            }
        };

        writeString(metadata_.name);
        size_t col_count = metadata_.columns.size();
        writePod(col_count);
        for (const auto& col : metadata_.columns) {
            writeString(col.name);
            writeString(col.type);
            uint8_t constraint = static_cast<uint8_t>(col.constraint);
            writePod(constraint);
            bool has_default = col.default_value.has_value();
            writePod(has_default);
            if (has_default) writeValue(*col.default_value);
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
        auto readValue = [&]() -> Value {
            uint8_t tag = 255;
            readPod(tag);
            switch (tag) {
                case 0: { int x = 0; readPod(x); return x; }
                case 1: return readString();
                case 2: return nullptr;
                case 3: { ColumnRef r; r.database = readString(); r.table = readString(); r.column = readString(); return r; }
                default: return nullptr;
            }
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
            if (has_default) col.default_value = readValue();
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
        for (auto& [idx, index] : unique_indexes_) index->clear();
        for (uint64_t row_id = 0; row_id < metadata_.row_count; ++row_id) {
            auto rowData = storage_.find_string(row_id);
            if (!rowData.has_value()) continue;
            Row row = deserializeRow(*rowData);
            for (const auto& [col_idx, index] : unique_indexes_) {
                if (col_idx >= row.size()) continue;
                const Value& v = row[col_idx];
                if (std::holds_alternative<std::nullptr_t>(v)) continue;
                std::string key = indexKey(v);
                if (index->contains(key))
                    throw std::runtime_error("Duplicate value found while rebuilding INDEXED constraint in table: " + metadata_.name);
                index->insert_string(key, std::to_string(row_id));
            }
        }
    }

    std::optional<Row> normalizeRow(const Row& input_row) const {
        if (input_row.size() > metadata_.columns.size()) return std::nullopt;
        Row row = input_row;
        row.resize(metadata_.columns.size(), nullptr);
        for (size_t i = 0; i < metadata_.columns.size(); ++i) {
            if (i >= input_row.size() || std::holds_alternative<std::nullptr_t>(row[i])) {
                if (metadata_.columns[i].default_value.has_value()) {
                    row[i] = *metadata_.columns[i].default_value;
                }
            }
        }
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

    bool isValueCompatibleWithColumn(const Value& v, const ColumnDef& col) const {
        if (std::holds_alternative<std::nullptr_t>(v)) return isNullable(col);
        if (isIntType(col.type)) return std::holds_alternative<int>(v);
        if (isStringType(col.type)) return std::holds_alternative<std::string>(v);
        return !std::holds_alternative<std::nullptr_t>(v);
    }

    bool validateRowTypesAndConstraints(const Row& row) const {
        if (row.size() != metadata_.columns.size()) return false;
        for (size_t i = 0; i < row.size(); ++i) {
            if (!isValueCompatibleWithColumn(row[i], metadata_.columns[i])) return false;
            if (metadata_.columns[i].constraint == ColumnConstraint::INDEXED &&
                std::holds_alternative<std::nullptr_t>(row[i])) return false;
        }
        return true;
    }

    std::string indexKey(const Value& v) const {
        if (std::holds_alternative<int>(v)) return "INT:" + std::to_string(std::get<int>(v));
        if (std::holds_alternative<std::string>(v)) return "STR:" + std::get<std::string>(v);
        throw std::runtime_error("INDEXED columns cannot be NULL or non-scalar");
    }

    bool checkUniqueConstraints(const Row& row, std::optional<uint64_t> current_row_id) const {
        for (const auto& [col_idx, index] : unique_indexes_) {
            if (col_idx >= row.size()) continue;
            const Value& v = row[col_idx];
            if (std::holds_alternative<std::nullptr_t>(v)) return false;
            std::string key = indexKey(v);
            auto existing = index->find_string(key);
            if (!existing.has_value()) continue;
            uint64_t existing_row_id = std::stoull(*existing);
            if (!current_row_id.has_value() || existing_row_id != *current_row_id) return false;
        }
        return true;
    }

    void insertIntoUniqueIndexes(const Row& row, uint64_t row_id) {
        for (const auto& [col_idx, index] : unique_indexes_) {
            if (col_idx >= row.size()) continue;
            const Value& v = row[col_idx];
            std::string key = indexKey(v);
            index->insert_string(key, std::to_string(row_id));
        }
    }

    void removeFromUniqueIndexes(const Row& row) {
        for (const auto& [col_idx, index] : unique_indexes_) {
            if (col_idx >= row.size()) continue;
            const Value& v = row[col_idx];
            if (std::holds_alternative<std::nullptr_t>(v)) continue;
            std::string key = indexKey(v);
            index->remove(key);
        }
    }

    int getColumnIndex(const std::string& column) const {
        for (size_t i = 0; i < metadata_.columns.size(); ++i)
            if (metadata_.columns[i].name == column) return static_cast<int>(i);
        return -1;
    }

    std::string serializeValue(const Value& value) {
    if (value.is_null) return "NULL";
    if (value.type == DataType::INT) {
        return "INT:" + std::to_string(value.ival);
    } else {
        return "STR:" + value.getString();  // value.getString() возвращает const std::string&
    }
}

Value deserializeValue(const std::string& str) {
    if (str == "NULL") return Value::null_of(DataType::INT);
    if (str.starts_with("INT:")) {
        return Value::of_int(std::stoi(str.substr(4)));
    }
    if (str.starts_with("STR:")) {
        // Интернируем строку при загрузке
        std::string content = str.substr(4);
        return Value::of_str(content);  // of_str уже интернирует
    }
    return Value::null_of(DataType::INT);
}

    std::string serializeRow(const Row& row) {
        std::string result;
        for (size_t i = 0; i < row.size(); ++i) {
            result += serializeValue(row[i]);
            if (i + 1 < row.size()) result += "|";
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
            } else current += c;
        }
        if (!current.empty()) row.push_back(deserializeValue(current));
        return row;
    }

    Value resolveValue(const Value& val, const Row& row) {
        if (std::holds_alternative<ColumnRef>(val)) {
            ColumnRef ref = std::get<ColumnRef>(val);
            int idx = getColumnIndex(ref.column);
            if (idx >= 0 && idx < static_cast<int>(row.size())) return row[idx];
            return nullptr;
        }
        return val;
    }

    bool compareValues(const Value& left, const Value& right, ComparisonOp op) {
        if (std::holds_alternative<int>(left) && std::holds_alternative<int>(right)) {
            int l = std::get<int>(left), r = std::get<int>(right);
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
            const std::string& l = std::get<std::string>(left);
            const std::string& r = std::get<std::string>(right);
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
            case Expr::AND: return evaluateCondition(expr->left.get(), row) && evaluateCondition(expr->right.get(), row);
            case Expr::OR:  return evaluateCondition(expr->left.get(), row) || evaluateCondition(expr->right.get(), row);
            case Expr::NOT: return !evaluateCondition(expr->left.get(), row);
            case Expr::BETWEEN: {
                Value val = resolveValue(expr->between.val, row);
                Value start = resolveValue(expr->between.start, row);
                Value end = resolveValue(expr->between.end, row);
                if (std::holds_alternative<int>(val) && std::holds_alternative<int>(start) && std::holds_alternative<int>(end)) {
                    int v = std::get<int>(val), s = std::get<int>(start), e = std::get<int>(end);
                    return v >= s && v <= e;
                }
                return false;
            }
            case Expr::LIKE: {
                Value val = resolveValue(expr->like.val, row);
                Value pattern = resolveValue(expr->like.pattern, row);
                if (std::holds_alternative<std::string>(val) && std::holds_alternative<std::string>(pattern)) {
                    std::string v = std::get<std::string>(val);
                    std::string p = std::get<std::string>(pattern);
                    if (p.ends_with("%")) { p.pop_back(); return v.starts_with(p); }
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
        meta.columns = stmt.columns;
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

    // === НОВЫЙ МЕТОД ДЛЯ REVERT ===
    std::shared_ptr<Database> getDatabase(const std::string& name) const {
        // Здесь нужно вернуть сам объект? Но метод нестатический, так что мы в контексте Database.
        // Вообще этот метод должен быть в DBMS, а не в Database.
        // Для удобства оставим заглушку – реальная логика в DBMS.
        return nullptr;
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

    // === НУЖНЫЙ МЕТОД ДЛЯ REVERT С КВАЛИФИЦИРОВАННЫМ ИМЕНЕМ ===
    std::shared_ptr<Database> getDatabase(const std::string& name) const {
        auto it = databases_.find(name);
        return it == databases_.end() ? nullptr : it->second;
    }

    std::shared_ptr<Database> currentDatabase() const { return current_db_; }
    const std::string& getRootDir() const { return root_dir_; }
    std::vector<std::string> getDatabaseList() const {
        std::vector<std::string> db_list;
        for (const auto& [name, _] : databases_) db_list.push_back(name);
        return db_list;
    }

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
            auto [ok, error] = table->insertRow(stmt.values[i]);
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
            ++inserted;
			StringPool::instance().dumpStats(); //udali
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
                if (std::holds_alternative<int>(row[i])) std::cout << std::get<int>(row[i]);
                else if (std::holds_alternative<std::string>(row[i])) std::cout << std::get<std::string>(row[i]);
                else std::cout << "NULL";
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
        size_t updated = table->updateRows(stmt.assignments, stmt.condition.get());
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

    // === НОВАЯ ОБРАБОТКА REVERT ===
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