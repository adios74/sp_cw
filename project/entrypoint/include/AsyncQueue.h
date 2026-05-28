#ifndef ASYNC_TASK_MANAGER_H
#define ASYNC_TASK_MANAGER_H

#include <string>
#include <queue>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>
#include <atomic>
#include <nlohmann/json.hpp>

// ======================== UUID v4 Generator ========================
inline std::string generateUUIDv4() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 255);

    std::stringstream ss;
    for (int i = 0; i < 16; ++i) {
        int byte = dis(gen);
        if (i == 6) byte = (byte & 0x0F) | 0x40; // version 4
        if (i == 8) byte = (byte & 0x3F) | 0x80; // variant 10xx
        ss << std::hex << std::setw(2) << std::setfill('0') << byte;
        if (i == 3 || i == 5 || i == 7 || i == 9) ss << '-';
    }
    return ss.str();
}

// ======================== Async Task Manager ========================
class AsyncTaskManager {
public:
    enum class Status {
        PENDING,
        RUNNING,
        COMPLETED,
        FAILED
    };

    struct TaskInfo {
        std::string id;
        std::string sql;
        std::string host;
        int port;
        Status status = Status::PENDING;
        std::string result;
        std::string error;
        std::mutex mtx;
        std::chrono::steady_clock::time_point created_at;
    };

    using ForwardCallback = std::function<std::string(const std::string&, int, const std::string&)>;

    AsyncTaskManager(size_t worker_count, ForwardCallback forward_cb)
        : forward_cb_(std::move(forward_cb)), stop_(false) {
        for (size_t i = 0; i < worker_count; ++i) {
            workers_.emplace_back(&AsyncTaskManager::workerLoop, this);
        }
        cleanup_thread_ = std::thread(&AsyncTaskManager::cleanupLoop, this);
    }

    ~AsyncTaskManager() {
        stop_ = true;
        cv_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
        cv_cleanup_.notify_all();
        if (cleanup_thread_.joinable()) cleanup_thread_.join();
    }

    // Добавляет задачу в очередь и возвращает её идентификатор
    std::string enqueue(const std::string& sql, const std::string& host, int port) {
        auto task = std::make_shared<TaskInfo>();
        task->id = generateUUIDv4();
        task->sql = sql;
        task->host = host;
        task->port = port;
        task->status = Status::PENDING;
        task->created_at = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            task_queue_.push(task);
            tasks_map_[task->id] = task;
        }
        cv_.notify_one();
        return task->id;
    }

    // Возвращает JSON-строку со статусом и результатом задачи
    std::string getStatusJson(const std::string& task_id) const {
        std::shared_ptr<TaskInfo> task;
        {
            std::lock_guard<std::mutex> lock(map_mutex_);
            auto it = tasks_map_.find(task_id);
            if (it == tasks_map_.end()) {
                return R"({"error": "Task not found"})";
            }
            task = it->second;
        }

        std::lock_guard<std::mutex> lock(task->mtx);
        nlohmann::json json;
        switch (task->status) {
            case Status::PENDING:  json["status"] = "PENDING"; break;
            case Status::RUNNING:  json["status"] = "RUNNING"; break;
            case Status::COMPLETED: json["status"] = "COMPLETED"; json["result"] = task->result; break;
            case Status::FAILED:   json["status"] = "FAILED"; json["error"] = task->error; break;
        }
        return json.dump();
    }

private:
    ForwardCallback forward_cb_;
    std::queue<std::shared_ptr<TaskInfo>> task_queue_;
    mutable std::mutex queue_mutex_;
    mutable std::mutex map_mutex_;
    std::unordered_map<std::string, std::shared_ptr<TaskInfo>> tasks_map_;
    std::condition_variable cv_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stop_;

    // Очистка устаревших задач
    std::thread cleanup_thread_;
    std::condition_variable cv_cleanup_;
    std::mutex cleanup_mutex_;

    void workerLoop() {
        while (!stop_.load(std::memory_order_acquire)) {
            std::shared_ptr<TaskInfo> task;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                cv_.wait(lock, [this]() { 
                    return !task_queue_.empty() || stop_.load(std::memory_order_acquire); 
                });
                if (stop_.load(std::memory_order_acquire)) {
                    // Обрабатываем оставшиеся задачи перед выходом (опционально)
                    while (!task_queue_.empty()) {
                        task = task_queue_.front();
                        task_queue_.pop();
                        // Можно выполнить или отбросить в зависимости от требований
                        // Здесь просто отбрасываем, так как сервер завершается
                    }
                    return;
                }
                task = task_queue_.front();
                task_queue_.pop();
            }

            {
                std::lock_guard<std::mutex> lock(task->mtx);
                task->status = Status::RUNNING;
            }

            try {
                std::string response = forward_cb_(task->host, task->port, task->sql);
                std::lock_guard<std::mutex> lock(task->mtx);
                task->result = response;
                task->status = Status::COMPLETED;
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(task->mtx);
                task->error = e.what();
                task->status = Status::FAILED;
            }
        }
    }

    void cleanupLoop() {
        const auto CLEANUP_INTERVAL = std::chrono::seconds(60);
        const auto TTL = std::chrono::minutes(10);

        while (!stop_.load(std::memory_order_acquire)) {
            {
                std::unique_lock<std::mutex> lock(cleanup_mutex_);
                cv_cleanup_.wait_for(lock, CLEANUP_INTERVAL, [this]() { 
                    return stop_.load(std::memory_order_acquire); 
                });
            }
            if (stop_.load(std::memory_order_acquire)) return;

            auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> map_lock(map_mutex_);
            for (auto it = tasks_map_.begin(); it != tasks_map_.end();) {
                auto& task = it->second;
                std::lock_guard<std::mutex> task_lock(task->mtx);
                if ((task->status == Status::COMPLETED || task->status == Status::FAILED) &&
                    (now - task->created_at) > TTL) {
                    it = tasks_map_.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
};

#endif // ASYNC_TASK_MANAGER_H