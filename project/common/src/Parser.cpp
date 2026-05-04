#include "../include/Parser.h"
#include <stdexcept>
#include <sstream>

Parser::Parser(Lexer& lexer) : lexer(lexer) {
    advance();
}

void Parser::advance() {
    currentToken = lexer.nextToken();
}

bool Parser::check(TokenType type) const {
    return currentToken.type == type;
}

Token Parser::consume(TokenType type, const std::string& errorMsg) {
    if (check(type)) {
        Token t = currentToken;
        advance();
        return t;
    }
    std::ostringstream oss;
    oss << "Syntax error at line " << currentToken.line 
        << ", column " << currentToken.column 
        << ": " << errorMsg;
    throw std::runtime_error(oss.str());
}

bool Parser::match(TokenType type) {
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}


// Разбор квалифицированных имён: a, a.b, a.b.c
std::vector<std::string> Parser::parseQualifiedName() {
    std::vector<std::string> parts;
    parts.push_back(consume(TokenType::IDENTIFIER, "Expected identifier").text);

    while (match(TokenType::DOT)) {
        parts.push_back(consume(TokenType::IDENTIFIER, "Expected identifier after '.'").text);
    }
    return parts;
}

TableRef Parser::parseTableRef() {
    auto parts = parseQualifiedName();
    TableRef ref;
    if (parts.size() == 1) {
        ref.name = parts[0];
    } else if (parts.size() == 2) {
        ref.database = parts[0];
        ref.name = parts[1];
    } else {
        throw std::runtime_error("Invalid table reference: too many parts");
    }
    return ref;
}

ColumnRef Parser::parseColumnRef() {
    auto parts = parseQualifiedName();
    ColumnRef ref;
    if (parts.size() == 1) {
        ref.column = parts[0];
    } else if (parts.size() == 2) {
        ref.table = parts[0];
        ref.column = parts[1];
    } else if (parts.size() == 3) {
        ref.database = parts[0];
        ref.table = parts[1];
        ref.column = parts[2];
    } else {
        throw std::runtime_error("Invalid column reference: too many parts");
    }
    return ref;
}

// Литералы и операнды
Value Parser::parseLiteral() {
    if (match(TokenType::NULL_VAL)) {
        return nullptr;
    } else if (check(TokenType::INT_LITERAL)) {
        std::string text = currentToken.text;
        advance();
        try {
            return std::stoi(text);
        } catch (...) {
            throw std::runtime_error("Invalid integer literal: " + text);
        }
    } else if (check(TokenType::STRING_LITERAL)) {
        std::string text = currentToken.text;
        advance();
        return text;
    } else {
        throw std::runtime_error("Expected literal value, but got '" + currentToken.text + "'");
    }
}

Value Parser::parseOperand() {
    if (check(TokenType::IDENTIFIER)) {
        return parseColumnRef();
    } else {
        return parseLiteral();
    }
}

// CREATE TABLE: определение столбца
ColumnDef Parser::parseColumnDef() {
    ColumnDef col;
    col.name = consume(TokenType::IDENTIFIER, "Expected column name").text;

    if (match(TokenType::INT_TYPE)) {
        col.type = "INT";
    } else if (match(TokenType::STRING_TYPE)) {
        col.type = "STRING";
    } else {
        throw std::runtime_error("Expected data type (INT or STRING) for column " + col.name);
    }

    col.constraint = ColumnConstraint::NONE;
    if (match(TokenType::INDEXED)) {
        col.constraint = ColumnConstraint::INDEXED;
    } else if (match(TokenType::NOT_NULL)) {
        col.constraint = ColumnConstraint::NOT_NULL;
    }

    if (match(TokenType::DEFAULT)) {
        col.default_value = parseLiteral();
    }

    return col;
}

// DDL: CREATE/DROP DATABASE, USE, CREATE/DROP TABLE
Statement Parser::parseDDL() {
    if (match(TokenType::CREATE)) {
        if (match(TokenType::DATABASE)) {
            return parseCreateDatabase();
        } else if (match(TokenType::TABLE)) {
            return parseCreateTable();
        } else {
            throw std::runtime_error("Expected DATABASE or TABLE after CREATE");
        }
    } else if (match(TokenType::DROP)) {
        if (match(TokenType::DATABASE)) {
            return parseDropDatabase();
        } else if (match(TokenType::TABLE)) {
            return parseDropTable();
        } else {
            throw std::runtime_error("Expected DATABASE or TABLE after DROP");
        }
    } else if (match(TokenType::USE)) {
        return parseUse();
    }
    throw std::runtime_error("Expected DDL statement (CREATE, DROP, USE)");
}

