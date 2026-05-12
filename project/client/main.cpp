#include <iostream>
#include <fstream>
#include "Text handler/include/CommandReader.h"
#include "../common/include/AST.h"
#include "../common/include/Lexer.h"
#include "../common/include/Parser.h"
#include "../common/include/Json.h"

int main(int argc, char* argv[]) {
    std::ifstream file;           // владеет файлом, если открыт
    std::istream* input = nullptr; // невладеющий указатель
    bool interactive = false;

    if (argc == 1) {
        input = &std::cin;
        interactive = true;
        std::cout << "Interactive mode. Type SQL commands terminated by ';'." << std::endl;
    } else if (argc == 2) {
        file.open(argv[1]);
        if (!file.is_open()) {
            std::cerr << "Error: cannot open file " << argv[1] << std::endl;
            return 1;
        }
        input = &file;
        std::cout << "Batch mode. Executing commands from " << argv[1] << std::endl;
    } else {
        std::cerr << "Usage: " << argv[0] << " [script_file]" << std::endl;
        return 1;
    }

    CommandReader reader(*input, interactive);

    while (auto command = reader.nextCommand()) {
        std::string sql = *command;

        if (sql == "EXIT" || sql == "exit") {
            break;
        }

        try {
            Lexer lexer(sql);
            Parser parser(lexer);
            Statement stmt = parser.parseStatement();
            std::cout << "Parsed: " << astToString(stmt) << std::endl;
        } catch (const std::exception& ex) {
            std::cerr << "Error: " << ex.what() << std::endl;
        }
    }
    
    // Тестовый код для JSON форматера
    TableSchema schema;
    schema.columnNames = {"id", "name", "age"};

    SelectStmt stmt;
    stmt.table.name = "students";
    // SELECT name, age FROM students WHERE ... (условие не важно)
    SelectColumn col1;
    col1.expr = ColumnRef{"name", "", ""};  // Исправлено: добавлены недостающие поля
    stmt.columns.push_back(col1);

    SelectColumn col2;
    col2.expr = ColumnRef{"age", "", ""};   // Исправлено: добавлены недостающие поля
    stmt.columns.push_back(col2);

    // Фиктивные строки - ИСПРАВЛЕНО: убран nullptr
    std::vector<Row> testRows = {
        {Value(1), Value(std::string("Alice")), Value(20)},
        {Value(2), Value(std::string("Bob")), Value(22)},
        {Value(3), Value(std::string("Unknown")), Value(19)}  // Вместо nullptr используем строку "Unknown"
    };

    std::string jsonResult = formatSelectResult(stmt, testRows, schema);
    std::cout << "JSON: " << jsonResult << std::endl;

    return 0;
}