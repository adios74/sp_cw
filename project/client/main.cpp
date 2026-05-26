#include <iostream>
#include <fstream>
#include <string>
#include <memory>
#include "Text handler/include/CommandReader.h"
#include "../common/include/AST.h"
#include "../common/include/Lexer.h"
#include "../common/include/Parser.h"
//#include "../common/include/Json.h"
#include "DbEngine.h"

void printHelp() {
    std::cout << "\n========================================\n";
    std::cout << "   Simple SQL Database Engine\n";
    std::cout << "========================================\n";
    std::cout << "Supported commands:\n";
    std::cout << "  CREATE DATABASE <name>;\n";
    std::cout << "  DROP DATABASE <name>;\n";
    std::cout << "  USE <database>;\n";
    std::cout << "  CREATE TABLE <name> (<col1> TYPE, <col2> TYPE, ...);\n";
    std::cout << "  DROP TABLE <name>;\n";
    std::cout << "  INSERT INTO <table> VALUES (val1, val2, ...);\n";
    std::cout << "  SELECT * FROM <table> [WHERE condition];\n";
    std::cout << "  UPDATE <table> SET col = value [WHERE condition];\n";
    std::cout << "  DELETE FROM <table> [WHERE condition];\n";
    std::cout << "  EXIT; - quit the program\n";
    std::cout << "========================================\n\n";
}

void executeCommand(const std::string& sql, SQLExecutor& executor) {
    if (sql.empty()) return;
    
    try {
        Lexer lexer(sql);
        Parser parser(lexer);
        Statement stmt = parser.parseStatement();
        executor.execute(stmt);
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << std::endl;
    }
}

int main(int argc, char* argv[]) {
    // Инициализация ядра БД
    DBMS dbms("./database_data");
    SQLExecutor executor(dbms);
    
    std::ifstream file;
    std::istream* input = nullptr;
    bool interactive = false;
    
    // Обработка аргументов командной строки
    if (argc == 1) {
        // Интерактивный режим
        input = &std::cin;
        interactive = true;
        printHelp();
        std::cout << "Interactive mode activated. Enter SQL commands (end with ';'):\n\n";
    } 
    else if (argc == 2) {
        // Пакетный режим - чтение из файла
        std::string filename = argv[1];
        if (filename == "--help" || filename == "-h") {
            printHelp();
            return 0;
        }
        
        file.open(filename);
        if (!file.is_open()) {
            std::cerr << "Error: Cannot open file '" << filename << "'\n";
            return 1;
        }
        input = &file;
        std::cout << "Batch mode: Executing commands from '" << filename << "'\n\n";
    } 
    else {
        std::cerr << "Usage:\n";
        std::cerr << "  " << argv[0] << "              - Interactive mode\n";
        std::cerr << "  " << argv[0] << " <file.sql>   - Execute commands from file\n";
        std::cerr << "  " << argv[0] << " --help       - Show this help\n";
        return 1;
    }
    
    CommandReader reader(*input, interactive);
    int commandCount = 0;
    
    while (auto command = reader.nextCommand()) {
        std::string sql = *command;
        
        // Обработка выхода
        if (sql == "EXIT" || sql == "exit" || sql == "QUIT" || sql == "quit") {
            std::cout << "Goodbye!\n";
            break;
        }
        
        // Пропуск пустых команд
        if (sql.empty()) continue;
        
        commandCount++;
        if (interactive) {
            std::cout << "\n[" << commandCount << "] > " << sql << "\n";
            std::cout << "---\n";
        }
        
        executeCommand(sql, executor);
        
        if (interactive) {
            std::cout << std::endl;
        }
    }
    
    if (!interactive && file.is_open()) {
        std::cout << "\nBatch execution completed. " << commandCount << " commands processed.\n";
    }
    
    return 0;
}