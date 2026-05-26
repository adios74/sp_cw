#ifndef AST_H
#define AST_H

#include <string>
#include <vector>
#include <memory>
#include <variant>
#include <optional>

// Имена таблиц (с опциональной базой данных)
struct TableRef {
    std::string name;
    std::string database;
};

// Значения (литералы и ссылки на столбцы)
struct ColumnRef {
    std::string column;
    std::string table;
    std::string database;
};

// Тип значения: число, строка, ссылка на столбец или NULL
using Value = std::variant<
    int,
    std::string,
    ColumnRef,
    std::nullptr_t
>;

// Условия WHERE
enum class ComparisonOp {
    EQUAL,
    NOT_EQUAL,
    LESS,
    GREATER,
    LESS_OR_EQUAL,
    GREATER_OR_EQUAL
};

struct ComparisonExpr {
    ComparisonOp op;
    Value left;
    Value right;
};

struct BetweenExpr {
    Value val;
    Value start;
    Value end;
};

struct LikeExpr {
    Value val;
    Value pattern;
};

struct Expr {
    enum Type { COMPARISON, BETWEEN, LIKE, AND, OR, NOT };
    Type type;

    ComparisonExpr comparison;
    BetweenExpr between;
    LikeExpr like;
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
};

// Определения столбцов (CREATE TABLE)
enum class ColumnConstraint {
    NONE,
    NOT_NULL,
    INDEXED
};

struct ColumnDef {
    std::string name;
    std::string type;
    ColumnConstraint constraint;
    std::optional<Value> default_value;
};

// Выражения для списка SELECT (включая агрегатные функции)
enum class AggFunc { SUM, COUNT, AVG };

struct AggCall {
    AggFunc func;
    Value arg;
};

// То, что может стоять после SELECT: звёздочка, колонка, агрегат
using SelectExpr = std::variant<
    std::monostate,
    ColumnRef,
    AggCall
>;

struct SelectColumn {
    SelectExpr expr;
    std::optional<std::string> alias;
};

// SQL-команды
struct CreateDatabaseStmt {
    std::string name;
};

struct DropDatabaseStmt {
    std::string name;
};

struct UseStmt {
    std::string name;
};

struct CreateTableStmt {
    TableRef table;
    std::vector<ColumnDef> columns;
};

struct DropTableStmt {
    TableRef table;
};

struct InsertStmt {
    TableRef table;
    std::vector<std::string> columns;
    std::vector<std::vector<Value>> values;
};

struct UpdateStmt {
    TableRef table;
    std::vector<std::pair<std::string, Value>> assignments;
    std::unique_ptr<Expr> condition;
};

struct DeleteStmt {
    TableRef table;
    std::unique_ptr<Expr> condition;
};

struct SelectStmt {
    std::vector<SelectColumn> columns;
    TableRef table;
    std::unique_ptr<Expr> condition;
};

// Корневой узел AST
using Statement = std::variant<
    CreateDatabaseStmt,
    DropDatabaseStmt,
    UseStmt,
    CreateTableStmt,
    DropTableStmt,
    InsertStmt,
    UpdateStmt,
    DeleteStmt,
    SelectStmt
>;

// Вспомогательные функции (отладка и фабрики)
std::string astToString(const Statement& stmt);
std::string valueToString(const Value& v);
std::string exprToString(const Expr* expr);
std::string selectExprToString(const SelectExpr& se);

// Фабрики для Expr
std::unique_ptr<Expr> makeComparison(ComparisonOp op, Value left, Value right);
std::unique_ptr<Expr> makeBetween(Value val, Value start, Value end);
std::unique_ptr<Expr> makeLike(Value val, Value pattern);
std::unique_ptr<Expr> makeAnd(std::unique_ptr<Expr> left, std::unique_ptr<Expr> right);
std::unique_ptr<Expr> makeOr(std::unique_ptr<Expr> left, std::unique_ptr<Expr> right);
std::unique_ptr<Expr> makeNot(std::unique_ptr<Expr> expr);

#endif