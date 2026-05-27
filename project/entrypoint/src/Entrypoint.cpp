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
    std::cerr << "[Entrypoint] ========== FORWARDING TO STORAGE ==========" << std::endl;
    std::cerr << "[Entrypoint] Host: " << host << std::endl;
    std::cerr << "[Entrypoint] Port: " << port << std::endl;
    std::cerr << "[Entrypoint] SQL: " << sql << std::endl;
    std::cerr << "[Entrypoint] Creating socket..." << std::endl;
    
    Socket sock;
    try {
        std::cerr << "[Entrypoint] Connecting to " << host << ":" << port << "..." << std::endl;
        sock.connect(host, port);
        std::cerr << "[Entrypoint] Connected successfully!" << std::endl;
        
        sock.setTimeout(5);
        std::cerr << "[Entrypoint] Timeout set to 5 seconds" << std::endl;
        
        std::cerr << "[Entrypoint] Sending SQL (" << sql.size() << " bytes)..." << std::endl;
        sock.send(sql);
        std::cerr << "[Entrypoint] SQL sent" << std::endl;
        
        std::cerr << "[Entrypoint] Shutting down write..." << std::endl;
        sock.shutdownWrite();
        std::cerr << "[Entrypoint] Write shutdown complete" << std::endl;
        
        std::cerr << "[Entrypoint] Waiting for response..." << std::endl;
        std::string response = sock.recv();
        
        std::cerr << "[Entrypoint] Response received (" << response.size() << " bytes)" << std::endl;
        std::cerr << "[Entrypoint] Response content: " << response << std::endl;
        std::cerr << "[Entrypoint] =========================================" << std::endl;
        
        sock.close();
        return response;
    } catch (const std::exception& e) {
        std::cerr << "[Entrypoint] EXCEPTION in forwardToStorage: " << e.what() << std::endl;
        std::cerr << "[Entrypoint] =========================================" << std::endl;
        return "Error: storage node " + host + ":" + std::to_string(port) +
               " is unavailable (" + e.what() + ")\n";
    }
}

std::pair<std::string, int> Entrypoint::chooseStorageForNewDB() {
    std::cerr << "[Entrypoint] chooseStorageForNewDB: START" << std::endl;
    std::cerr << "[Entrypoint] chooseStorageForNewDB: mutex locked" << std::endl;
    std::cerr << "[Entrypoint] chooseStorageForNewDB: storage_nodes_.size() = " << storage_nodes_.size() << std::endl;
    
    if (storage_nodes_.empty()) {
        std::cerr << "[Entrypoint] chooseStorageForNewDB: No storage nodes!" << std::endl;
        throw std::runtime_error("No storage nodes available");
    }
    
    for (size_t i = 0; i < storage_nodes_.size(); i++) {
        std::cerr << "[Entrypoint] chooseStorageForNewDB: node[" << i << "] = " 
                  << storage_nodes_[i].first << ":" << storage_nodes_[i].second << std::endl;
    }
    
    std::cerr << "[Entrypoint] chooseStorageForNewDB: next_node_index_ = " << next_node_index_ << std::endl;
    size_t index = next_node_index_ % storage_nodes_.size();
    std::cerr << "[Entrypoint] chooseStorageForNewDB: computed index = " << index << std::endl;
    
    auto& node = storage_nodes_[index];
    std::cerr << "[Entrypoint] chooseStorageForNewDB: selected node = " << node.first << ":" << node.second << std::endl;
    
    next_node_index_++;
    std::cerr << "[Entrypoint] chooseStorageForNewDB: next_node_index_ incremented to " << next_node_index_ << std::endl;
    
    std::cerr << "[Entrypoint] chooseStorageForNewDB: returning" << std::endl;
    return node;
}

