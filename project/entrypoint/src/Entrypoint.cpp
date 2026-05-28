#include "Entrypoint.h"
#include "Auth.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <regex>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <chrono>
#include <thread>
#include <cstring>

// ==================== Конструктор / Деструктор ====================

Entrypoint::Entrypoint(int client_port, const std::string& authFile)
    : client_port_(client_port), server_fd_(-1), running_(false), auth_(authFile)
{
    char buffer[1024];
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer)-1);
    if (len != -1) {
        buffer[len] = '\0';
        executable_path_ = buffer;
    } else {
        executable_path_ = "./prog";
    }

    task_manager_ = std::make_shared<AsyncTaskManager>(
        4,
        [this](const std::string& host, int port, const std::string& sql) -> std::string {
            return this->forwardToStorage(host, port, sql);
        }
    );
}

Entrypoint::~Entrypoint() { stop(); }

// ==================== Публичные методы ====================

void Entrypoint::start() {
    Socket listener;
    listener.bind(client_port_);
    listener.listen();
    server_fd_ = listener.getFd();
    running_ = true;
    std::cout << "Entrypoint listening for clients on port " << client_port_ << std::endl;

    heartbeat_thread_ = std::thread(&Entrypoint::heartbeatLoop, this);

    while (running_) {
        try {
            int client_fd = listener.accept();
            client_threads_.emplace_back(&Entrypoint::handleClient, this, client_fd);
        } catch (const std::exception& e) {
            if (running_) std::cerr << "Accept error: " << e.what() << std::endl;
        }
    }

    if (heartbeat_thread_.joinable())
        heartbeat_thread_.join();

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
        if (node.host == host && node.port == port) {
            std::cerr << "Storage node " << host << ":" << port << " already registered" << std::endl;
            return;
        }
    }
    std::string db_root;
    if (host == "127.0.0.1" || host == "localhost") {
        db_root = "./storage_data_" + std::to_string(port);
    } else {
        db_root = "";
    }
    storage_nodes_.emplace_back(host, port, db_root);
    std::cout << "Storage node added: " << host << ":" << port << std::endl;
}

void Entrypoint::removeStorageNode(const std::string& host, int port) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(storage_nodes_.begin(), storage_nodes_.end(),
        [&](const StorageNodeInfo& node) {
            return node.host == host && node.port == port;
        });
    if (it == storage_nodes_.end()) {
        std::cerr << "Storage node not found: " << host << ":" << port << std::endl;
        return;
    }
    for (auto db_it = db_to_storage_.begin(); db_it != db_to_storage_.end(); ) {
        if (db_it->second == std::make_pair(it->host, it->port)) {
            std::cout << "Database " << db_it->first << " will be inaccessible (storage removed)" << std::endl;
            db_it = db_to_storage_.erase(db_it);
        } else {
            ++db_it;
        }
    }
    storage_nodes_.erase(it);
    std::cout << "Storage node removed: " << host << ":" << port << std::endl;
}

// ==================== Приватные методы ====================

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

        std::string response;
        char buf[4096];
        while (true) {
            ssize_t n = ::recv(sock.getFd(), buf, sizeof(buf) - 1, 0);
            if (n <= 0) break;
            buf[n] = '\0';
            response += buf;
        }

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
    if (storage_nodes_.empty()) {
        throw std::runtime_error("No storage nodes available");
    }
    std::vector<std::pair<std::string, int>> alive_nodes;
    for (const auto& node : storage_nodes_) {
        if (node.alive)
            alive_nodes.emplace_back(node.host, node.port);
    }
    if (alive_nodes.empty()) {
        throw std::runtime_error("No alive storage nodes available");
    }
    size_t index = next_node_index_ % alive_nodes.size();
    auto& node = alive_nodes[index];
    next_node_index_++;
    return node;
}

