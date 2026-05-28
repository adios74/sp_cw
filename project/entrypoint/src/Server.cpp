#include "../include/Server.h"
#include "Network.h"
#include "Lexer.h"
#include "Parser.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>

Server::Server(int port, const std::string& db_root)
    : port_(port), db_root_(db_root), running_(false), server_fd_(-1) {
    if (!fs::exists(db_root)) {
        fs::create_directories(db_root);
    }
    access_logger_ = std::make_unique<AccessLogger>(db_root + "/access.log");
}

Server::~Server() { stop(); }

void Server::start() {
    dbms_ = std::make_unique<DBMS>(db_root_);
    Socket listener;
    listener.bind(port_);
    listener.listen();
    server_fd_ = listener.getFd();
    running_ = true;
    std::cout << "Server listening on port " << port_ << std::endl;
    std::cout << "Access log: " << db_root_ + "/access.log" << std::endl;

    while (running_) {
        try {
            std::cerr << "[Storage] DEBUG: Waiting for accept on port " << port_ << "..." << std::endl;
            int client_fd = listener.accept();
            std::cerr << "[Storage] DEBUG: Accepted connection, fd=" << client_fd << std::endl;
            client_threads_.emplace_back(&Server::handleClient, this, client_fd);
        } catch (const std::exception& e) {
            if (running_) std::cerr << "Accept error: " << e.what() << std::endl;
        }
    }
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
    
    std::cerr << "[Storage] New connection, fd=" << client_fd << std::endl;
    
    SQLExecutor executor(*dbms_);
    std::string buffer;
    std::string client_id = "client_" + std::to_string(client_fd);

    while (true) {
        try {
            std::string request = client.recv();
            if (request.empty()) break;
            buffer += request;
            size_t pos;
            while ((pos = buffer.find(';')) != std::string::npos) {
                std::string command = buffer.substr(0, pos + 1);
                buffer.erase(0, pos + 1);
                command.erase(0, command.find_first_not_of(" \t\n\r"));
                command.erase(command.find_last_not_of(" \t\n\r") + 1);
                if (command.empty()) continue;

                // Начало замера и телеметрии
                telemetry_.recordQueryStart();
                auto start = std::chrono::system_clock::now();
                auto exec_start = TelemetryCollector::Clock::now();

                std::stringstream out;
                auto old_cout = std::cout.rdbuf(out.rdbuf());
                auto old_cerr = std::cerr.rdbuf(out.rdbuf());  // ловим и ошибки

                bool success = true;
                std::string error_msg;
                try {
                    Lexer lexer(command);
                    Parser parser(lexer);
                    Statement stmt = parser.parseStatement();
                    executor.execute(stmt);
                } catch (const std::exception& ex) {
                    success = false;
                    error_msg = ex.what();
                    out << "Error: " << ex.what() << std::endl;
                }

                std::cout.rdbuf(old_cout);
                std::cerr.rdbuf(old_cerr);

                auto exec_end = TelemetryCollector::Clock::now();
                telemetry_.recordQueryEnd(exec_end - exec_start, !success);

                auto end = std::chrono::system_clock::now();
                // Логирование запроса
                access_logger_->logRequest(command, client_id, "handler_" + std::to_string(client_fd),
                                           start, end, success, error_msg);

                // Формируем ответ: результат + метрики
                std::string response = out.str();
                nlohmann::json metrics = telemetry_.getMetricsJson();
                response += "\nMETRICS: " + metrics.dump(4);   // автоматически после каждого запроса

                client.send(response);
            }
        } catch (const std::exception& ex) {
            out << "Error: " << ex.what() << std::endl;
        }
        
        std::cout.rdbuf(old_cout);
        accumulated_output += out.str();
    }
    
    std::cerr << "[Storage] Sending response: " << accumulated_output << std::endl;
    client.send(accumulated_output);
    std::cerr << "[Storage] Response sent, closing connection" << std::endl;
}