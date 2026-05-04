#include "../include/AST.h"
#include <sstream>


std::string valueToString(const Value& v) {
    struct Visitor {
        std::string operator()(int i) { 
            return std::to_string(i); 
        }
        std::string operator()(const std::string& s) { 
            return "\"" + s + "\""; 
        }
        std::string operator()(const ColumnRef& r) {
            std::ostringstream oss;
            if (!r.database.empty()) {
                oss << r.database << ".";
            }
            if (!r.table.empty()) {
                oss << r.table << ".";
            }
            oss << r.column;
            return oss.str();
        }
        std::string operator()(std::nullptr_t) { 
            return "NULL"; 
        }
    };
    return std::visit(Visitor{}, v);
}

std::string selectExprToString(const SelectExpr& se) {
    struct Visitor {
        std::string operator()(std::monostate) { 
            return "*"; 
        }
        std::string operator()(const ColumnRef& cr) { 
            return valueToString(cr); 
        }
        std::string operator()(const AggCall& ag) {
            std::string funcName;
            switch (ag.func) {
                case AggFunc::SUM:   funcName = "SUM"; break;
                case AggFunc::COUNT: funcName = "COUNT"; break;
                case AggFunc::AVG:   funcName = "AVG"; break;
            }
            if (std::holds_alternative<std::nullptr_t>(ag.arg)) {
                return funcName + "(*)";
            }
            return funcName + "(" + valueToString(ag.arg) + ")";
        }
    };
    return std::visit(Visitor{}, se);
}

std::string exprToString(const Expr* expr) {
    if (!expr) return "(null)";

    switch (expr->type) {
        case Expr::COMPARISON: {
            std::string op;
            switch (expr->comparison.op) {
                case ComparisonOp::EQUAL:            op = "=="; break;
                case ComparisonOp::NOT_EQUAL:        op = "!="; break;
                case ComparisonOp::LESS:             op = "<";  break;
                case ComparisonOp::GREATER:          op = ">";  break;
                case ComparisonOp::LESS_OR_EQUAL:    op = "<="; break;
                case ComparisonOp::GREATER_OR_EQUAL: op = ">="; break;
            }
            return valueToString(expr->comparison.left) + " " + op + " " +
                   valueToString(expr->comparison.right);
        }
        case Expr::BETWEEN:
            return valueToString(expr->between.val) + " BETWEEN " +
                   valueToString(expr->between.start) + " AND " +
                   valueToString(expr->between.end);
        case Expr::LIKE:
            return valueToString(expr->like.val) + " LIKE \"" + expr->like.pattern + "\"";
        case Expr::AND:
            return "(" + exprToString(expr->left.get()) + " AND " +
                   exprToString(expr->right.get()) + ")";
        case Expr::OR:
            return "(" + exprToString(expr->left.get()) + " OR " +
                   exprToString(expr->right.get()) + ")";
        case Expr::NOT:
            return "NOT " + exprToString(expr->left.get());
    }
    return "unknown";
}

