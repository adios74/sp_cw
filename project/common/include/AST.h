#ifndef AST_H
#define AST_H

#include <string>
#include <vector>
#include <memory>
#include <variant>
#include <optional>

// ============================================================
// Имена таблиц (с опциональной базой данных)
// ============================================================
struct TableRef {
    std::string name;           // имя таблицы
    std::string database;       // база данных (если пусто — используется текущая)
};

// ============================================================
// Значения (литералы и ссылки на столбцы)
// ============================================================
struct ColumnRef {
    std::string column;         // имя колонки
    std::string table;          // опционально: table.column
    std::string database;       // опционально: db.table.column
};

// Тип значения: число, строка, ссылка на столбец или NULL
using Value = std::variant<
    int,                        // INT_LITERAL
    std::string,                // STRING_LITERAL
    ColumnRef,                  // имя колонки
    std::nullptr_t              // NULL
>;

// ============================================================
// Условия WHERE
// ============================================================
enum class ComparisonOp {
    EQUAL,              // ==
    NOT_EQUAL,          // !=
    LESS,               // <
    GREATER,            // >
    LESS_OR_EQUAL,      // <=
    GREATER_OR_EQUAL    // >=
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
    std::string pattern;   // регулярное выражение
};

struct Expr {
    enum Type { COMPARISON, BETWEEN, LIKE, AND, OR, NOT };
    Type type;

    // В зависимости от type активно одно из полей
    ComparisonExpr comparison;
    BetweenExpr between;
    LikeExpr like;
    std::unique_ptr<Expr> left;    // для AND/OR/NOT
    std::unique_ptr<Expr> right;   // для AND/OR
};

// ============================================================
// Определения столбцов (CREATE TABLE)
// ============================================================
enum class ColumnConstraint {
    NONE,
    NOT_NULL,
    INDEXED
};

struct ColumnDef {
    std::string name;
    std::string type;                        // "INT" или "STRING"
    ColumnConstraint constraint;
    std::optional<Value> default_value;      // DEFAULT value
};

// ============================================================
// Выражения для списка SELECT (включая агрегатные функции)
// ============================================================
enum class AggFunc { SUM, COUNT, AVG };

struct AggCall {
    AggFunc func;
    // Аргумент: колонка или NULL (для COUNT(*) можно передать nullptr)
    Value arg;
};

// То, что может стоять после SELECT: звёздочка, колонка, агрегат
using SelectExpr = std::variant<
    std::monostate,     // SELECT *
    ColumnRef,          // SELECT col
    AggCall             // SELECT SUM(col)
>;

struct SelectColumn {
    SelectExpr expr;
    std::optional<std::string> alias;   // AS alias
};

// ============================================================
// SQL-команды
// ============================================================
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
    std::string name;
    std::vector<ColumnDef> columns;
};

struct DropTableStmt {
    std::string name;   // можно расширить до TableRef, но проще без базы
};

struct InsertStmt {
    TableRef table;
    std::vector<std::string> columns;
    std::vector<std::vector<Value>> values;   // множественная вставка
};

struct UpdateStmt {
    TableRef table;
    std::vector<std::pair<std::string, Value>> assignments;
    std::unique_ptr<Expr> condition;          // WHERE обязателен по синтаксису
};

struct DeleteStmt {
    TableRef table;
    std::unique_ptr<Expr> condition;          // WHERE обязателен
};

struct SelectStmt {
    // Если columns пуст — это SELECT *
    std::vector<SelectColumn> columns;
    TableRef table;
    std::unique_ptr<Expr> condition;          // может отсутствовать (nullptr)
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

// ============================================================
// Вспомогательные функции (отладка и фабрики)
// ============================================================
std::string astToString(const Statement& stmt);
std::string valueToString(const Value& v);
std::string exprToString(const Expr* expr);
std::string selectExprToString(const SelectExpr& se);

// Фабрики для Expr
std::unique_ptr<Expr> makeComparison(ComparisonOp op, Value left, Value right);
std::unique_ptr<Expr> makeBetween(Value val, Value start, Value end);
std::unique_ptr<Expr> makeLike(Value val, std::string pattern);
std::unique_ptr<Expr> makeAnd(std::unique_ptr<Expr> left, std::unique_ptr<Expr> right);
std::unique_ptr<Expr> makeOr(std::unique_ptr<Expr> left, std::unique_ptr<Expr> right);
std::unique_ptr<Expr> makeNot(std::unique_ptr<Expr> expr);

#endif // AST_H