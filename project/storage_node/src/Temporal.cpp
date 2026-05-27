#include "../include/Temporal.h"
#include <filesystem>
#include <iostream>
#include <cstring>

TemporalManager::TemporalManager(const std::string& log_file_path)
    : log_path_(log_file_path) {
    openWriteAppend();
}

TemporalManager::~TemporalManager() {
    close();
}

bool TemporalManager::openRead() const {
    if (log_file_.is_open()) log_file_.close();
    log_file_.open(log_path_, std::ios::binary | std::ios::in);
    return log_file_.is_open();
}

bool TemporalManager::openWriteAppend() {
    if (log_file_.is_open()) log_file_.close();
    log_file_.open(log_path_, std::ios::binary | std::ios::out | std::ios::app);
    if (!log_file_.is_open()) {
        log_file_.open(log_path_, std::ios::binary | std::ios::out | std::ios::trunc);
    }
    return log_file_.is_open();
}

void TemporalManager::close() const{
    if (log_file_.is_open())
        log_file_.close();
}

void TemporalManager::writeRecordToStream(std::ostream& os, const LogRecord& rec) {
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        rec.timestamp.time_since_epoch()).count();
    os.write(reinterpret_cast<const char*>(&ts), sizeof(ts));
    uint8_t op = static_cast<uint8_t>(rec.type);
    os.write(reinterpret_cast<const char*>(&op), sizeof(op));
    os.write(reinterpret_cast<const char*>(&rec.row_id), sizeof(rec.row_id));

    uint64_t old_sz = rec.old_data.size();
    os.write(reinterpret_cast<const char*>(&old_sz), sizeof(old_sz));
    if (old_sz)
        os.write(rec.old_data.data(), old_sz);

    uint64_t new_sz = rec.new_data.size();
    os.write(reinterpret_cast<const char*>(&new_sz), sizeof(new_sz));
    if (new_sz)
        os.write(rec.new_data.data(), new_sz);
}

bool TemporalManager::readRecordFromStream(std::istream& is, LogRecord& rec) {
    uint64_t ts;
    if (!is.read(reinterpret_cast<char*>(&ts), sizeof(ts)))
        return false;
    rec.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(ts));

    uint8_t op;
    if (!is.read(reinterpret_cast<char*>(&op), sizeof(op)))
        return false;
    rec.type = static_cast<LogOpType>(op);

    if (!is.read(reinterpret_cast<char*>(&rec.row_id), sizeof(rec.row_id)))
        return false;

    uint64_t old_sz, new_sz;
    if (!is.read(reinterpret_cast<char*>(&old_sz), sizeof(old_sz)))
        return false;
    rec.old_data.resize(old_sz);
    if (old_sz && !is.read(&rec.old_data[0], old_sz))
        return false;

    if (!is.read(reinterpret_cast<char*>(&new_sz), sizeof(new_sz)))
        return false;
    rec.new_data.resize(new_sz);
    if (new_sz && !is.read(&rec.new_data[0], new_sz))
        return false;

    return true;
}

void TemporalManager::writeRecord(const LogRecord& rec) {
    if (!log_file_.is_open() && !openWriteAppend())
        return;
    log_file_.seekp(0, std::ios::end);
    writeRecordToStream(log_file_, rec);
    log_file_.flush();
}

bool TemporalManager::readRecord(LogRecord& rec) const{
    return readRecordFromStream(log_file_, rec);
}

void TemporalManager::logInsert(uint64_t row_id, const std::string& serialized_row) {
    LogRecord rec;
    rec.timestamp = std::chrono::system_clock::now();
    rec.type = LogOpType::INSERT;
    rec.row_id = row_id;
    rec.old_data = "";
    rec.new_data = serialized_row;
    writeRecord(rec);
}

void TemporalManager::logUpdate(uint64_t row_id, const std::string& old_serialized, const std::string& new_serialized) {
    LogRecord rec;
    rec.timestamp = std::chrono::system_clock::now();
    rec.type = LogOpType::UPDATE;
    rec.row_id = row_id;
    rec.old_data = old_serialized;
    rec.new_data = new_serialized;
    writeRecord(rec);
}

void TemporalManager::logDelete(uint64_t row_id, const std::string& serialized_row) {
    LogRecord rec;
    rec.timestamp = std::chrono::system_clock::now();
    rec.type = LogOpType::DELETE;
    rec.row_id = row_id;
    rec.old_data = serialized_row;
    rec.new_data = "";
    writeRecord(rec);
}

void TemporalManager::truncateAfter(std::chrono::system_clock::time_point time) {
    if (!openRead()) return;
    std::vector<LogRecord> keep;
    LogRecord rec;
    while (readRecord(rec)) {
        if (rec.timestamp <= time)
            keep.push_back(rec);
        else
            break; // лог хронологический, дальше все позже
    }
    close();

    openWriteAppend();
    log_file_.close();
    log_file_.open(log_path_, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!log_file_.is_open()) return;
    for (const auto& r : keep) {
        writeRecordToStream(log_file_, r);
    }
    log_file_.flush();
    close();
    openWriteAppend();
}

bool TemporalManager::revertTo(std::chrono::system_clock::time_point target_time,
                               std::function<bool(const LogRecord&)> applyUndo) {
    if (!openRead()) return false;
    std::vector<LogRecord> records;
    LogRecord rec;
    while (readRecord(rec)) {
        records.push_back(rec);
    }
    close();

    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        if (it->timestamp <= target_time)
            break;
        LogRecord undo;
        undo.row_id = it->row_id;
        switch (it->type) {
            case LogOpType::INSERT:
                undo.type = LogOpType::DELETE;
                undo.old_data = it->new_data;
                undo.new_data.clear();
                break;
            case LogOpType::DELETE:
                undo.type = LogOpType::INSERT;
                undo.old_data.clear();
                undo.new_data = it->old_data;
                break;
            case LogOpType::UPDATE:
                undo.type = LogOpType::UPDATE;
                undo.old_data = it->new_data;
                undo.new_data = it->old_data;
                break;
        }
        if (!applyUndo(undo))
            return false;
    }
    truncateAfter(target_time);
    return true;
}

std::chrono::system_clock::time_point TemporalManager::getLatestTimestamp() const {
    if (!openRead()) return std::chrono::system_clock::time_point();
    LogRecord rec;
    std::chrono::system_clock::time_point latest;
    while (readRecord(rec)) {
        if (rec.timestamp > latest)
            latest = rec.timestamp;
    }
    close();
    return latest;
}