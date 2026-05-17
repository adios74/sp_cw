#ifndef LEXER_H
#define LEXER_H

#include <string>
#include <stdexcept>

enum class TokenType {
    CREATE,
    DATABASE,
    DROP,
    USE,
    TABLE,
    INSERT,
    INTO,
    VALUE,
    SELECT,
    FROM,
    WHERE,
    UPDATE,
    SET,
    DELETE,
    LIKE,
    BETWEEN,
    AND,
    OR,
    NOT,
    NULL_VAL,        // NULL
    INDEXED,
    NOT_NULL,
    DEFAULT,
    AS,
    REVERT,

    SUM,
    COUNT,
    AVG,
    INT_TYPE,        // INT
    STRING_TYPE,     // STRING

    OP_EQUAL,           // ==
    OP_NOT_EQUAL,           // !=
    OP_LESS,           // <
    OP_GREATER,           // >
    OP_LESS_OR_EQUAL,           // <=
    OP_GREATER_OR_EQUAL,           // >=
    OP_ASSIGN,          // =

    COMMA,           // ,
    SEMICOLON,       // ;
    LBRACKET,          // (
    RBRACKET,          // )
    STAR,            // *
    DOT,

    IDENTIFIER,
    INT_LITERAL,
    STRING_LITERAL,
    END_OF_FILE
};

struct Token {
    TokenType type;
    std::string text;
    int line;
    int column;

    Token() : type(TokenType::END_OF_FILE), text(""), line(0), column(0) {}

    Token(TokenType type, const std::string& text, int line, int col)
        : type(type), text(text), line(line), column(col) {}
};

class Lexer {
public:
    explicit Lexer(const std::string& input);
    Token nextToken();
    Token peekToken();

private:
    std::string source;
    size_t pos;
    int line, column;
    Token currentToken;
    bool peeked;

    void skipWhitespace();
    Token readWordOrKeyword();
    Token readNumber();
    Token readString();
    Token readOperatorOrPunctuation();

    static bool hasMixedCase(const std::string& word);

    static bool isLetter(char c);
    static bool isDigit(char c);
    static bool isIdentChar(char c);
};

#endif