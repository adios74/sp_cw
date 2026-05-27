#include "Entrypoint.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <regex>

Entrypoint::Entrypoint(int client_port)
    : client_port_(client_port), server_fd_(-1), running_(false) {}

Entrypoint::~Entrypoint() { stop(); }

void Entrypoint::start() {
    Socket listener;
    listener.bind(client_port_);
    listener.listen();
    server_fd_ = listener.getFd();
    running_ = true;
    std::cout << "Entrypoint listening for clients on port " << client_port_ << std::endl;

    while (running_) {
        try {
            int client_fd = listener.accept();
            client_threads_.emplace_back(&Entrypoint::handleClient, this, client_fd);
        } catch (const std::exception& e) {
            if (running_) std::cerr << "Accept error: " << e.what() << std::endl;
        }
    }
    for (auto& t : client_threads_) {
        if (t.joinable()) t.join();
    }
}

void Entrypoint::stop() {
    running_ = false;
    if (server_fd_ != -1) {
        ::close(server_fd_);
        server_fd_ = -1;
    }
}

void Entrypoint::addStorageNode(const std::string& host, int port) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& node : storage_nodes_) {
        if (node.first == host && node.second == port) {
            std::cerr << "Storage node " << host << ":" << port << " already registered" << std::endl;
            return;
        }
    }
    storage_nodes_.emplace_back(host, port);
    std::cout << "Storage node added: " << host << ":" << port << std::endl;
}

void Entrypoint::removeStorageNode(const std::string& host, int port) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find(storage_nodes_.begin(), storage_nodes_.end(),
                        std::make_pair(host, port));
    if (it == storage_nodes_.end()) {
        std::cerr << "Storage node not found: " << host << ":" << port << std::endl;
        return;
    }
    // Удаляем все БД, которые размещены на этом узле
    for (auto db_it = db_to_storage_.begin(); db_it != db_to_storage_.end(); ) {
        if (db_it->second == *it) {
            std::cout << "Database " << db_it->first << " will be inaccessible (storage removed)" << std::endl;
            db_it = db_to_storage_.erase(db_it);
        } else {
            ++db_it;
        }
    }
    storage_nodes_.erase(it);
    std::cout << "Storage node removed: " << host << ":" << port << std::endl;
}

std::string Entrypoint::forwardToStorage(const std::string& host, int port, const std::string& sql) {
    Socket sock;
    try {
        sock.setTimeout(2);   // таймаут на всякий случай
        sock.connect(host, port);
        sock.send(sql + ";");
        sock.shutdownWrite(); // сигнализируем конец передачи, сервер получит EOF
        std::string response = sock.recv(); // теперь recv прочитает ответ и завершится
        sock.close();
        return response;
    } catch (const std::exception& e) {
        return std::string("Error: storage node ") + host + ":" + std::to_string(port) + 
               " is unavailable (" + e.what() + ")\n";
    }
}

std::pair<std::string, int> Entrypoint::chooseStorageForNewDB() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (storage_nodes_.empty()) {
        throw std::runtime_error("No storage nodes available");
    }
    auto& node = storage_nodes_[next_node_index_ % storage_nodes_.size()];
    next_node_index_++;
    return node;
}

std::string Entrypoint::extractDatabaseName(const std::string& sql, const std::string& current_db) const {
    std::string upper = sql;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    size_t start = upper.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return current_db;
    upper = upper.substr(start);
    
    // CREATE DATABASE name
    if (upper.starts_with("CREATE DATABASE ") || upper.starts_with("CREATE DATABASE\n")) {
        size_t keyword_end = upper.find_first_not_of(" \t", 15); // после "CREATE DATABASE"
        if (keyword_end == std::string::npos) return "";
        size_t name_end = upper.find_first_of(" ;\t\n\r", keyword_end);
        if (name_end == std::string::npos) name_end = upper.size();
        return sql.substr(start + keyword_end, name_end - keyword_end);
    }
    
    // USE name
    if (upper.starts_with("USE ")) {
        size_t p = upper.find_first_of(" \t", 4);
        if (p != std::string::npos) {
            return sql.substr(start + 4, p - 4);
        }
        return sql.substr(start + 4);
    }
    
    // Ищем квалифицированные имена вида db.table
    std::regex db_table_regex(R"(\b([a-zA-Z_][a-zA-Z0-9_]*)\.([a-zA-Z_][a-zA-Z0-9_]*))");
    std::smatch match;
    if (std::regex_search(sql, match, db_table_regex)) {
        return match[1].str();
    }
    return current_db;
}