std::string Entrypoint::extractDatabaseName(const std::string& sql, const std::string& current_db) const {
    std::string upper = sql;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    size_t start = upper.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return current_db;
    upper = upper.substr(start);
    
    if (upper.starts_with("CREATE DATABASE ") || upper.starts_with("CREATE DATABASE\n")) {
        size_t keyword_end = upper.find_first_not_of(" \t", 15);
        if (keyword_end == std::string::npos) return "";
        size_t name_end = upper.find_first_of(" ;\t\n\r", keyword_end);
        if (name_end == std::string::npos) name_end = upper.size();
        return sql.substr(start + keyword_end, name_end - keyword_end);
    }
    
    if (upper.starts_with("USE ")) {
        size_t p = upper.find_first_of(" \t", 4);
        if (p != std::string::npos) {
            return sql.substr(start + 4, p - 4);
        }
        return sql.substr(start + 4);
    }
    
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
    
    std::cerr << "[Entrypoint] New client connected, fd=" << client_fd << std::endl;
    
    while (true) {
        try {
            std::cerr << "[Entrypoint] Waiting for data from client..." << std::endl;
            std::string request = client.recv();
            std::cerr << "[Entrypoint] Received " << request.size() << " bytes from client" << std::endl;
            
            if (request.empty()) {
                std::cerr << "[Entrypoint] Empty request, client disconnected" << std::endl;
                break;
            }
            
            buffer += request;
            size_t pos;
            while ((pos = buffer.find(';')) != std::string::npos) {
                std::string command = buffer.substr(0, pos + 1);
                buffer.erase(0, pos + 1);
                command.erase(0, command.find_first_not_of(" \t\n\r"));
                command.erase(command.find_last_not_of(" \t\n\r") + 1);
                if (command.empty()) continue;
                
                std::cerr << "[Entrypoint] Processing command: " << command << std::endl;
                
                std::string upper_cmd = command;
                std::transform(upper_cmd.begin(), upper_cmd.end(), upper_cmd.begin(), ::toupper);
                
                // ADD STORAGE
                if (upper_cmd.find("ADD STORAGE") == 0) {
                    std::cerr << "[Entrypoint] ADD STORAGE command" << std::endl;
                    std::regex add_regex(R"(ADD STORAGE\s+\"([^\"]+):(\d+)\")");
                    std::smatch m;
                    if (std::regex_search(command, m, add_regex)) {
                        std::string host = m[1].str();
                        int port = std::stoi(m[2].str());
                        std::cerr << "[Entrypoint] Adding storage: " << host << ":" << port << std::endl;
                        addStorageNode(host, port);
                        std::string response = "Storage node added\n";
                        client.send(response);
                        std::cerr << "[Entrypoint] Sent: " << response;
                    } else {
                        std::string error = "Error: usage ADD STORAGE \"host:port\"\n";
                        client.send(error);
                        std::cerr << "[Entrypoint] Sent error: " << error;
                    }
                    continue;
                }
                
                // REMOVE STORAGE
                if (upper_cmd.find("REMOVE STORAGE") == 0) {
                    std::cerr << "[Entrypoint] REMOVE STORAGE command" << std::endl;
                    std::regex rem_regex(R"(REMOVE STORAGE\s+\"([^\"]+):(\d+)\")");
                    std::smatch m;
                    if (std::regex_search(command, m, rem_regex)) {
                        std::string host = m[1].str();
                        int port = std::stoi(m[2].str());
                        std::cerr << "[Entrypoint] Removing storage: " << host << ":" << port << std::endl;
                        removeStorageNode(host, port);
                        client.send("Storage node removed\n");
                    } else {
                        client.send("Error: usage REMOVE STORAGE \"host:port\"\n");
                    }
                    continue;
                }
                
                // SHOW STORAGES
                if (upper_cmd.find("SHOW STORAGES") == 0) {
                    std::cerr << "[Entrypoint] SHOW STORAGES command" << std::endl;
                    std::lock_guard<std::mutex> lock(mutex_);
                    std::stringstream ss;
                    for (const auto& node : storage_nodes_)
                        ss << node.first << ":" << node.second << "\n";
                    std::string response = ss.str();
                    if (response.empty()) response = "No storage nodes\n";
                    client.send(response);
                    std::cerr << "[Entrypoint] Sent storage list: " << response;
                    continue;
                }

                // Extract database name for other commands
                std::string db_name = extractDatabaseName(command, current_db);
                std::cerr << "[Entrypoint] Database name: '" << db_name << "'" << std::endl;

                // CREATE DATABASE
                if (upper_cmd.find("CREATE DATABASE") == 0) {
                    std::cerr << "[Entrypoint] CREATE DATABASE detected" << std::endl;
                    std::cerr << "[Entrypoint] db_name = '" << db_name << "'" << std::endl;
                    std::cerr << "[Entrypoint] db_name.empty() = " << db_name.empty() << std::endl;
                    
                    if (db_name.empty()) {
                        std::cerr << "[Entrypoint] Database name is empty, sending error" << std::endl;
                        client.send("Error: invalid database name\n");
                        continue;
                    }
                    
                    std::cerr << "[Entrypoint] Acquiring mutex lock..." << std::endl;
                    std::pair<std::string, int> target;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        std::cerr << "[Entrypoint] Mutex lock acquired" << std::endl;
                        
                        std::cerr << "[Entrypoint] Checking if database exists: " << db_name << std::endl;
                        if (db_to_storage_.count(db_name)) {
                            std::cerr << "[Entrypoint] Database already exists!" << std::endl;
                            client.send("Error: database already exists\n");
                            continue;
                        }
                        
                        std::cerr << "[Entrypoint] Checking storage_nodes_.empty() = " << storage_nodes_.empty() << std::endl;
                        if (storage_nodes_.empty()) {
                            std::cerr << "[Entrypoint] No storage nodes available!" << std::endl;
                            client.send("Error: no storage nodes available\n");
                            continue;
                        }
                        
                        std::cerr << "[Entrypoint] Calling chooseStorageForNewDB()..." << std::endl;
                        target = chooseStorageForNewDB();
                        std::cerr << "[Entrypoint] Storage chosen: " << target.first << ":" << target.second << std::endl;
                        
                        db_to_storage_[db_name] = target;
                        std::cerr << "[Entrypoint] Database mapped to storage" << std::endl;
                    }
                    std::cerr << "[Entrypoint] Mutex released" << std::endl;
                    
                    std::cerr << "[Entrypoint] Calling forwardToStorage..." << std::endl;
                    std::string response = forwardToStorage(target.first, target.second, command);
                    std::cerr << "[Entrypoint] forwardToStorage returned, response size=" << response.size() << std::endl;
                    std::cerr << "[Entrypoint] Response content: " << response << std::endl;
                    
                    client.send(response);
                    std::cerr << "[Entrypoint] Response sent to client" << std::endl;
                    continue;
                }

                // USE
                if (upper_cmd.find("USE ") == 0) {
                    current_db = db_name;
                    std::string response = "Using database " + current_db + "\n";
                    client.send(response);
                    std::cerr << "[Entrypoint] Sent: " << response;
                    continue;
                }

                // For other SQL commands (SELECT, INSERT, UPDATE, DELETE, etc.)
                if (db_name.empty()) {
                    std::string error = "Error: no database selected or specified\n";
                    client.send(error);
                    std::cerr << "[Entrypoint] Sent error: " << error;
                    continue;
                }

                std::pair<std::string, int> target;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto it = db_to_storage_.find(db_name);
                    if (it == db_to_storage_.end()) {
                        std::string error = "Error: database '" + db_name + "' not found\n";
                        client.send(error);
                        std::cerr << "[Entrypoint] Sent error: " << error;
                        continue;
                    }
                    target = it->second;
                }

                std::cerr << "[Entrypoint] Forwarding SQL to storage: " << target.first << ":" << target.second << std::endl;
                std::string response = forwardToStorage(target.first, target.second, command);
                client.send(response);
                std::cerr << "[Entrypoint] Response sent to client" << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Client handler error: " << e.what() << std::endl;
            break;
        }
    }
    client.close();
    std::cerr << "[Entrypoint] Client disconnected, fd=" << client_fd << std::endl;
}