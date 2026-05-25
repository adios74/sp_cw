#ifndef TEMPORAL_H
#define TEMPORAL_H

#include <string>
#include <cstdint>
#include <chrono>
#include <vector>
#include <functional>
#include <fstream>

namespace fs = std::filesystem;

enum class LogOpType : uint8_t {
    INSERT = 1,
    UPDATE = 2,
    DELETE = 3
};

struct LogRecord {
    std::chrono::system_clock::time_point timestamp;
    LogOpType type;
    uint64_t row_id;
    std::string old_data;   // сериализованная строка ДО (для UPDATE/DELETE)
    std::string new_data;   // сериализованная строка ПОСЛЕ (для INSERT/UPDATE)
};

class TemporalManager {
public:
    explicit TemporalManager(const std::string& log_file_path);
    ~TemporalManager();

    // Запись операций
    void logInsert(uint64_t row_id, const std::string& serialized_row);
    void logUpdate(uint64_t row_id, const std::string& old_serialized, const std::string& new_serialized);
    void logDelete(uint64_t row_id, const std::string& serialized_row);

    // Откат к моменту времени (applyUndo вызывается для каждой обратной операции)
    bool revertTo(std::chrono::system_clock::time_point target_time,
                  std::function<bool(const LogRecord& undoRecord)> applyUndo);

    // Обрезать лог после указанного времени
    void truncateAfter(std::chrono::system_clock::time_point time);

    std::chrono::system_clock::time_point getLatestTimestamp() const;

private:
    std::string log_path_;
    mutable std::fstream log_file_;

    bool openRead() const;
    bool openWriteAppend();
    void close() const;

    bool readRecord(LogRecord& rec) const;
    void writeRecord(const LogRecord& rec);

    static bool readRecordFromStream(std::istream& is, LogRecord& rec);
    static void writeRecordToStream(std::ostream& os, const LogRecord& rec);
};

#endif // TEMPORAL_H