CreateDatabaseStmt Parser::parseCreateDatabase() {
    CreateDatabaseStmt stmt;
    stmt.name = consume(TokenType::IDENTIFIER, "Expected database name").text;
    return stmt;
}

DropDatabaseStmt Parser::parseDropDatabase() {
    DropDatabaseStmt stmt;
    stmt.name = consume(TokenType::IDENTIFIER, "Expected database name").text;
    return stmt;
}

UseStmt Parser::parseUse() {
    UseStmt stmt;
    stmt.name = consume(TokenType::IDENTIFIER, "Expected database name after USE").text;
    return stmt;
}

CreateTableStmt Parser::parseCreateTable() {
    CreateTableStmt stmt;
    stmt.name = consume(TokenType::IDENTIFIER, "Expected table name").text;
    consume(TokenType::LBRACKET, "Expected '(' after table name");
    if (!check(TokenType::RBRACKET)) {
        stmt.columns.push_back(parseColumnDef());
        while (match(TokenType::COMMA)) {
            stmt.columns.push_back(parseColumnDef());
        }
    }
    consume(TokenType::RBRACKET, "Expected ')' after column definitions");
    return stmt;
}

DropTableStmt Parser::parseDropTable() {
    DropTableStmt stmt;
    stmt.name = consume(TokenType::IDENTIFIER, "Expected table name").text;
    return stmt;
}

// DML: INSERT, UPDATE, DELETE, SELECT
Statement Parser::parseDML() {
    if (match(TokenType::INSERT)) {
        return parseInsert();
    } else if (match(TokenType::UPDATE)) {
        return parseUpdate();
    } else if (match(TokenType::DELETE)) {
        return parseDelete();
    } else if (match(TokenType::SELECT)) {
        return parseSelect();
    } else {
        throw std::runtime_error("Expected DML statement (INSERT, UPDATE, DELETE, SELECT)");
    }
}

InsertStmt Parser::parseInsert() {
    InsertStmt stmt;
    consume(TokenType::INTO, "Expected INTO after INSERT");
    stmt.table = parseTableRef();

    consume(TokenType::LBRACKET, "Expected '(' after table name");
    if (!check(TokenType::RBRACKET)) {
        stmt.columns.push_back(consume(TokenType::IDENTIFIER, "Expected column name").text);
        while (match(TokenType::COMMA)) {
            stmt.columns.push_back(consume(TokenType::IDENTIFIER, "Expected column name").text);
        }
    }
    consume(TokenType::RBRACKET, "Expected ')' after column list");

    consume(TokenType::VALUE, "Expected VALUE");

    do {
        consume(TokenType::LBRACKET, "Expected '(' before value list");
        std::vector<Value> values;
        if (!check(TokenType::RBRACKET)) {
            values.push_back(parseLiteral());
            while (match(TokenType::COMMA)) {
                values.push_back(parseLiteral());
            }
        }
        consume(TokenType::RBRACKET, "Expected ')' after value list");
        stmt.values.push_back(std::move(values));
    } while (match(TokenType::COMMA));

    return stmt;
}

UpdateStmt Parser::parseUpdate() {
    UpdateStmt stmt;
    stmt.table = parseTableRef();

    consume(TokenType::SET, "Expected SET");

    do {
        std::string col = consume(TokenType::IDENTIFIER, "Expected column name").text;
        consume(TokenType::OP_ASSIGN, "Expected '=' after column name");
        Value val = parseOperand();
        stmt.assignments.emplace_back(col, std::move(val));
    } while (match(TokenType::COMMA));

    consume(TokenType::WHERE, "WHERE clause is mandatory for UPDATE");
    stmt.condition = parseExpression();

    return stmt;
}

DeleteStmt Parser::parseDelete() {
    DeleteStmt stmt;
    consume(TokenType::FROM, "Expected FROM after DELETE");
    stmt.table = parseTableRef();

    consume(TokenType::WHERE, "WHERE clause is mandatory for DELETE");
    stmt.condition = parseExpression();

    return stmt;
}

// SELECT
SelectStmt Parser::parseSelect() {
    SelectStmt stmt;
    if (match(TokenType::STAR)) {
    } else {
        stmt.columns.push_back(parseSelectItem());
        while (match(TokenType::COMMA)) {
            stmt.columns.push_back(parseSelectItem());
        }
    }

    consume(TokenType::FROM, "Expected FROM in SELECT");
    stmt.table = parseTableRef();

    if (match(TokenType::WHERE)) {
        stmt.condition = parseExpression();
    }

    return stmt;
}

