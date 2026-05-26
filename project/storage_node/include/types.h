#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <cassert>

enum class DataType : uint8_t { INT = 0, STRING = 1 };

struct Value {
    bool      is_null = true;
    DataType  type    = DataType::INT;
    int32_t   ival    = 0;
    std::string sval;

    Value() = default;

    static Value null_of(DataType t) { Value v; v.is_null=true; v.type=t; return v; }
    static Value of_int(int32_t i)   { Value v; v.is_null=false; v.type=DataType::INT; v.ival=i; return v; }
    static Value of_str(std::string s){ Value v; v.is_null=false; v.type=DataType::STRING; v.sval=std::move(s); return v; }

    bool operator==(const Value& o) const {
        if (is_null && o.is_null) return true;
        if (is_null || o.is_null) return false;
        if (type != o.type) return false;
        return type==DataType::INT ? ival==o.ival : sval==o.sval;
    }
    bool operator!=(const Value& o) const { return !(*this==o); }
    bool operator<(const Value& o) const {
        if (is_null && o.is_null) return false;
        if (is_null)   return true;
        if (o.is_null) return false;
        if (type==DataType::INT) return ival < o.ival;
        return sval < o.sval;
    }
    bool operator<=(const Value& o) const { return !(o < *this); }
    bool operator>(const Value& o)  const { return o < *this; }
    bool operator>=(const Value& o) const { return !(*this < o); }

    std::string to_json() const {
        if (is_null) return "null";
        if (type==DataType::INT) return std::to_string(ival);
        std::string r="\"";
        for (char c: sval) {
            if      (c=='"')  r+="\\\"";
            else if (c=='\\') r+="\\\\";
            else if (c=='\n') r+="\\n";
            else if (c=='\r') r+="\\r";
            else if (c=='\t') r+="\\t";
            else              r+=c;
        }
        return r+"\"";
    }
    std::string to_display() const {
        if (is_null) return "NULL";
        if (type==DataType::INT) return std::to_string(ival);
        return sval;
    }
};

struct ColumnDef {
    std::string name;
    DataType    type     = DataType::INT;
    bool        not_null = false;
    bool        indexed  = false;   // implies not_null + unique
};

using Record = std::vector<Value>;