std::string Entrypoint::extractDatabaseName(const std::string& sql, const std::string& current_db) const {
    std::string upper = sql;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    size_t start = upper.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return current_db;
    upper = upper.substr(start);

    // CREATE DATABASE
    if (upper.starts_with("CREATE DATABASE ") || upper.starts_with("CREATE DATABASE\n")) {
        size_t keyword_end = upper.find_first_not_of(" \t", 15);
        if (keyword_end == std::string::npos) return "";
        size_t name_end = upper.find_first_of(" ;\t\n\r", keyword_end);
        if (name_end == std::string::npos) name_end = upper.size();
        return sql.substr(start + keyword_end, name_end - keyword_end);
    }

    // DROP DATABASE
    if (upper.starts_with("DROP DATABASE ") || upper.starts_with("DROP DATABASE\n")) {
        size_t keyword_end = upper.find_first_not_of(" \t", 13);
        if (keyword_end == std::string::npos) return "";
        size_t name_end = upper.find_first_of(" ;\t\n\r", keyword_end);
        if (name_end == std::string::npos) name_end = upper.size();
        return sql.substr(start + keyword_end, name_end - keyword_end);
    }

    // USE
    if (upper.starts_with("USE ")) {
        size_t p = upper.find_first_of(" \t", 4);
        if (p != std::string::npos) {
            return sql.substr(start + 4, p - 4);
        }
        return sql.substr(start + 4);
    }

    // REVERT TABLE [db.]table TO TIMESTAMP '...'
    if (upper.starts_with("REVERT TABLE ") || upper.starts_with("REVERT ")) {
        size_t prefix_len = upper.starts_with("REVERT TABLE ") ? 13 : 7; // length of "REVERT TABLE " or "REVERT "
        std::string rest = upper.substr(prefix_len);
        size_t name_start = rest.find_first_not_of(" \t\n\r");
        if (name_start == std::string::npos) return current_db;
        rest = rest.substr(name_start);
        // extract table name up to space or dot
        size_t dot_pos = rest.find('.');
        if (dot_pos != std::string::npos) {
            std::string db_part = rest.substr(0, dot_pos);
            // validate as identifier (start with alpha or underscore)
            if (!db_part.empty() && (std::isalpha(db_part[0]) || db_part[0] == '_')) {
                bool valid = true;
                for (char c : db_part) {
                    if (!std::isalnum(c) && c != '_') { valid = false; break; }
                }
                if (valid) return db_part;
            }
        }
        return current_db;
    }

    // generic db.table detection (fallback)
    std::regex db_table_regex(R"(\b([a-zA-Z_][a-zA-Z0-9_]*)\.([a-zA-Z_][a-zA-Z0-9_]*))");
    std::smatch match;
    if (std::regex_search(sql, match, db_table_regex)) {
        return match[1].str();
    }
    return current_db;
}

// ==================== Heartbeat и управление процессами ====================

void Entrypoint::heartbeatLoop() {
    const int HEARTBEAT_INTERVAL_SEC = 5;

    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(HEARTBEAT_INTERVAL_SEC));

        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& node : storage_nodes_) {
            bool was_alive = node.alive;
            bool is_alive = checkNodeAlive(node);
            node.alive = is_alive;
            node.last_heartbeat = std::chrono::steady_clock::now();
            if (!is_alive && was_alive) {
                std::cerr << "[Heartbeat] Storage node " << node.host << ":" << node.port
                          << " is dead. Attempting restart..." << std::endl;
                restartStorageNode(node);
            } else if (is_alive && !was_alive) {
                std::cout << "[Heartbeat] Storage node " << node.host << ":" << node.port
                          << " is alive again." << std::endl;
            }
        }
    }
}

bool Entrypoint::checkNodeAlive(const StorageNodeInfo& node) {
    Socket test_socket;
    try {
        test_socket.connect(node.host, node.port);
        test_socket.close();
        return true;
    } catch (const std::exception& e) {
        return false;
    }
}

void Entrypoint::restartStorageNode(StorageNodeInfo& node) {
    if (node.host != "127.0.0.1" && node.host != "localhost") {
        std::cerr << "[Heartbeat] Cannot restart remote node " << node.host << ":" << node.port
                  << " – only localhost restart is supported." << std::endl;
        return;
    }

    if (node.pid > 0) {
        ::kill(node.pid, SIGTERM);
        int status;
        waitpid(node.pid, &status, WNOHANG);
        node.pid = 0;
    }

    startStorageProcess(node);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    node.alive = checkNodeAlive(node);
    if (node.alive) {
        std::cout << "[Heartbeat] Successfully restarted storage node " << node.host << ":" << node.port << std::endl;
    } else {
        std::cerr << "[Heartbeat] Failed to restart storage node " << node.host << ":" << node.port << std::endl;
    }
}

