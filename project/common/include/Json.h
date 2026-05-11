#ifndef JSON_FORMATTER_H
#define JSON_FORMATTER_H

#include <string>
#include <vector>
#include "../include/AST.h"
#include "../include/json.hpp"

using Row = std::vector<Value>;

struct TableSchema {
    std::vector<std::string> columnNames;
    // потом доработать
};

nlohmann::json valueToJson(const Value& val);

inline std::string formatSelectResult(const SelectStmt& stmt,
                                      const std::vector<Row>& rows,
                                      const TableSchema& schema) {
}

#endif