std::string astToString(const Statement& stmt) {
    std::ostringstream oss;

    struct Visitor {
        std::ostringstream& oss;

        void operator()(const CreateDatabaseStmt& s) {
            oss << "CREATE DATABASE " << s.name;
        }
        void operator()(const DropDatabaseStmt& s) {
            oss << "DROP DATABASE " << s.name;
        }
        void operator()(const UseStmt& s) {
            oss << "USE " << s.name;
        }
        void operator()(const CreateTableStmt& s) {
            oss << "CREATE TABLE " << s.name << " (";
            for (size_t i = 0; i < s.columns.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << s.columns[i].name << " " << s.columns[i].type;
                switch (s.columns[i].constraint) {
                    case ColumnConstraint::NOT_NULL: oss << " NOT_NULL"; break;
                    case ColumnConstraint::INDEXED:  oss << " INDEXED"; break;
                    default: break;
                }
                if (s.columns[i].default_value) {
                    oss << " DEFAULT " << valueToString(*s.columns[i].default_value);
                }
            }
            oss << ")";
        }
        void operator()(const DropTableStmt& s) {
            oss << "DROP TABLE " << s.name;
        }
        void operator()(const InsertStmt& s) {
            oss << "INSERT INTO ";
            if (!s.table.database.empty()) oss << s.table.database << ".";
            oss << s.table.name << " (";
            for (size_t i = 0; i < s.columns.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << s.columns[i];
            }
            oss << ") VALUE ";
            for (size_t i = 0; i < s.values.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << "(";
                for (size_t j = 0; j < s.values[i].size(); ++j) {
                    if (j > 0) oss << ", ";
                    oss << valueToString(s.values[i][j]);
                }
                oss << ")";
            }
        }
        void operator()(const UpdateStmt& s) {
            oss << "UPDATE ";
            if (!s.table.database.empty()) oss << s.table.database << ".";
            oss << s.table.name << " SET ";
            for (size_t i = 0; i < s.assignments.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << s.assignments[i].first << " = "
                    << valueToString(s.assignments[i].second);
            }
            if (s.condition) {
                oss << " WHERE " << exprToString(s.condition.get());
            }
        }
        void operator()(const DeleteStmt& s) {
            oss << "DELETE FROM ";
            if (!s.table.database.empty()) oss << s.table.database << ".";
            oss << s.table.name;
            if (s.condition) {
                oss << " WHERE " << exprToString(s.condition.get());
            }
        }
        void operator()(const SelectStmt& s) {
            oss << "SELECT ";
            if (s.columns.empty()) {
                oss << "*";
            } else {
                for (size_t i = 0; i < s.columns.size(); ++i) {
                    if (i > 0) oss << ", ";
                    oss << selectExprToString(s.columns[i].expr);
                    if (s.columns[i].alias) {
                        oss << " AS " << *s.columns[i].alias;
                    }
                }
            }
            oss << " FROM ";
            if (!s.table.database.empty()) oss << s.table.database << ".";
            oss << s.table.name;
            if (s.condition) {
                oss << " WHERE " << exprToString(s.condition.get());
            }
        }
    };

    Visitor v{oss};
    std::visit(v, stmt);
    return oss.str();
}


std::unique_ptr<Expr> makeComparison(ComparisonOp op, Value left, Value right) {
    auto e = std::make_unique<Expr>();
    e->type = Expr::COMPARISON;
    e->comparison.op = op;
    e->comparison.left = std::move(left);
    e->comparison.right = std::move(right);
    return e;
}

std::unique_ptr<Expr> makeBetween(Value val, Value start, Value end) {
    auto e = std::make_unique<Expr>();
    e->type = Expr::BETWEEN;
    e->between.val = std::move(val);
    e->between.start = std::move(start);
    e->between.end = std::move(end);
    return e;
}

std::unique_ptr<Expr> makeLike(Value val, std::string pattern) {
    auto e = std::make_unique<Expr>();
    e->type = Expr::LIKE;
    e->like.val = std::move(val);
    e->like.pattern = std::move(pattern);
    return e;
}

std::unique_ptr<Expr> makeAnd(std::unique_ptr<Expr> left, std::unique_ptr<Expr> right) {
    auto e = std::make_unique<Expr>();
    e->type = Expr::AND;
    e->left = std::move(left);
    e->right = std::move(right);
    return e;
}

std::unique_ptr<Expr> makeOr(std::unique_ptr<Expr> left, std::unique_ptr<Expr> right) {
    auto e = std::make_unique<Expr>();
    e->type = Expr::OR;
    e->left = std::move(left);
    e->right = std::move(right);
    return e;
}

std::unique_ptr<Expr> makeNot(std::unique_ptr<Expr> expr) {
    auto e = std::make_unique<Expr>();
    e->type = Expr::NOT;
    e->left = std::move(expr);
    return e;
}