void Entrypoint::startStorageProcess(StorageNodeInfo& node) {
    pid_t pid = fork();
    if (pid == -1) {
        std::cerr << "[Heartbeat] Fork failed for storage node " << node.host << ":" << node.port << std::endl;
        return;
    }
    if (pid == 0) {
        std::string port_str = std::to_string(node.port);
        const char* args[] = {
            executable_path_.c_str(),
            "--server",
            port_str.c_str(),
            node.db_root.c_str(),
            nullptr
        };
        execvp(args[0], const_cast<char* const*>(args));
        std::cerr << "[Heartbeat] execvp failed for " << executable_path_ << std::endl;
        exit(1);
    } else {
        node.pid = pid;
    }
}

// ==================== Обработка клиента ====================

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

                std::string command_no_semicolon = command;
                if (!command_no_semicolon.empty() && command_no_semicolon.back() == ';')
                    command_no_semicolon.pop_back();

                std::string upper_cmd = command_no_semicolon;
                std::transform(upper_cmd.begin(), upper_cmd.end(), upper_cmd.begin(), ::toupper);

                // --- Аутентификация ---
                if (upper_cmd.find("LOGIN") == 0 && current_user.empty()) {
                    std::istringstream iss(command_no_semicolon);
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
                    std::string token = command_no_semicolon.substr(7);
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

                std::string db_name = extractDatabaseName(command_no_semicolon, current_db);

                // --- Административные команды (требуют ADMIN) ---
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
                        if (std::regex_search(command_no_semicolon, m, add_regex)) {
                            addStorageNode(m[1].str(), std::stoi(m[2].str()));
                            client.send("Storage node added\n");
                        } else {
                            client.send("Error: usage ADD STORAGE \"host:port\"\n");
                        }
                    } else if (upper_cmd.find("REMOVE STORAGE") == 0) {
                        std::regex rem_regex(R"(REMOVE STORAGE\s+\"([^\"]+):(\d+)\")");
                        std::smatch m;
                        if (std::regex_search(command_no_semicolon, m, rem_regex)) {
                            removeStorageNode(m[1].str(), std::stoi(m[2].str()));
                            client.send("Storage node removed\n");
                        } else {
                            client.send("Error: usage REMOVE STORAGE \"host:port\"\n");
                        }
                    } else { // SHOW STORAGES
                        std::lock_guard<std::mutex> lock(mutex_);
                        std::stringstream ss;
                        for (const auto& node : storage_nodes_)
                            ss << node.host << ":" << node.port
                               << (node.alive ? " (alive)" : " (dead)") << "\n";
                        std::string resp = ss.str();
                        if (resp.empty()) resp = "No storage nodes\n";
                        client.send(resp);
                    }
                    continue;
                }

                // --- Управление пользователями и правами (требуют ADMIN) ---
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

                    if (upper_cmd.find("CREATE USER") == 0) {
                        std::regex re(R"(CREATE USER\s+\"([^\"]+)\"\s+\"([^\"]+)\"(?:\s+ADMIN)?)");
                        std::smatch m;
                        if (std::regex_search(command_no_semicolon, m, re)) {
                            std::string user = m[1];
                            std::string pass = m[2];
                            bool admin = (command_no_semicolon.find("ADMIN") != std::string::npos);
                            if (auth_.createUser(user, pass)) {
                                if (admin) auth_.addUserToGroup(user, "admin");
                                client.send("User created\n");
                            } else {
                                client.send("Error: user already exists\n");
                            }
                        } else {
                            client.send("Error: usage CREATE USER \"username\" \"password\" [ADMIN]\n");
                        }
                    }
                    else if (upper_cmd.find("CREATE GROUP") == 0) {
                        std::regex re(R"(CREATE GROUP\s+\"([^\"]+)\")");
                        std::smatch m;
                        if (std::regex_search(command_no_semicolon, m, re)) {
                            if (auth_.addGroup(m[1])) {
                                client.send("Group created\n");
                            } else {
                                client.send("Error: group already exists\n");
                            }
                        } else {
                            client.send("Error: usage CREATE GROUP \"groupname\"\n");
                        }
                    }
                    else if (upper_cmd.find("ADD USER") == 0) {
                        std::regex re(R"(ADD USER\s+\"([^\"]+)\"\s+TO GROUP\s+\"([^\"]+)\")");
                        std::smatch m;
                        if (std::regex_search(command_no_semicolon, m, re)) {
                            if (auth_.addUserToGroup(m[1], m[2])) {
                                client.send("User added to group\n");
                            } else {
                                client.send("Error: user or group not found, or already in group\n");
                            }
                        } else {
                            client.send("Error: usage ADD USER \"username\" TO GROUP \"groupname\"\n");
                        }
                    }
                    else if (upper_cmd.find("SET PERMISSION ON") == 0 && upper_cmd.find("FOR USER") != std::string::npos) {
                        std::regex re(R"(SET PERMISSION ON\s+\"([^\"]+)\"\s+FOR USER\s+\"([^\"]+)\"\s*=\s*(\d+))");
                        std::smatch m;
                        if (std::regex_search(command_no_semicolon, m, re)) {
                            std::string db = m[1];
                            std::string user = m[2];
                            int flags = std::stoi(m[3]);
                            auth_.setUserDBPermissions(user, db, static_cast<uint8_t>(flags));
                            client.send("Permissions set\n");
                        } else {
                            client.send("Error: usage SET PERMISSION ON \"db\" FOR USER \"username\" = <flags>\n");
                        }
                    }
                    else if (upper_cmd.find("SET PERMISSION ON") == 0 && upper_cmd.find("FOR GROUP") != std::string::npos) {
                        std::regex re(R"(SET PERMISSION ON\s+\"([^\"]+)\"\s+FOR GROUP\s+\"([^\"]+)\"\s*=\s*(\d+))");
                        std::smatch m;
                        if (std::regex_search(command_no_semicolon, m, re)) {
                            std::string db = m[1];
                            std::string grp = m[2];
                            int flags = std::stoi(m[3]);
                            auth_.setGroupDBPermissions(grp, db, static_cast<uint8_t>(flags));
                            client.send("Permissions set\n");
                        } else {
                            client.send("Error: usage SET PERMISSION ON \"db\" FOR GROUP \"groupname\" = <flags>\n");
                        }
                    }
                    else if (upper_cmd.find("SET DEFAULT PERMISSION ON") == 0) {
                        std::regex re(R"(SET DEFAULT PERMISSION ON\s+\"([^\"]+)\"\s*=\s*(\d+))");
                        std::smatch m;
                        if (std::regex_search(command_no_semicolon, m, re)) {
                            std::string db = m[1];
                            int flags = std::stoi(m[2]);
                            auth_.setDefaultDBPermissions(db, static_cast<uint8_t>(flags));
                            client.send("Default permissions set\n");
                        } else {
                            client.send("Error: usage SET DEFAULT PERMISSION ON \"db\" = <flags>\n");
                        }
                    }
                    else if (upper_cmd.find("SHOW USERS") == 0) {
                        client.send(auth_.listUsers());
                    }
                    else if (upper_cmd.find("SHOW GROUPS") == 0) {
                        client.send(auth_.listGroups());
                    }
                    else if (upper_cmd.find("SHOW PERMISSIONS FOR USER") == 0) {
                        std::regex re(R"(SHOW PERMISSIONS FOR USER\s+\"([^\"]+)\")");
                        std::smatch m;
                        if (std::regex_search(command_no_semicolon, m, re)) {
                            client.send(auth_.getUserPermissions(m[1]));
                        } else {
                            client.send("Error: usage SHOW PERMISSIONS FOR USER \"username\"\n");
                        }
                    }
                    else if (upper_cmd.find("SHOW PERMISSIONS FOR GROUP") == 0) {
                        std::regex re(R"(SHOW PERMISSIONS FOR GROUP\s+\"([^\"]+)\")");
                        std::smatch m;
                        if (std::regex_search(command_no_semicolon, m, re)) {
                            client.send(auth_.getGroupPermissions(m[1]));
                        } else {
                            client.send("Error: usage SHOW PERMISSIONS FOR GROUP \"groupname\"\n");
                        }
                    }
                    else {
                        client.send("Use: SHOW PERMISSIONS FOR USER \"name\" or FOR GROUP \"name\"\n");
                    }
                    continue;
                }

                // --- USE ---
                if (upper_cmd.find("USE ") == 0) {
                    current_db = db_name;
                    std::cerr << "[Entrypoint] DEBUG: USE command, setting current_db to: " << current_db << std::endl;
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
                        try {
                            target = chooseStorageForNewDB();
                        } catch (const std::runtime_error& e) {
                            client.send(std::string("Error: ") + e.what() + "\n");
                            continue;
                        }
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

                // --- Асинхронные запросы (ASYNC) ---
                if (upper_cmd.find("ASYNC ") == 0) {
                    std::string async_sql = command_no_semicolon.substr(6);
                    size_t start = async_sql.find_first_not_of(" \t");
                    if (start != std::string::npos) async_sql = async_sql.substr(start);
                    if (async_sql.empty()) {
                        client.send("Error: missing SQL after ASYNC\n");
                        continue;
                    }

                    std::string async_db = extractDatabaseName(async_sql, current_db);
                    
                    std::cerr << "[Entrypoint] DEBUG ASYNC: async_sql='" << async_sql << "'" << std::endl;
                    std::cerr << "[Entrypoint] DEBUG ASYNC: async_db='" << async_db << "'" << std::endl;
                    std::cerr << "[Entrypoint] DEBUG ASYNC: current_db='" << current_db << "'" << std::endl;
                    std::cerr << "[Entrypoint] DEBUG ASYNC: current_user='" << current_user << "'" << std::endl;
                    
                    if (async_db.empty()) {
                        client.send("Error: no database specified or selected for ASYNC command\n");
                        continue;
                    }

                    // Проверка прав доступа
                    std::string upper_async = async_sql;
                    std::transform(upper_async.begin(), upper_async.end(), upper_async.begin(), ::toupper);
                    size_t first_non_space = upper_async.find_first_not_of(" \t\n\r");
                    if (first_non_space != std::string::npos) {
                        upper_async = upper_async.substr(first_non_space);
                    }
                    
                    bool allowed = false;
                    if (upper_async.find("CREATE TABLE") == 0) {
                        allowed = auth_.checkPermission(current_user, async_db, Operation::CREATE_TABLE);
                    }
                    else if (upper_async.find("DROP TABLE") == 0) {
                        allowed = auth_.checkPermission(current_user, async_db, Operation::DROP_TABLE);
                    }
                    else if (upper_async.find("SELECT") == 0) {
                        allowed = auth_.checkPermission(current_user, async_db, Operation::READ);
                    }
                    else if (upper_async.find("INSERT") == 0 ||
                             upper_async.find("UPDATE") == 0 ||
                             upper_async.find("DELETE") == 0) {
                        allowed = auth_.checkPermission(current_user, async_db, Operation::WRITE);
                    }
                    else if (upper_async.find("REVERT") == 0) {
                        allowed = auth_.checkPermission(current_user, async_db, Operation::WRITE);
                    }
                    else {
                        client.send("Error: unsupported command for async execution\n");
                        continue;
                    }

                    if (!allowed) {
                        client.send("Error: permission denied for async operation\n");
                        continue;
                    }

                    std::pair<std::string, int> target;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        auto it = db_to_storage_.find(async_db);
                        if (it == db_to_storage_.end()) {
                            client.send("Error: database '" + async_db + "' not found\n");
                            continue;
                        }
                        target = it->second;
                    }

                    std::string full_sql = "USE " + async_db + "; " + async_sql;
                    if (!full_sql.empty() && full_sql.back() != ';') {
                        full_sql += ';';
                    }
                    std::string task_id = task_manager_->enqueue(full_sql, target.first, target.second);
                    client.send("TASK " + task_id + "\n");
                    continue;
                }

                // --- Проверка статуса асинхронной задачи ---
                if (upper_cmd.find("STATUS ") == 0) {
                    std::string task_id = command_no_semicolon.substr(7);
                    size_t st = task_id.find_first_not_of(" \t");
                    size_t en = task_id.find_last_not_of(" \t");
                    if (st != std::string::npos)
                        task_id = task_id.substr(st, en - st + 1);
                    std::string json = task_manager_->getStatusJson(task_id);
                    client.send(json + "\n");
                    continue;
                }

                // --- Обычные SQL-команды (синхронные) ---
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

                // Проверка прав для синхронных команд
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
                else if (upper_cmd.find("REVERT") == 0)
                    allowed = auth_.checkPermission(current_user, db_name, Operation::WRITE);
                else {
                    client.send("Error: unsupported command\n");
                    continue;
                }

                if (!allowed) {
                    client.send("Error: permission denied\n");
                    continue;
                }

                std::string forward_cmd = "USE " + db_name + "; " + command;
                std::string response = forwardToStorage(target.first, target.second, forward_cmd);
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