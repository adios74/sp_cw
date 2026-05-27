#include "Entrypoint.h"
#include "Auth.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <regex>

// ==================== Реализация Entrypoint ====================

Entrypoint::Entrypoint(int client_port, const std::string& authFile)
    : client_port_(client_port), server_fd_(-1), running_(false), auth_(authFile) {}

Entrypoint::~Entrypoint() { stop(); }

void Entrypoint::start() {
    Socket listener;
    listener.bind(client_port_);
    listener.listen();
    server_fd_ = listener.getFd();
    running_ = true;
    std::cout << "Entrypoint listening for clients on port " << client_port_ << std::endl;

    // Дефолтные права для новых БД (опционально)
    // auth_.setDefaultDBPermissions("mydb", (uint8_t)Operation::READ | (uint8_t)Operation::WRITE);

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
    
    Socket sock;
    try {
        sock.connect(host, port);
        sock.setTimeout(5);
        sock.send(sql);
        sock.shutdownWrite();
        std::string response = sock.recv();
        std::cerr << "[Entrypoint] Response: " << response << std::endl;
        sock.close();
        return response;
    } catch (const std::exception& e) {
        std::cerr << "[Entrypoint] EXCEPTION: " << e.what() << std::endl;
        return "Error: storage node " + host + ":" + std::to_string(port) +
               " is unavailable (" + e.what() + ")\n";
    }
}

