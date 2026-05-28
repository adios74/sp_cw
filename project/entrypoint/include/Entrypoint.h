#ifndef ENTRYPOINT_H
#define ENTRYPOINT_H

#include <string>
#include <thread>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <deque>
#include <chrono>
#include <atomic>
#include <sys/types.h>   // для pid_t
#include "Network.h"
#include "Auth.h"

class Entrypoint {
public:
    Entrypoint(int client_port, const std::string& authFile = "auth.data");
    ~Entrypoint();

    void start();
    void stop();

    void addStorageNode(const std::string& host, int port);
    void removeStorageNode(const std::string& host, int port);

    void setExecutablePath(const std::string& path);  // для перезапуска storage-узлов

private:
    // Расширенная информация об узле хранения (из первой версии)
    struct StorageNodeInfo {
        std::string host;
        int port;
        std::string db_root;   // путь к файлам БД для этого узла
        bool alive;
        pid_t pid;             // PID процесса storage-сервера (0 если неизвестен)
        std::chrono::steady_clock::time_point last_heartbeat;

        StorageNodeInfo(const std::string& h, int p, const std::string& root)
            : host(h), port(p), db_root(root), alive(true), pid(0) {}
    };

    struct ClientSession {
        std::string current_database;
        // при необходимости можно добавить поля для аутентификации
    };

    void handleClient(int client_fd);
    std::string forwardToStorage(const std::string& host, int port, const std::string& sql);
    std::pair<std::string, int> chooseStorageForNewDB();
    std::string extractDatabaseName(const std::string& sql, const std::string& current_db) const;

    // Методы для heartbeat и перезапуска (из первой версии)
    void heartbeatLoop();
    bool checkNodeAlive(const StorageNodeInfo& node);
    void restartStorageNode(StorageNodeInfo& node);
    void startStorageProcess(StorageNodeInfo& node);

    int client_port_;
    int server_fd_;
    std::atomic<bool> running_;   // атомарный флаг (из первой версии)

    mutable std::mutex mutex_;
    std::vector<StorageNodeInfo> storage_nodes_;   // расширенная информация (первая версия)
    size_t next_node_index_ = 0;
    std::unordered_map<std::string, std::pair<std::string, int>> db_to_storage_;
    std::unordered_map<int, ClientSession> sessions_;

    std::vector<std::thread> client_threads_;
    std::thread heartbeat_thread_;

    std::string executable_path_;   // путь к исполняемому файлу storage-сервера (для перезапуска)
    AuthManager auth_;              // менеджер аутентификации (из второй версии)
};

#endif // ENTRYPOINT_H