#ifndef SERVER_H
#define SERVER_H

#include <string>
#include <thread>
#include <vector>
#include <memory>
#include "DbEngine.h"
#include "AccessLog.h"
#include "Telemetry.h"

class Server {
public:
    Server(int port, const std::string& db_root);
    ~Server();
    void start();
    void stop();

private:
    void handleClient(int client_fd);

    int port_;
    std::string db_root_;
    std::unique_ptr<DBMS> dbms_;
    std::vector<std::thread> client_threads_;
    bool running_;
    int server_fd_;

    std::unique_ptr<AccessLogger> access_logger_;
    TelemetryCollector telemetry_;
};

#endif