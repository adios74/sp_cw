#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include "../../common/include/StringPool.h"

enum class DataType : uint8_t { INT = 0, STRING = 1 };

struct DBValue {
    bool      is_null = true;
    DataType  type    = DataType::INT;
    int32_t   ival    = 0;
    std::shared_ptr<const std::string> sptr;

    DBValue() = default;

    static DBValue null_of(DataType t) { DBValue v; v.is_null = true; v.type = t; return v; }
    static DBValue of_int(int32_t i)   { DBValue v; v.is_null = false; v.type = DataType::INT; v.ival = i; return v; }
    static DBValue of_str(const std::string& s) {
        DBValue v;
        v.is_null = false;
        v.type = DataType::STRING;
        v.sptr = StringPool::instance().intern(s);
        return v;
    }
    static DBValue of_interned(std::shared_ptr<const std::string> ptr) {
        DBValue v;
        v.is_null = false;
        v.type = DataType::STRING;
        v.sptr = std::move(ptr);
        return v;
    }

    bool operator==(const DBValue& o) const {
        if (is_null && o.is_null) return true;
        if (is_null || o.is_null) return false;
        if (type != o.type) return false;
        if (type == DataType::INT) return ival == o.ival;
        return *sptr == *o.sptr;
    }
    bool operator!=(const DBValue& o) const { return !(*this == o); }
    bool operator<(const DBValue& o) const {
        if (is_null && o.is_null) return false;
        if (is_null)   return true;
        if (o.is_null) return false;
        if (type == DataType::INT) return ival < o.ival;
        return *sptr < *o.sptr;
    }
    bool operator<=(const DBValue& o) const { return !(o < *this); }
    bool operator>(const DBValue& o)  const { return o < *this; }
    bool operator>=(const DBValue& o) const { return !(*this < o); }

    std::string to_json() const {
        if (is_null) return "null";
        if (type == DataType::INT) return std::to_string(ival);
        std::string r = "\"";
        for (char c : *sptr) {
            if      (c == '"')  r += "\\\"";
            else if (c == '\\') r += "\\\\";
            else if (c == '\n') r += "\\n";
            else if (c == '\r') r += "\\r";
            else if (c == '\t') r += "\\t";
            else                r += c;
        }
        return r + "\"";
    }

    std::string to_display() const {
        if (is_null) return "NULL";
        if (type == DataType::INT) return std::to_string(ival);
        return *sptr;
    }

    const std::string& getString() const {
        static const std::string empty;
        if (is_null || type != DataType::STRING) return empty;
        return *sptr;
    }
};