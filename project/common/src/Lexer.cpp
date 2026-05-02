#include "../include/Lexer.h"
#include <cctype>
#include <unordered_map>

Lexer::Lexer(const std::string& input)
    : source(input), pos(0), line(1), column(1), peeked(false)
{}

Token Lexer::nextToken() {
    if (peeked) {
        peeked = false;
        return currentToken;
    }
    skipWhitespace();
    if (pos >= source.size()) {
        return Token(TokenType::END_OF_FILE, "", line, column);
    }

    char c = source[pos];

    if (c == '"') {
        return readString();
    }

    if (isLetter(c) || c == '_') {
        return readWordOrKeyword();
    }

    if (isDigit(c)) {
        return readNumber();
    }

    return readOperatorOrPunctuation();
}

Token Lexer::peekToken() {
    if (!peeked) {
        currentToken = nextToken();
        peeked = true;
    }
    return currentToken;
}

void Lexer::skipWhitespace() {
    while (pos < source.size() && (source[pos] == ' ' || source[pos] == '\t' ||
                                    source[pos] == '\n' || source[pos] == '\r')) {
        if (source[pos] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
        ++pos;
    }
}

bool Lexer::isLetter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool Lexer::isDigit(char c) {
    return c >= '0' && c <= '9';
}

bool Lexer::isIdentChar(char c) {
    return isLetter(c) || isDigit(c) || c == '_';
}

bool Lexer::hasMixedCase(const std::string& word) {
    bool hasUpper = false;
    bool hasLower = false;

    for (char c : word) {
        if (::isupper(static_cast<unsigned char>(c))) {
            hasUpper = true;
        } else if (::islower(static_cast<unsigned char>(c))) {
            hasLower = true;
        }
    }
    return hasUpper && hasLower;
}

Token Lexer::readWordOrKeyword() {
    size_t start = pos;
    int startCol = column;

    while (pos < source.size() && isIdentChar(source[pos])) {
        ++pos;
        ++column;
    }

    std::string word = source.substr(start, pos - start);

    if (isDigit(word[0])) {
        throw std::runtime_error("Lexer error: identifier cannot start with digit at line " +
                                 std::to_string(line) + ", column " + std::to_string(startCol));
    }

    static const std::unordered_map<std::string, TokenType> keywords = {
        {"CREATE", TokenType::CREATE},
        {"DATABASE", TokenType::DATABASE},
        {"DROP", TokenType::DROP},
        {"USE", TokenType::USE},
        {"TABLE", TokenType::TABLE},
        {"INSERT", TokenType::INSERT},
        {"INTO", TokenType::INTO},
        {"VALUE", TokenType::VALUE},
        {"SELECT", TokenType::SELECT},
        {"FROM", TokenType::FROM},
        {"WHERE", TokenType::WHERE},
        {"UPDATE", TokenType::UPDATE},
        {"SET", TokenType::SET},
        {"DELETE", TokenType::DELETE},
        {"LIKE", TokenType::LIKE},
        {"BETWEEN", TokenType::BETWEEN},
        {"AND", TokenType::AND},
        {"OR", TokenType::OR},
        {"NOT", TokenType::NOT},
        {"NULL", TokenType::NULL_VAL},
        {"INDEXED", TokenType::INDEXED},
        {"NOT_NULL", TokenType::NOT_NULL},
        {"DEFAULT", TokenType::DEFAULT},
        {"AS", TokenType::AS},
        {"REVERT", TokenType::REVERT},
        {"SUM", TokenType::SUM},
        {"COUNT", TokenType::COUNT},
        {"AVG", TokenType::AVG},
        {"INT", TokenType::INT_TYPE},
        {"STRING", TokenType::STRING_TYPE}
    };

    std::string upperWord;
    upperWord.reserve(word.size());
    for (char c : word) {
        upperWord.push_back(static_cast<char>(::toupper(static_cast<unsigned char>(c))));
    }

    auto it = keywords.find(upperWord);
    if (it != keywords.end()) {
        if (hasMixedCase(word)) {
            throw std::runtime_error("Lexer error: mixed case is not allowed in word '" + word +
                                    "' at line " + std::to_string(line) + ", column " + std::to_string(startCol));
        }
        return Token(it->second, word, line, startCol);
    } else {
        return Token(TokenType::IDENTIFIER, word, line, startCol);
    }
}

Token Lexer::readNumber() {
    size_t start = pos;
    int startCol = column;
    while (pos < source.size() && isDigit(source[pos])) {
        ++pos;
        ++column;
    }
    std::string number = source.substr(start, pos - start);
    return Token(TokenType::INT_LITERAL, number, line, startCol);
}

Token Lexer::readString() {

    if (source[pos] != '"') {
        throw std::runtime_error("Lexer internal error: expected opening quote");
    }
    int startLine = line;
    int startCol = column;
    ++pos;
    ++column;
    std::string value;

    while (pos < source.size()) {

        char c = source[pos];

        if (c == '"') {
            ++pos;
            ++column;
            return Token(TokenType::STRING_LITERAL, value, startLine, startCol);

        } else if (c == '\\') {
            ++pos;
            ++column;
            if (pos >= source.size()) {
                throw std::runtime_error("Lexer error: unexpected end of input after escape at line " +
                                         std::to_string(line) + ", column " + std::to_string(column));
            }
            char escaped = source[pos];
            ++pos;
            ++column;
            switch (escaped) {
                case '"':  {
                    value += '"'; 
                    break;
                }
                case '\\': {
                    value += '\\'; 
                    break;
                }
                case 'n':  {
                    value += '\n'; 
                    break;
                }
                case 't':  {
                    value += '\t'; 
                    break;
                }
                default:
                    throw std::runtime_error("Lexer error: unknown escape sequence '\\" +
                                             std::string(1, escaped) + "' at line " +
                                             std::to_string(line) + ", column " + std::to_string(column-1));
            }
        } else {
            value += c;
            ++pos;
            ++column;

            if (c == '\n') {
                ++line;
                column = 1;
            }
        }
    }
    throw std::runtime_error("Lexer error: unterminated string literal starting at line " +
                             std::to_string(startLine) + ", column " + std::to_string(startCol));
}

Token Lexer::readOperatorOrPunctuation() {
    char c = source[pos];
    int startLine = line;
    int startCol = column;
    std::string opStr(1, c);
    ++pos;
    ++column;

    if (c == '=') {

        if (pos < source.size() && source[pos] == '=') {
            opStr += '=';
            ++pos;
            ++column;
            return Token(TokenType::OP_EQUAL, opStr, startLine, startCol);

        } else {
            throw std::runtime_error("Lexer error: unexpected '=' at line " +
                                     std::to_string(line) + ", column " + std::to_string(column-1) +
                                     " (did you mean '=='?)");
        }
    } else if (c == '!') {

        if (pos < source.size() && source[pos] == '=') {
            opStr += '=';
            ++pos;
            ++column;
            return Token(TokenType::OP_NOT_EQUAL, opStr, startLine, startCol);

        } else {
            throw std::runtime_error("Lexer error: unexpected '!' at line " +
                                     std::to_string(line) + ", column " + std::to_string(column-1) +
                                     " (did you mean '!='?)");
        }

    } else if (c == '<') {

        if (pos < source.size() && source[pos] == '=') {
            opStr += '=';
            ++pos;
            ++column;
            return Token(TokenType::OP_LESS_OR_EQUAL, opStr, startLine, startCol);

        } else {
            return Token(TokenType::OP_LESS, opStr, startLine, startCol);
        }

    } else if (c == '>') {

        if (pos < source.size() && source[pos] == '=') {
            opStr += '=';
            ++pos;
            ++column;
            return Token(TokenType::OP_GREATER_OR_EQUAL, opStr, startLine, startCol);

        } else {
            return Token(TokenType::OP_GREATER, opStr, startLine, startCol);
        }

    } else if (c == ',') {
        return Token(TokenType::COMMA, opStr, startLine, startCol);

    } else if (c == ';') {
        return Token(TokenType::SEMICOLON, opStr, startLine, startCol);

    } else if (c == '(') {
        return Token(TokenType::LBRACKET, opStr, startLine, startCol);

    } else if (c == ')') {
        return Token(TokenType::RBRACKET, opStr, startLine, startCol);

    } else if (c == '*') {
        return Token(TokenType::STAR, opStr, startLine, startCol);

    } else {
        throw std::runtime_error("Lexer error: unexpected character '" + std::string(1, c) +
                                 "' at line " + std::to_string(line) + ", column " + std::to_string(startCol));
    }
}