std::pair<std::string, int> Entrypoint::chooseStorageForNewDB() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (storage_nodes_.empty()) {
        throw std::runtime_error("No storage nodes available");
    }
    size_t index = next_node_index_ % storage_nodes_.size();
    auto& node = storage_nodes_[index];
    next_node_index_++;
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
    
    if (upper.starts_with("DROP DATABASE ") || upper.starts_with("DROP DATABASE\n")) {
        size_t keyword_end = upper.find_first_not_of(" \t", 13);
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
    std::string current_user;

    std::cerr << "[Entrypoint] New client connected, fd=" << client_fd << std::endl;
    
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
                
                // --- Аутентификация ---
                if (upper_cmd.find("LOGIN") == 0 && current_user.empty()) {
                    if (command.back() == ';') command.pop_back(); 
                    std::istringstream iss(command);
                    std::string cmd, username, password;
                    iss >> cmd >> username >> password;
                    std::string token = auth_.login(username, password);
                    if (token.empty()) {
                        client.send("Error: authentication failed\n");
                    } else {
                        client.send("TOKEN " + token + "\n");
                    }
                    continue;
                }

                if (upper_cmd.find("BEARER ") == 0 && current_user.empty()) {
                    std::string token = command.substr(7);
                    if (!token.empty() && token.back() == ';') token.pop_back();
                    size_t st = token.find_first_not_of(" \t");
                    size_t en = token.find_last_not_of(" \t");
                    if (st != std::string::npos)
                        token = token.substr(st, en - st + 1);
                    std::string user = auth_.validateToken(token);
                    if (user.empty()) {
                        client.send("Error: invalid or expired token\n");
                    } else {
                        current_user = user;
                        client.send("OK\n");
                    }
                    continue;
                }

                if (current_user.empty()) {
                    client.send("Error: authentication required (use LOGIN then BEARER)\n");
                    continue;
                }
                
                std::string db_name = extractDatabaseName(command, current_db);

                // --- Административные команды ---
                if (upper_cmd.find("ADD STORAGE") == 0 ||
                    upper_cmd.find("REMOVE STORAGE") == 0 ||
                    upper_cmd.find("SHOW STORAGES") == 0) {
                    
                    if (!auth_.checkPermission(current_user, "", Operation::ADMIN)) {
                        client.send("Error: administrator privileges required\n");
                        continue;
                    }
                    
                    if (upper_cmd.find("ADD STORAGE") == 0) {
                        std::regex add_regex(R"(ADD STORAGE\s+\"([^\"]+):(\d+)\")");
                        std::smatch m;
                        if (std::regex_search(command, m, add_regex)) {
                            addStorageNode(m[1].str(), std::stoi(m[2].str()));
                            client.send("Storage node added\n");
                        } else {
                            client.send("Error: usage ADD STORAGE \"host:port\"\n");
                        }
                    } else if (upper_cmd.find("REMOVE STORAGE") == 0) {
                        std::regex rem_regex(R"(REMOVE STORAGE\s+\"([^\"]+):(\d+)\")");
                        std::smatch m;
                        if (std::regex_search(command, m, rem_regex)) {
                            removeStorageNode(m[1].str(), std::stoi(m[2].str()));
                            client.send("Storage node removed\n");
                        } else {
                            client.send("Error: usage REMOVE STORAGE \"host:port\"\n");
                        }
                    } else {
                        std::lock_guard<std::mutex> lock(mutex_);
                        std::stringstream ss;
                        for (const auto& node : storage_nodes_)
                            ss << node.first << ":" << node.second << "\n";
                        std::string resp = ss.str();
                        if (resp.empty()) resp = "No storage nodes\n";
                        client.send(resp);
                    }
                    continue;
                }
                
                // --- Управление пользователями (только ADMIN) ---
                if (upper_cmd.find("CREATE USER") == 0 ||
                    upper_cmd.find("CREATE GROUP") == 0 ||
                    upper_cmd.find("ADD USER") == 0 ||
                    upper_cmd.find("SET PERMISSION ON") == 0 ||
                    upper_cmd.find("SET DEFAULT PERMISSION ON") == 0 ||
                    upper_cmd.find("SHOW USERS") == 0 ||
                    upper_cmd.find("SHOW GROUPS") == 0 ||
                    upper_cmd.find("SHOW PERMISSIONS") == 0) {

                    if (!auth_.checkPermission(current_user, "", Operation::ADMIN)) {
                        client.send("Error: administrator privileges required\n");
                        continue;
                    }

                    // CREATE USER "username" "password" [ADMIN]
                    if (upper_cmd.find("CREATE USER") == 0) {
                        std::regex re(R"(CREATE USER\s+\"([^\"]+)\"\s+\"([^\"]+)\"(?:\s+ADMIN)?)");
                        std::smatch m;
                        if (std::regex_search(command, m, re)) {
                            std::string user = m[1];
                            std::string pass = m[2];
                            bool admin = (command.find("ADMIN") != std::string::npos);
                            if (auth_.createUser(user, pass)) {
                                if (admin) {
                                    auth_.addUserToGroup(user, "admin");
                                }
                                client.send("User created\n");
                            } else {
                                client.send("Error: user already exists\n");
                            }
                        } else {
                            client.send("Error: usage CREATE USER \"username\" \"password\" [ADMIN]\n");
                        }
                    }
                    // CREATE GROUP "groupname"
                    else if (upper_cmd.find("CREATE GROUP") == 0) {
                        std::regex re(R"(CREATE GROUP\s+\"([^\"]+)\")");
                        std::smatch m;
                        if (std::regex_search(command, m, re)) {
                            if (auth_.addGroup(m[1])) {
                                client.send("Group created\n");
                            } else {
                                client.send("Error: group already exists\n");
                            }
                        } else {
                            client.send("Error: usage CREATE GROUP \"groupname\"\n");
                        }
                    }
                    // ADD USER "username" TO GROUP "groupname"
                    else if (upper_cmd.find("ADD USER") == 0) {
                        std::regex re(R"(ADD USER\s+\"([^\"]+)\"\s+TO GROUP\s+\"([^\"]+)\")");
                        std::smatch m;
                        if (std::regex_search(command, m, re)) {
                            if (auth_.addUserToGroup(m[1], m[2])) {
                                client.send("User added to group\n");
                            } else {
                                client.send("Error: user or group not found, or already in group\n");
                            }
                        } else {
                            client.send("Error: usage ADD USER \"username\" TO GROUP \"groupname\"\n");
                        }
                    }
                    // SET PERMISSION ON "db" FOR USER "username" = <flags>
                    else if (upper_cmd.find("SET PERMISSION ON") == 0 && upper_cmd.find("FOR USER") != std::string::npos) {
                        std::regex re(R"(SET PERMISSION ON\s+\"([^\"]+)\"\s+FOR USER\s+\"([^\"]+)\"\s*=\s*(\d+))");
                        std::smatch m;
                        if (std::regex_search(command, m, re)) {
                            std::string db = m[1];
                            std::string user = m[2];
                            int flags = std::stoi(m[3]);
                            auth_.setUserDBPermissions(user, db, static_cast<uint8_t>(flags));
                            client.send("Permissions set\n");
                        } else {
                            client.send("Error: usage SET PERMISSION ON \"db\" FOR USER \"username\" = <flags>\n");
                        }
                    }
                    // SET PERMISSION ON "db" FOR GROUP "groupname" = <flags>
                    else if (upper_cmd.find("SET PERMISSION ON") == 0 && upper_cmd.find("FOR GROUP") != std::string::npos) {
                        std::regex re(R"(SET PERMISSION ON\s+\"([^\"]+)\"\s+FOR GROUP\s+\"([^\"]+)\"\s*=\s*(\d+))");
                        std::smatch m;
                        if (std::regex_search(command, m, re)) {
                            std::string db = m[1];
                            std::string grp = m[2];
                            int flags = std::stoi(m[3]);
                            auth_.setGroupDBPermissions(grp, db, static_cast<uint8_t>(flags));
                            client.send("Permissions set\n");
                        } else {
                            client.send("Error: usage SET PERMISSION ON \"db\" FOR GROUP \"groupname\" = <flags>\n");
                        }
                    }
                    // SET DEFAULT PERMISSION ON "db" = <flags>
                    else if (upper_cmd.find("SET DEFAULT PERMISSION ON") == 0) {
                        std::regex re(R"(SET DEFAULT PERMISSION ON\s+\"([^\"]+)\"\s*=\s*(\d+))");
                        std::smatch m;
                        if (std::regex_search(command, m, re)) {
                            std::string db = m[1];
                            int flags = std::stoi(m[2]);
                            auth_.setDefaultDBPermissions(db, static_cast<uint8_t>(flags));
                            client.send("Default permissions set\n");
                        } else {
                            client.send("Error: usage SET DEFAULT PERMISSION ON \"db\" = <flags>\n");
                        }
                    }
                    // SHOW USERS
                    else if (upper_cmd.find("SHOW USERS") == 0) {
                        client.send(auth_.listUsers());
                    }
                    // SHOW GROUPS
                    else if (upper_cmd.find("SHOW GROUPS") == 0) {
                        client.send(auth_.listGroups());
                    }
                    // SHOW PERMISSIONS FOR USER "username"
                    else if (upper_cmd.find("SHOW PERMISSIONS FOR USER") == 0) {
                        std::regex re(R"(SHOW PERMISSIONS FOR USER\s+\"([^\"]+)\")");
                        std::smatch m;
                        if (std::regex_search(command, m, re)) {
                            client.send(auth_.getUserPermissions(m[1]));
                        } else {
                            client.send("Error: usage SHOW PERMISSIONS FOR USER \"username\"\n");
                        }
                    }
                    // SHOW PERMISSIONS FOR GROUP "groupname"
                    else if (upper_cmd.find("SHOW PERMISSIONS FOR GROUP") == 0) {
                        std::regex re(R"(SHOW PERMISSIONS FOR GROUP\s+\"([^\"]+)\")");
                        std::smatch m;
                        if (std::regex_search(command, m, re)) {
                            client.send(auth_.getGroupPermissions(m[1]));
                        } else {
                            client.send("Error: usage SHOW PERMISSIONS FOR GROUP \"groupname\"\n");
                        }
                    }
                    // SHOW PERMISSIONS (без параметров) – краткая подсказка
                    else if (upper_cmd.find("SHOW PERMISSIONS") == 0) {
                        client.send("Use: SHOW PERMISSIONS FOR USER \"name\" or FOR GROUP \"name\"\n");
                    }
                    continue;
                }
                
                // --- USE ---
                if (upper_cmd.find("USE ") == 0) {
                    current_db = db_name;
                    client.send("Using database " + current_db + "\n");
                    continue;
                }
                
                // --- CREATE DATABASE ---
                if (upper_cmd.find("CREATE DATABASE") == 0) {
                    if (!auth_.checkPermission(current_user, "", Operation::CREATE_DATABASE)) {
                        client.send("Error: permission denied (CREATE DATABASE)\n");
                        continue;
                    }
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
                
                // --- DROP DATABASE ---
                if (upper_cmd.find("DROP DATABASE") == 0) {
                    if (!auth_.checkPermission(current_user, db_name, Operation::DELETE_DATABASE)) {
                        client.send("Error: permission denied (DROP DATABASE)\n");
                        continue;
                    }
                    if (db_name.empty()) {
                        client.send("Error: no database specified\n");
                        continue;
                    }
                    
                    std::pair<std::string, int> target;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        auto it = db_to_storage_.find(db_name);
                        if (it == db_to_storage_.end()) {
                            client.send("Error: database not found\n");
                            continue;
                        }
                        target = it->second;
                    }
                    
                    std::string response = forwardToStorage(target.first, target.second, command);
                    if (response.find("Database dropped") != std::string::npos ||
                        response.find("dropped") != std::string::npos) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        db_to_storage_.erase(db_name);
                        if (current_db == db_name) current_db.clear();
                    }
                    client.send(response);
                    continue;
                }
                
                // --- Прочие SQL-команды ---
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
                
                bool allowed = false;
                if (upper_cmd.find("CREATE TABLE") == 0)
                    allowed = auth_.checkPermission(current_user, db_name, Operation::CREATE_TABLE);
                else if (upper_cmd.find("DROP TABLE") == 0)
                    allowed = auth_.checkPermission(current_user, db_name, Operation::DROP_TABLE);
                else if (upper_cmd.find("SELECT") == 0)
                    allowed = auth_.checkPermission(current_user, db_name, Operation::READ);
                else if (upper_cmd.find("INSERT") == 0 ||
                         upper_cmd.find("UPDATE") == 0 ||
                         upper_cmd.find("DELETE") == 0)
                    allowed = auth_.checkPermission(current_user, db_name, Operation::WRITE);
                else {
                    client.send("Error: unsupported command\n");
                    continue;
                }
                
                if (!allowed) {
                    client.send("Error: permission denied\n");
                    continue;
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
    std::cerr << "[Entrypoint] Client disconnected, fd=" << client_fd << std::endl;
}