#ifndef PARSER_H
#define PARSER_H

#include "Lexer.h"
#include "AST.h"
#include <memory>

class Parser {
public:
    explicit Parser(Lexer& lexer);
    Statement parseStatement();

private:
    Lexer& lexer;
    Token currentToken;

    void advance();
    bool check(TokenType type) const;
    Token consume(TokenType type, const std::string& errorMsg);
    bool match(TokenType type);

    std::vector<std::string> parseQualifiedName();
    TableRef parseTableRef();
    ColumnRef parseColumnRef();
    Value parseLiteral();
    Value parseOperand();
    AggFunc parseAggFunc();

    Statement parseDDL();
    CreateDatabaseStmt parseCreateDatabase();
    DropDatabaseStmt parseDropDatabase();
    UseStmt parseUse();
    CreateTableStmt parseCreateTable();
    DropTableStmt parseDropTable();
    ColumnDef parseColumnDef();

    Statement parseDML();
    InsertStmt parseInsert();
    UpdateStmt parseUpdate();
    DeleteStmt parseDelete();
    SelectStmt parseSelect();
    RevertStmt parseRevert();

    SelectColumn parseSelectItem(); 

    std::unique_ptr<Expr> parseWhereClause();
    std::unique_ptr<Expr> parseExpression();
    std::unique_ptr<Expr> parseOrExpr();
    std::unique_ptr<Expr> parseAndExpr();
    std::unique_ptr<Expr> parsePrimaryExpr();
};

#endif