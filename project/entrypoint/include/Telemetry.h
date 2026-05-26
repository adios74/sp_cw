#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <deque>
#include <mutex>
#include <chrono>
#include <algorithm>
#include <nlohmann/json.hpp>

class TelemetryCollector {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;

    void recordQueryStart() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = Clock::now();
        query_starts_.push_back(now);
        while (!query_starts_.empty() && query_starts_.front() < now - std::chrono::seconds(1)) {
            query_starts_.pop_front();
        }
        aggregateRPS(now);
    }

    void recordQueryEnd(Duration duration, bool is_error) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = Clock::now();
        processing_times_.push_back({now, duration});
        while (!processing_times_.empty() && processing_times_.front().first < now - std::chrono::seconds(10)) {
            processing_times_.pop_front();
        }
        if (is_error) {
            error_timestamps_.push_back(now);
            while (!error_timestamps_.empty() && error_timestamps_.front() < now - std::chrono::minutes(1)) {
                error_timestamps_.pop_front();
            }
        }
    }

    double currentRPS() const {
        auto now = Clock::now();
        while (!query_starts_.empty() && query_starts_.front() < now - std::chrono::seconds(1)) {
            query_starts_.pop_front();
        }
        return static_cast<double>(query_starts_.size());
    }

    double averageRPS10min() const {
        if (rps_aggregates_.empty()) return 0.0;
        double total = 0.0;
        for (const auto& p : rps_aggregates_) total += p.second;
        auto window_sec = std::chrono::duration_cast<std::chrono::seconds>(
            rps_aggregates_.back().first - rps_aggregates_.front().first).count();
        if (window_sec == 0) window_sec = 1;
        return total / window_sec;
    }

    double maxRPS10min() const {
        size_t max_val = 0;
        for (const auto& p : rps_aggregates_)
            if (p.second > max_val) max_val = p.second;
        return static_cast<double>(max_val);
    }

    double averageProcessingTime10s() const {
        if (processing_times_.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& p : processing_times_)
            sum += std::chrono::duration<double, std::milli>(p.second).count();
        return sum / processing_times_.size();
    }

    size_t errorRate1min() const {
        auto now = Clock::now();
        while (!error_timestamps_.empty() && error_timestamps_.front() < now - std::chrono::minutes(1)) {
            error_timestamps_.pop_front();
        }
        return error_timestamps_.size();
    }

    nlohmann::json getMetricsJson() {
        std::lock_guard<std::mutex> lock(mutex_);
        return {
            {"current_rps", currentRPS()},
            {"avg_rps_10min", averageRPS10min()},
            {"max_rps_10min", maxRPS10min()},
            {"avg_time_10s_ms", averageProcessingTime10s()},
            {"errors_1min", errorRate1min()}
        };
    }

private:
    mutable std::mutex mutex_;
    mutable std::deque<TimePoint> query_starts_;
    mutable std::deque<std::pair<TimePoint, Duration>> processing_times_;
    mutable std::deque<TimePoint> error_timestamps_;
    mutable std::deque<std::pair<TimePoint, size_t>> rps_aggregates_;

    void aggregateRPS(TimePoint now) {
        auto this_second = std::chrono::time_point_cast<std::chrono::seconds>(now);
        if (rps_aggregates_.empty() || rps_aggregates_.back().first != this_second) {
            rps_aggregates_.push_back({this_second, 1});
        } else {
            rps_aggregates_.back().second++;
        }
        auto limit = this_second - std::chrono::minutes(10);
        while (!rps_aggregates_.empty() && rps_aggregates_.front().first <= limit) {
            rps_aggregates_.pop_front();
        }
    }
};

#endif