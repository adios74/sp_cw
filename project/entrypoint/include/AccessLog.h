#ifndef ACCESS_LOG_H
#define ACCESS_LOG_H

#include <string>
#include <fstream>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <mutex>

class AccessLogger {
public:
    explicit AccessLogger(const std::string& filename) {
        logfile_.open(filename, std::ios::out | std::ios::app);
        if (!logfile_.is_open()) {
            throw std::runtime_error("Cannot open access log file: " + filename);
        }
    }

    ~AccessLogger() {
        if (logfile_.is_open()) logfile_.close();
    }

    void logRequest(const std::string& query,
                    const std::string& client_id,
                    const std::string& handler_id,
                    std::chrono::system_clock::time_point start,
                    std::chrono::system_clock::time_point end,
                    bool success,
                    const std::string& error_message = "") {
        std::lock_guard<std::mutex> lock(mutex_);

        auto start_time = formatTime(start);
        auto end_time   = formatTime(end);
        std::string escaped_query = escape(query);
        int status_code = success ? 0 : 1;
        std::string escaped_error = escape(error_message);

        logfile_ << start_time << "|"
                 << end_time << "|"
                 << client_id << "|"
                 << handler_id << "|"
                 << escaped_query << "|"
                 << status_code << "|"
                 << escaped_error << "\n";
        logfile_.flush();
    }

private:
    std::string formatTime(std::chrono::system_clock::time_point tp) {
        auto t = std::chrono::system_clock::to_time_t(tp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      tp.time_since_epoch()) % 1000;
        std::tm tm = *std::localtime(&t);
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
            << '.' << std::setfill('0') << std::setw(3) << ms.count();
        return oss.str();
    }

    std::string escape(const std::string& s) {
        std::string out;
        for (char c : s) {
            if (c == '|') out += "\\|";
            else if (c == '\n' || c == '\r') out += ' ';
            else out += c;
        }
        return out;
    }

    std::ofstream logfile_;
    std::mutex mutex_;
};

#endif