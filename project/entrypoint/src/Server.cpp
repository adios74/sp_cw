#include "../include/Server.h"
#include "Network.h"
#include "Lexer.h"
#include "Parser.h"
#include "../../storage_node/include/DbEngine.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;

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
    std::cout << "Storage server listening on port " << port_ << std::endl;
    std::cerr << "[Storage] DEBUG: Server started on port " << port_ << ", fd=" << server_fd_ << std::endl;

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
    SQLExecutor executor(*dbms_);
    std::string buffer;
    std::string client_id = "client_" + std::to_string(client_fd);

    std::cerr << "[Storage] New connection, fd=" << client_fd << std::endl;

    while (true) {
        try {
            std::cerr << "[Storage] Waiting for data..." << std::endl;
            std::string request = client.recv();
            if (request.empty()) {
                std::cerr << "[Storage] No data received or connection closed" << std::endl;
                break;
            }
            std::cerr << "[Storage] Received " << request.size() << " bytes: " << request << std::endl;

            buffer += request;
            size_t pos;
            while ((pos = buffer.find(';')) != std::string::npos) {
                std::string command = buffer.substr(0, pos + 1);
                buffer.erase(0, pos + 1);
                command.erase(0, command.find_first_not_of(" \t\n\r"));
                command.erase(command.find_last_not_of(" \t\n\r") + 1);
                if (command.empty()) continue;

                std::string command_no_semicolon = command;
                if (!command_no_semicolon.empty() && command_no_semicolon.back() == ';')
                    command_no_semicolon.pop_back();

                std::cerr << "[Storage] Processing command: " << command_no_semicolon << std::endl;

                // Начало замера и телеметрии
                telemetry_.recordQueryStart();
                auto start = std::chrono::system_clock::now();
                auto exec_start = TelemetryCollector::Clock::now();

                std::stringstream out;
                auto old_cout = std::cout.rdbuf(out.rdbuf());
                auto old_cerr = std::cerr.rdbuf(out.rdbuf());

                bool success = true;
                std::string error_msg;
                std::string upper;
                bool is_use = false;

                try {
                    upper = command_no_semicolon;
                    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

                    if (upper.find("USE ") == 0) {
                        is_use = true;
                        std::string db = command_no_semicolon.substr(4);
                        db.erase(0, db.find_first_not_of(" \t"));
                        if (dbms_->useDatabase(db))
                            out << "Using database " << db << std::endl;
                        else
                            out << "Error: Database '" << db << "' not found" << std::endl;
                    } else {
                        Lexer lexer(command_no_semicolon);
                        Parser parser(lexer);
                        Statement stmt = parser.parseStatement();
                        executor.execute(stmt);
                    }
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
                access_logger_->logRequest(command_no_semicolon, client_id,
                                           "handler_" + std::to_string(client_fd),
                                           start, end, success, error_msg);

                std::string response = out.str();
                if (!is_use) {
                    nlohmann::json metrics = telemetry_.getMetricsJson();
                    std::ofstream metrics_file(db_root_ + "/metrics.log", std::ios::app);
                    if (metrics_file.is_open()) {
                        metrics_file << metrics.dump() << std::endl;
                    } else {
                        std::cerr << "[Storage] Failed to open metrics log file" << std::endl;
                    }
                }

                std::cerr << "[Storage] Sending response: " << response << std::endl;
                client.send(response);
            }
        } catch (const std::exception& e) {
            std::cerr << "Client handler error: " << e.what() << std::endl;
            break;
        }
    }
    std::cerr << "[Storage] Closing connection, fd=" << client_fd << std::endl;
    client.close();
}