void Entrypoint::handleClient(int client_fd) {
    Socket client;
    client.setFd(client_fd);
    std::string buffer;
    std::string current_db;

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
                
                std::string upper_cmd = command;
                std::transform(upper_cmd.begin(), upper_cmd.end(), upper_cmd.begin(), ::toupper);
                
                // Административные команды
                if (upper_cmd.starts_with("ADD STORAGE ")) {
                    std::regex add_regex(R"(ADD STORAGE\s+\"([^\"]+):(\d+)\")");
                    std::smatch m;
                    if (std::regex_search(command, m, add_regex)) {
                        std::string host = m[1].str();
                        int port = std::stoi(m[2].str());
                        addStorageNode(host, port);
                        client.send("Storage node added\n");
                    } else {
                        client.send("Error: usage ADD STORAGE \"host:port\"\n");
                    }
                    continue;
                }
                if (upper_cmd.starts_with("REMOVE STORAGE ")) {
                    std::regex rem_regex(R"(REMOVE STORAGE\s+\"([^\"]+):(\d+)\")");
                    std::smatch m;
                    if (std::regex_search(command, m, rem_regex)) {
                        std::string host = m[1].str();
                        int port = std::stoi(m[2].str());
                        removeStorageNode(host, port);
                        client.send("Storage node removed\n");
                    } else {
                        client.send("Error: usage REMOVE STORAGE \"host:port\"\n");
                    }
                    continue;
                }
                if (upper_cmd.starts_with("SHOW STORAGES")) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    std::stringstream ss;
                    for (const auto& node : storage_nodes_)
                        ss << node.first << ":" << node.second << "\n";
                    client.send(ss.str());
                    continue;
                }

                // Определяем имя БД
                std::string db_name = extractDatabaseName(command, current_db);

                // CREATE DATABASE
                if (upper_cmd.find("CREATE DATABASE") == 0) {
                    if (db_name.empty()) {
                        client.send("Error: invalid database name\n");
                        continue;
                    }
                    std::pair<std::string, int> target;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (db_to_storage_.count(db_name)) {
                            client.send("Error: database already exists\n");
                            continue;
                        }
                        if (storage_nodes_.empty()) {
                            client.send("Error: no storage nodes available\n");
                            continue;
                        }
                        target = chooseStorageForNewDB();
                        db_to_storage_[db_name] = target;
                    }
                    std::string response = forwardToStorage(target.first, target.second, command);
                    client.send(response);
                    continue;
                }

                // USE
                if (upper_cmd.find("USE ") == 0) {
                    current_db = db_name;
                    client.send("Using database " + current_db + "\n");
                    continue;
                }

                // Для других команд БД должна быть определена и существовать
                if (db_name.empty()) {
                    client.send("Error: no database selected or specified\n");
                    continue;
                }

                std::pair<std::string, int> target;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto it = db_to_storage_.find(db_name);
                    if (it == db_to_storage_.end()) {
                        client.send("Error: database '" + db_name + "' not found\n");
                        continue;
                    }
                    target = it->second;
                }

                std::string response = forwardToStorage(target.first, target.second, command);
                client.send(response);
            }
        } catch (const std::exception& e) {
            std::cerr << "Client handler error: " << e.what() << std::endl;
            break;
        }
    }
    client.close();
}

