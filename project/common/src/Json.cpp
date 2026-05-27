#include "../include/Json.h"
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <algorithm>

nlohmann::json valueToJson(const Value& val) {
    
    return std::visit([](auto&& arg) -> nlohmann::json {

        using T = std::decay_t<decltype(arg)>;

        if constexpr (std::is_same_v<T, int>) {
            return arg;

        } else if constexpr (std::is_same_v<T, std::string>) {
            return arg;

        } else if constexpr (std::is_same_v<T, ColumnRef>) {
            return arg.column;

        } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
            return nullptr;
        }
    }, val);
}

std::string formatSelectResult(const SelectStmt& stmt,
                               const std::vector<Row>& rows,
                               const TableSchema& schema) {
    using json = nlohmann::json;

    struct OutputColumn {
        int index;                 // индекс колонки в Row (или -1 для агрегатов)
        std::string jsonKey;       // имя ключа в выходном JSON
        bool isAggregate = false;  // является ли агрегатной функцией
        AggFunc aggFunc;           // если агрегат
        int aggArgIndex = -1;      // индекс аргумента агрегата в Row (или -1 для COUNT(*))
    };

    std::vector<OutputColumn> outputCols;

    if (stmt.columns.empty()) {

        for (size_t i = 0; i < schema.columnNames.size(); ++i) {
            OutputColumn col;
            col.index = static_cast<int>(i);
            col.jsonKey = schema.columnNames[i];
            col.isAggregate = false;
            col.aggArgIndex = -1;
            outputCols.push_back(col);
        }

    } else {
        for (const auto& selCol : stmt.columns) {
            OutputColumn out;
            std::visit([&](const auto& expr) {
                using T = std::decay_t<decltype(expr)>;

                if constexpr (std::is_same_v<T, std::monostate>) {
                    throw std::runtime_error("Unexpected star in select list");

                } else if constexpr (std::is_same_v<T, ColumnRef>) {
                    out.jsonKey = selCol.alias.value_or(expr.column);
                    auto it = std::find(schema.columnNames.begin(), schema.columnNames.end(), expr.column);

                    if (it == schema.columnNames.end()) {
                        throw std::runtime_error("Column " + expr.column + " not found in schema");
                    }
                    out.index = static_cast<int>(std::distance(schema.columnNames.begin(), it));

                } else if constexpr (std::is_same_v<T, AggCall>) {
                    out.isAggregate = true;
                    out.aggFunc = expr.func;
                    out.jsonKey = selCol.alias.value_or(
                        [&]() -> std::string {
                            switch (expr.func) {
                                case AggFunc::SUM: return "SUM";
                                case AggFunc::COUNT: return "COUNT";
                                case AggFunc::AVG: return "AVG";
                                default: return "UNKNOWN";
                            }
                        }()
                    );
                    if (std::holds_alternative<ColumnRef>(expr.arg)) {
                        const auto& colRef = std::get<ColumnRef>(expr.arg);
                        auto it = std::find(schema.columnNames.begin(), schema.columnNames.end(), colRef.column);
                        if (it == schema.columnNames.end())
                            throw std::runtime_error("Aggregate argument column not found");
                        out.aggArgIndex = static_cast<int>(std::distance(schema.columnNames.begin(), it));
                    } else if (std::holds_alternative<std::nullptr_t>(expr.arg)) {
                        out.aggArgIndex = -1;
                    } else {
                        throw std::runtime_error("Aggregate argument must be a column or *");
                    }
                }
            }, selCol.expr);
            outputCols.push_back(out);
        }
    }

    bool hasAggregates = std::any_of(outputCols.begin(), outputCols.end(),
                                     [](const OutputColumn& c) { return c.isAggregate; });

    if (hasAggregates) {
        json resultObj = json::object();
        for (const auto& out : outputCols) {
            if (out.isAggregate) {
                double aggValue = 0.0;
                if (out.aggFunc == AggFunc::COUNT) {
                    if (out.aggArgIndex == -1) {
                        aggValue = static_cast<double>(rows.size());
                    } else {
                        int cnt = 0;
                        for (const auto& row : rows) {
                            if (!std::holds_alternative<std::nullptr_t>(row[out.aggArgIndex]))
                                ++cnt;
                        }
                        aggValue = cnt;
                    }
                } else if (out.aggFunc == AggFunc::SUM) {
                    double sum = 0.0;
                    for (const auto& row : rows) {
                        const Value& v = row[out.aggArgIndex];
                        std::visit([&](const auto& val) {
                            if constexpr (std::is_same_v<std::decay_t<decltype(val)>, int>)
                                sum += val;
                            else if constexpr (std::is_same_v<std::decay_t<decltype(val)>, std::string>)
                                throw std::runtime_error("SUM on string column");
                        }, v);
                    }
                    aggValue = sum;
                } else if (out.aggFunc == AggFunc::AVG) {
                    double sum = 0.0;
                    int cnt = 0;
                    for (const auto& row : rows) {
                        const Value& v = row[out.aggArgIndex];
                        std::visit([&](const auto& val) {
                            if constexpr (std::is_same_v<std::decay_t<decltype(val)>, int>) {
                                sum += val;
                                ++cnt;
                            }
                        }, v);
                    }
                    aggValue = cnt > 0 ? sum / cnt : 0;
                }
                resultObj[out.jsonKey] = aggValue;
            } else {
                if (!rows.empty()) {
                    resultObj[out.jsonKey] = valueToJson(rows[0][out.index]);
                } else {
                    resultObj[out.jsonKey] = nullptr;
                }
            }
        }
        return resultObj.dump(4);
    }

    json jsonArray = json::array();
    for (const auto& row : rows) {
        json rowObj = json::object();
        for (const auto& out : outputCols) {
            rowObj[out.jsonKey] = valueToJson(row[out.index]);
        }
        jsonArray.push_back(rowObj);
    }
    return jsonArray.dump(4);
}