#ifndef ENTRYPOINT_H
#define ENTRYPOINT_H

#include <string>
#include <thread>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <deque>
#include "Network.h"      // предполагается, что Socket уже есть
#include "Auth.h"

class Entrypoint {
public:
    Entrypoint(int client_port);
    ~Entrypoint();

    void start();
    void stop();

    // Административные команды (можно вызывать и программно)
    void addStorageNode(const std::string& host, int port);
    void removeStorageNode(const std::string& host, int port);

private:
    void handleClient(int client_fd);
    AuthManager auth_;
    // Отправляет SQL-команду на указанный Storage и возвращает ответ
    std::string forwardToStorage(const std::string& host, int port, const std::string& sql);
    
    // Выбирает Storage для новой БД (round-robin)
    std::pair<std::string, int> chooseStorageForNewDB();
    
    // Извлекает имя БД из запроса (USE / квалифицированное имя / сессия)
    std::string extractDatabaseName(const std::string& sql, const std::string& current_db) const;
    
    // Хранилище состояния сессий клиентов
    struct ClientSession {
        std::string current_database;
    };

    int client_port_;
    int server_fd_;
    bool running_;
    
    // Защищённые мьютексом данные кластера
    mutable std::mutex mutex_;
    std::vector<std::pair<std::string, int>> storage_nodes_;        // все узлы
    size_t next_node_index_ = 0;                                   // для round-robin
    std::unordered_map<std::string, std::pair<std::string, int>> db_to_storage_; // БД -> (host, port)
    std::unordered_map<int, ClientSession> sessions_;              // fd -> сессия
    
    std::vector<std::thread> client_threads_;
};

#endif