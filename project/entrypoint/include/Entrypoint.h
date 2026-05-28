#ifndef ENTRYPOINT_H
#define ENTRYPOINT_H

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <memory>
#include <chrono>
#include "Auth.h"
#include "Network.h"
#include "AsyncQueue.h"

struct StorageNodeInfo {
    std::string host;
    int port;
    std::string db_root;
    bool alive = false;
    std::chrono::steady_clock::time_point last_heartbeat;
    pid_t pid = 0;

    StorageNodeInfo() = default;
    StorageNodeInfo(const std::string& h, int p, const std::string& root)
        : host(h), port(p), db_root(root) {}
};

class Entrypoint {
public:
    Entrypoint(int client_port = 8080, const std::string& authFile = "auth.data");
    ~Entrypoint();

    void start();
    void stop();

    void addStorageNode(const std::string& host, int port);
    void removeStorageNode(const std::string& host, int port);

private:
    int client_port_;
    int server_fd_;
    bool running_;
    AuthManager auth_;
    std::string executable_path_;

    std::vector<StorageNodeInfo> storage_nodes_;
    std::unordered_map<std::string, std::pair<std::string, int>> db_to_storage_;
    size_t next_node_index_ = 0;

    std::thread heartbeat_thread_;
    std::vector<std::thread> client_threads_;

    std::mutex mutex_;
    std::shared_ptr<AsyncTaskManager> task_manager_;

    std::string forwardToStorage(const std::string& host, int port, const std::string& sql);
    std::pair<std::string, int> chooseStorageForNewDB();
    std::string extractDatabaseName(const std::string& sql, const std::string& current_db) const;

    void heartbeatLoop();
    bool checkNodeAlive(const StorageNodeInfo& node);
    void restartStorageNode(StorageNodeInfo& node);
    void startStorageProcess(StorageNodeInfo& node);
    void handleClient(int client_fd);
};

#endif // ENTRYPOINT_H