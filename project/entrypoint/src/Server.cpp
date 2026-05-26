#include "../include/Server.h"
#include "Network.h"
#include "Lexer.h"
#include "Parser.h"
#include <iostream>
#include <sstream>

Server::Server(int port, const std::string& db_root)
    : port_(port), db_root_(db_root), running_(false), server_fd_(-1) {}

Server::~Server() { stop(); }

void Server::start() {
    dbms_ = std::make_unique<DBMS>(db_root_);
    Socket listener;
    listener.bind(port_);
    listener.listen();
    server_fd_ = listener.getFd(); // сохраняем для возможности остановки
    running_ = true;
    std::cout << "Server listening on port " << port_ << std::endl;

    while (running_) {
        try {
            int client_fd = listener.accept();
            client_threads_.emplace_back(&Server::handleClient, this, client_fd);
        } catch (const std::exception& e) {
            if (running_) std::cerr << "Accept error: " << e.what() << std::endl;
        }
    }
    // Ждём завершения всех клиентских потоков
    for (auto& t : client_threads_) {
        if (t.joinable()) t.join();
    }
}

void Server::stop() {
    running_ = false;
    if (server_fd_ != -1) {
        ::close(server_fd_);
        server_fd_ = -1;
    }
}

void Server::handleClient(int client_fd) {
    Socket client;
    client.setFd(client_fd);
    SQLExecutor executor(*dbms_);
    std::string buffer;

    while (true) {
        try {
            std::string request = client.recv();
            if (request.empty()) break;
            // Простой протокол: каждая команда заканчивается ';'
            buffer += request;
            size_t pos;
            while ((pos = buffer.find(';')) != std::string::npos) {
                std::string command = buffer.substr(0, pos + 1);
                buffer.erase(0, pos + 1);
                // Убираем лишние пробелы
                command.erase(0, command.find_first_not_of(" \t\n\r"));
                command.erase(command.find_last_not_of(" \t\n\r") + 1);
                if (command.empty()) continue;

                // Перенаправляем вывод в строку
                std::stringstream out;
                auto old_cout = std::cout.rdbuf(out.rdbuf());

                try {
                    Lexer lexer(command);
                    Parser parser(lexer);
                    Statement stmt = parser.parseStatement();
                    executor.execute(stmt);
                } catch (const std::exception& ex) {
                    out << "Error: " << ex.what() << std::endl;
                }

                std::cout.rdbuf(old_cout);
                client.send(out.str());
            }
        } catch (const std::exception& e) {
            std::cerr << "Client handler error: " << e.what() << std::endl;
            break;
        }
    }
    client.close();
}