SelectColumn Parser::parseSelectItem() {
    SelectColumn col;

    if (check(TokenType::SUM) || check(TokenType::COUNT) || check(TokenType::AVG)) {
        AggCall agg;
        agg.func = parseAggFunc();
        consume(TokenType::LBRACKET, "Expected '(' after aggregate function");
        if (match(TokenType::STAR)) {
            if (agg.func != AggFunc::COUNT) {
                throw std::runtime_error("Only COUNT(*) is supported, not SUM(*) or AVG(*)");
            }
            agg.arg = nullptr;
        } else {
            agg.arg = parseOperand();
        }
        consume(TokenType::RBRACKET, "Expected ')' after aggregate argument");
        col.expr = std::move(agg);
    } else {
        ColumnRef ref = parseColumnRef();
        col.expr = ref;
    }

    if (match(TokenType::AS)) {
        col.alias = consume(TokenType::IDENTIFIER, "Expected alias after AS").text;
    }

    return col;
}

AggFunc Parser::parseAggFunc() {
    if (match(TokenType::SUM)) return AggFunc::SUM;
    if (match(TokenType::COUNT)) return AggFunc::COUNT;
    if (match(TokenType::AVG)) return AggFunc::AVG;
    throw std::runtime_error("Expected aggregate function (SUM, COUNT, AVG)");
}

// WHERE (conditions)
std::unique_ptr<Expr> Parser::parseWhereClause() {
    consume(TokenType::WHERE, "Expected WHERE");
    return parseExpression();
}

std::unique_ptr<Expr> Parser::parseExpression() {
    return parseOrExpr();
}

std::unique_ptr<Expr> Parser::parseOrExpr() {
    auto left = parseAndExpr();
    while (match(TokenType::OR)) {
        auto right = parseAndExpr();
        left = makeOr(std::move(left), std::move(right));
    }
    return left;
}

std::unique_ptr<Expr> Parser::parseAndExpr() {
    auto left = parsePrimaryExpr();
    while (match(TokenType::AND)) {
        auto right = parsePrimaryExpr();
        left = makeAnd(std::move(left), std::move(right));
    }
    return left;
}

std::unique_ptr<Expr> Parser::parsePrimaryExpr() {
    if (match(TokenType::LBRACKET)) {
        auto expr = parseExpression();
        consume(TokenType::RBRACKET, "Expected ')'");
        return expr;
    }

    Value leftVal = parseOperand();
    if (match(TokenType::BETWEEN)) {
        Value startVal = parseOperand();
        consume(TokenType::AND, "Expected AND in BETWEEN");
        Value endVal = parseOperand();
        return makeBetween(std::move(leftVal), std::move(startVal), std::move(endVal));
    } else if (match(TokenType::LIKE)) {
        std::string pattern = consume(TokenType::STRING_LITERAL, "Expected string pattern after LIKE").text;
        return makeLike(std::move(leftVal), std::move(pattern));
    } else if (check(TokenType::OP_EQUAL) || check(TokenType::OP_NOT_EQUAL) ||
               check(TokenType::OP_LESS) || check(TokenType::OP_GREATER) ||
               check(TokenType::OP_LESS_OR_EQUAL) || check(TokenType::OP_GREATER_OR_EQUAL)) {
        ComparisonOp op;
        TokenType t = currentToken.type;
        advance();
        switch (t) {
            case TokenType::OP_EQUAL: op = ComparisonOp::EQUAL; break;
            case TokenType::OP_NOT_EQUAL: op = ComparisonOp::NOT_EQUAL; break;
            case TokenType::OP_LESS: op = ComparisonOp::LESS; break;
            case TokenType::OP_GREATER: op = ComparisonOp::GREATER; break;
            case TokenType::OP_LESS_OR_EQUAL: op = ComparisonOp::LESS_OR_EQUAL; break;
            case TokenType::OP_GREATER_OR_EQUAL: op = ComparisonOp::GREATER_OR_EQUAL; break;
            default: throw std::runtime_error("Unexpected operator");
        }
        Value rightVal = parseOperand();
        return makeComparison(op, std::move(leftVal), std::move(rightVal));
    }
    throw std::runtime_error("Invalid expression after operand");
}

// Главный метод
Statement Parser::parseStatement() {
    if (check(TokenType::END_OF_FILE)) {
        throw std::runtime_error("Empty statement");
    }

    Statement stmt;
    if (check(TokenType::CREATE) || check(TokenType::DROP) || check(TokenType::USE)) {
        stmt = parseDDL();
    } else if (check(TokenType::INSERT) || check(TokenType::UPDATE) ||
               check(TokenType::DELETE) || check(TokenType::SELECT)) {
        stmt = parseDML();
    } else {
        throw std::runtime_error("Unexpected token: " + currentToken.text);
    }
    if (!check(TokenType::END_OF_FILE)) {
        throw std::runtime_error("Unexpected tokens after end of statement");
    }
    return stmt;
}