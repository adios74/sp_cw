#include "../include/Server.h"
#include "Network.h"
#include "Lexer.h"
#include "Parser.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>

Server::Server(int port, const std::string& db_root)
    : port_(port), db_root_(db_root), running_(false), server_fd_(-1) {}

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

std::string Server::executeSelect(SelectStmt& stmt) {
    auto db = dbms_->currentDatabase();
    if (!db) return "Error: No database selected\n";
    auto table = db->getTable(stmt.table.name);
    if (!table) return "Error: Table not found\n";

    auto rows = table->selectRows(stmt.condition.get());
    nlohmann::json result = nlohmann::json::array();
    for (const auto& row : rows) {
        nlohmann::json obj;
        for (size_t i = 0; i < row.size(); ++i) {
            const auto& col = table->metadata().columns[i];
            if (row[i].is_null) {
                obj[col.name] = nullptr;
            } else if (row[i].type == DataType::INT) {
                obj[col.name] = row[i].ival;
            } else {
                obj[col.name] = row[i].getString();
            }
        }
        result.push_back(obj);
    }
    return result.dump() + "\n";
}

void Server::handleClient(int client_fd) {
    Socket client;
    client.setFd(client_fd);

    std::cerr << "[Storage] New connection, fd=" << client_fd << std::endl;

    std::string data = client.recv();
    if (data.empty()) {
        std::cerr << "[Storage] No data received or connection closed" << std::endl;
        return;
    }
    std::cerr << "[Storage] Received " << data.size() << " bytes: " << data << std::endl;

    std::string buffer = data;
    std::string accumulated_output;

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

        std::string upper = command_no_semicolon;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

        std::string response;

        try {
            if (upper.find("USE ") == 0) {
                std::string db = command_no_semicolon.substr(4);
                db.erase(0, db.find_first_not_of(" \t"));
                if (dbms_->useDatabase(db))
                    response = "Using database " + db + "\n";
                else
                    response = "Error: Database '" + db + "' not found\n";
            }
            else if (upper.find("CREATE DATABASE") == 0) {
                Lexer lexer(command_no_semicolon);
                Parser parser(lexer);
                CreateDatabaseStmt stmt = std::get<CreateDatabaseStmt>(parser.parseStatement());
                bool ok = dbms_->createDatabase(stmt.name);
                response = ok ? "Database created\n" : "Failed to create database\n";
            }
            else if (upper.find("DROP DATABASE") == 0) {
                Lexer lexer(command_no_semicolon);
                Parser parser(lexer);
                DropDatabaseStmt stmt = std::get<DropDatabaseStmt>(parser.parseStatement());
                bool ok = dbms_->dropDatabase(stmt.name);
                response = ok ? "Database dropped\n" : "Failed to drop database\n";
            }
            else if (upper.find("CREATE TABLE") == 0) {
                Lexer lexer(command_no_semicolon);
                Parser parser(lexer);
                CreateTableStmt stmt = std::get<CreateTableStmt>(parser.parseStatement());
                auto db = dbms_->currentDatabase();
                if (!db) response = "Error: No database selected\n";
                else {
                    bool ok = db->createTable(stmt);
                    response = ok ? "Table created\n" : "Failed to create table\n";
                }
            }
            else if (upper.find("DROP TABLE") == 0) {
                Lexer lexer(command_no_semicolon);
                Parser parser(lexer);
                DropTableStmt stmt = std::get<DropTableStmt>(parser.parseStatement());
                auto db = dbms_->currentDatabase();
                if (!db) response = "Error: No database selected\n";
                else {
                    bool ok = db->dropTable(stmt.table.name);
                    response = ok ? "Table dropped\n" : "Failed to drop table\n";
                }
            }
            else if (upper.find("INSERT") == 0) {
                Lexer lexer(command_no_semicolon);
                Parser parser(lexer);
                InsertStmt stmt = std::get<InsertStmt>(parser.parseStatement());
                auto db = dbms_->currentDatabase();
                if (!db) response = "Error: No database selected\n";
                else {
                    auto table = db->getTable(stmt.table.name);
                    if (!table) response = "Error: Table not found\n";
                    else {
                        std::vector<std::vector<DBValue>> dbRows;
                        for (const auto& rowVals : stmt.values) {
                            std::vector<DBValue> dbRow;
                            for (const auto& val : rowVals) {
                                dbRow.push_back(Table::astToDBValue(val));
                            }
                            dbRows.push_back(std::move(dbRow));
                        }
                        size_t inserted = 0;
                        bool error = false;
                        for (size_t i = 0; i < dbRows.size(); ++i) {
                            auto [ok, err] = table->insertRow(dbRows[i]);
                            if (ok) inserted++;
                            else {
                                if (err == Table::InsertError::DUPLICATE_KEY)
                                    response = "Error: Duplicate key\n";
                                else if (err == Table::InsertError::NOT_NULL_VIOLATION)
                                    response = "Error: NOT NULL constraint failed\n";
                                else if (err == Table::InsertError::TYPE_MISMATCH)
                                    response = "Error: Type mismatch\n";
                                else
                                    response = "Error: Insert failed\n";
                                error = true;
                                break;
                            }
                        }
                        if (!error)
                            response = std::to_string(inserted) + " row(s) inserted\n";
                    }
                }
            }
            else if (upper.find("UPDATE") == 0) {
                Lexer lexer(command_no_semicolon);
                Parser parser(lexer);
                UpdateStmt stmt = std::get<UpdateStmt>(parser.parseStatement());
                auto db = dbms_->currentDatabase();
                if (!db) response = "Error: No database selected\n";
                else {
                    auto table = db->getTable(stmt.table.name);
                    if (!table) response = "Error: Table not found\n";
                    else {
                        std::vector<std::pair<std::string, DBValue>> assignments;
                        for (const auto& [col, val] : stmt.assignments) {
                            assignments.emplace_back(col, Table::astToDBValue(val));
                        }
                        size_t updated = table->updateRows(assignments, stmt.condition.get());
                        response = std::to_string(updated) + " row(s) updated\n";
                    }
                }
            }
            else if (upper.find("DELETE") == 0) {
                Lexer lexer(command_no_semicolon);
                Parser parser(lexer);
                DeleteStmt stmt = std::get<DeleteStmt>(parser.parseStatement());
                auto db = dbms_->currentDatabase();
                if (!db) response = "Error: No database selected\n";
                else {
                    auto table = db->getTable(stmt.table.name);
                    if (!table) response = "Error: Table not found\n";
                    else {
                        size_t deleted = table->deleteRows(stmt.condition.get());
                        response = std::to_string(deleted) + " row(s) deleted\n";
                    }
                }
            }
            else if (upper.find("SELECT") == 0) {
                Lexer lexer(command_no_semicolon);
                Parser parser(lexer);
                SelectStmt stmt = std::get<SelectStmt>(parser.parseStatement());
                response = executeSelect(stmt);
            }
            else if (upper.find("SHOW STORAGES") == 0) {
                // Эта команда обрабатывается Entrypoint, до Storage не доходит
                response = "Unknown command (should be handled by Entrypoint)\n";
            }
            else {
                response = "Unknown command\n";
            }
        } catch (const std::exception& ex) {
            response = "Error: " + std::string(ex.what()) + "\n";
        }

        accumulated_output += response;
    }

    if (accumulated_output.empty())
        accumulated_output = "OK\n";
    client.send(accumulated_output);

    std::cerr << "[Storage] Sending response: " << accumulated_output << std::endl;
    std::cerr << "[Storage] Response sent, closing connection" << std::endl;
}