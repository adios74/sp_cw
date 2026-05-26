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
#include "../common/include/Network.h"
#include "../entrypoint/include/Server.h"

void printHelp() {
    std::cout << "\n========================================\n";
    std::cout << "   SQL Database Engine (Client-Server)\n";
    std::cout << "========================================\n";
    std::cout << "Server mode:\n";
    std::cout << "  " << "storage_client --server [port]\n";
    std::cout << "Client mode (connects to localhost:12345 by default):\n";
    std::cout << "  " << "storage_client                 - interactive\n";
    std::cout << "  " << "storage_client script.sql      - batch mode\n";
    std::cout << "  " << "storage_client host:port       - connect to specific server\n";
    std::cout << "  " << "storage_client script.sql host:port\n";
    std::cout << "========================================\n\n";
//
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
    std::cout << "  REVERT <table> \"yyyy.mm.dd-hh:mm:ss.msmsms]\";\n";
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

// Функция для отправки одной команды на сервер и вывода ответа
void sendCommand(Socket& client, const std::string& cmd) {
    if (cmd.empty()) return;
    client.send(cmd + ";");
    std::string response = client.recv();
    std::cout << response;
}

int main(int argc, char* argv[]) {
    // Проверка на --help
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h") {
            printHelp();
            return 0;
        }
    }

    // --- Режим сервера ---
    if (argc >= 2 && std::string(argv[1]) == "--server") {
        int port = 12345;
        if (argc >= 3) port = std::stoi(argv[2]);
        Server server(port, "./database_data");
        server.start();
        return 0;
    }

    // --- Режим клиента ---
    // Параметры по умолчанию
    std::string host = "127.0.0.1";
    int port = 12345;
    std::string scriptFile;

    // Разбор аргументов клиента
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.find(':') != std::string::npos) {
            // Формат host:port
            host = arg.substr(0, arg.find(':'));
            port = std::stoi(arg.substr(arg.find(':')+1));
        } else {
            // Имя файла
            scriptFile = arg;
        }
    }

    // Подключение к серверу
    Socket client;
    try {
        client.connect(host, port);
        std::cout << "Connected to " << host << ":" << port << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Cannot connect to server: " << e.what() << std::endl;
        return 1;
    }

    // Выбор источника команд
    std::ifstream file;
    std::istream* input = &std::cin;
    bool interactive = false;

    if (!scriptFile.empty()) {
        file.open(scriptFile);
        if (!file.is_open()) {
            std::cerr << "Error: Cannot open file '" << scriptFile << "'\n";
            return 1;
        }
        input = &file;
        interactive = false;
        std::cout << "Batch mode: executing commands from '" << scriptFile << "'\n\n";
    } else {
        interactive = true;
        std::cout << "Interactive mode. Enter SQL commands (end with ';'):\n";
    }

    CommandReader reader(*input, interactive);
    int commandCount = 0;

    while (auto command = reader.nextCommand()) {
        std::string sql = *command;
        if (sql.empty()) continue;

        // Команда выхода (только в интерактивном режиме)
        if (interactive && (sql == "EXIT" || sql == "exit" || sql == "QUIT" || sql == "quit")) {
            std::cout << "Goodbye!\n";
            break;
        }

        if (interactive) {
            std::cout << "\n[" << ++commandCount << "] > " << sql << "\n---\n";
        }

        try {
            sendCommand(client, sql);
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
            break;
        }

        if (interactive) std::cout << std::endl;
    }

    if (!interactive && file.is_open()) {
        std::cout << "\nBatch execution completed. " << commandCount << " commands processed.\n";
    }

    return 0;
}