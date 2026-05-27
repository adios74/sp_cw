#ifndef ENTRYPOINT_H
#define ENTRYPOINT_H

#include <string>
#include <vector>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <utility>

#include "Auth.h"              // ваш менеджер аутентификации
#include "../../common/include/Network.h"           // реальный класс Socket из common/include

class Entrypoint {
public:
    Entrypoint(int client_port, const std::string& authFile = "auth.data");
    ~Entrypoint();

    void start();
    void stop();

    void addStorageNode(const std::string& host, int port);
    void removeStorageNode(const std::string& host, int port);

private:
    int client_port_;
    int server_fd_;
    bool running_;
    std::vector<std::thread> client_threads_;
    AuthManager auth_;

    std::vector<std::pair<std::string, int>> storage_nodes_;
    std::unordered_map<std::string, std::pair<std::string, int>> db_to_storage_;
    size_t next_node_index_ = 0;
    mutable std::mutex mutex_;

    void handleClient(int client_fd);
    std::string forwardToStorage(const std::string& host, int port, const std::string& sql);
    std::pair<std::string, int> chooseStorageForNewDB();
    std::string extractDatabaseName(const std::string& sql, const std::string& current_db) const;
};

#endif // ENTRYPOINT_H