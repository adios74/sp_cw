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
#include <sys/types.h>   // for pid_t
#include "Network.h"

class Entrypoint {
public:
    Entrypoint(int client_port);
    ~Entrypoint();

    void start();
    void stop();

    void addStorageNode(const std::string& host, int port);
    void removeStorageNode(const std::string& host, int port);

private:
    // Extended storage node information
    struct StorageNodeInfo {
        std::string host;
        int port;
        std::string db_root;   // path to database files for this node
        bool alive;
        pid_t pid;             // process ID of the storage server (0 if unknown/not managed)
        std::chrono::steady_clock::time_point last_heartbeat;

        StorageNodeInfo(const std::string& h, int p, const std::string& root)
            : host(h), port(p), db_root(root), alive(true), pid(0) {}
    };

    struct ClientSession {
        std::string current_database;
    };

    void handleClient(int client_fd);
    std::string forwardToStorage(const std::string& host, int port, const std::string& sql);
    std::pair<std::string, int> chooseStorageForNewDB();
    std::string extractDatabaseName(const std::string& sql, const std::string& current_db) const;

    // Heartbeat related methods
    void heartbeatLoop();
    bool checkNodeAlive(const StorageNodeInfo& node);
    void restartStorageNode(StorageNodeInfo& node);
    void startStorageProcess(StorageNodeInfo& node);  // launches the server process

    int client_port_;
    int server_fd_;
    std::atomic<bool> running_;

    mutable std::mutex mutex_;
    std::vector<StorageNodeInfo> storage_nodes_;        // extended node info
    size_t next_node_index_ = 0;
    std::unordered_map<std::string, std::pair<std::string, int>> db_to_storage_;
    std::unordered_map<int, ClientSession> sessions_;

    std::vector<std::thread> client_threads_;
    std::thread heartbeat_thread_;

    // Path to the executable (for restarting storage nodes)
    std::string executable_path_;
};

#endif