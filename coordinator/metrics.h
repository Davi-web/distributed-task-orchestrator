#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "coordinator/task_store.h"
#include "coordinator/worker_pool.h"
#include "common/utils.h"

namespace orch {

struct MetricsSnapshot {
    double throughput_tasks_per_sec = 0.0;
    int64_t p50_latency_ms = 0;
    int64_t p95_latency_ms = 0;
    int64_t p99_latency_ms = 0;
    int64_t queue_depth = 0;
    int64_t alive_workers = 0;
    int64_t total_ok = 0;
    int64_t total_err = 0;
};

class MetricsCollector {
public:
    explicit MetricsCollector(const TaskStore& tasks,
                              const WorkerPool& workers,
                              size_t window_size = 10000)
        : tasks_(tasks),
          workers_(workers),
          window_size_(window_size) {}

    ~MetricsCollector() {
        stop();
    }

    void start() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) {
            return;
        }

        thread_ = std::thread([this]() { run(); });
    }

    void stop() {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false)) {
            return;
        }

        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void record_completion(int64_t latency_ms, bool success) {
        const int64_t completed_at_ms = now_ms();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            samples_.push_back({latency_ms, completed_at_ms});
            if (samples_.size() > window_size_) {
                samples_.pop_front();
            }

            if (success) {
                total_ok_++;
            } else {
                total_err_++;
            }
        }
    }

    MetricsSnapshot snapshot() const {
        std::vector<int64_t> latencies;
        size_t completions_last_5s = 0;
        int64_t total_ok = 0;
        int64_t total_err = 0;
        const int64_t cutoff_ms = now_ms() - 5000;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            latencies.reserve(samples_.size());
            for (const auto& sample : samples_) {
                latencies.push_back(sample.latency_ms);
                if (sample.completed_at_ms >= cutoff_ms) {
                    completions_last_5s++;
                }
            }
            total_ok = total_ok_;
            total_err = total_err_;
        }

        std::sort(latencies.begin(), latencies.end());

        MetricsSnapshot snapshot;
        snapshot.throughput_tasks_per_sec = completions_last_5s / 5.0;
        snapshot.p50_latency_ms = percentile(latencies, 50);
        snapshot.p95_latency_ms = percentile(latencies, 95);
        snapshot.p99_latency_ms = percentile(latencies, 99);
        snapshot.queue_depth = static_cast<int64_t>(tasks_.pending_count());
        snapshot.alive_workers = static_cast<int64_t>(workers_.alive_count());
        snapshot.total_ok = total_ok;
        snapshot.total_err = total_err;
        return snapshot;
    }

private:
    struct CompletionSample {
        int64_t latency_ms;
        int64_t completed_at_ms;
    };

    void run() {
        std::unique_lock<std::mutex> lock(cv_mutex_);
        while (running_) {
            if (cv_.wait_for(lock, std::chrono::seconds(5),
                             [this]() { return !running_.load(); })) {
                break;
            }

            lock.unlock();
            print_snapshot();
            lock.lock();
        }
    }

    void print_snapshot() const {
        const MetricsSnapshot metrics = snapshot();
        std::ostringstream line;
        line << std::fixed << std::setprecision(1)
             << "[Metrics] throughput=" << metrics.throughput_tasks_per_sec
             << " tasks/s | p50=" << metrics.p50_latency_ms << "ms"
             << " p95=" << metrics.p95_latency_ms << "ms"
             << " p99=" << metrics.p99_latency_ms << "ms"
             << " | queue=" << metrics.queue_depth
             << " | workers=" << metrics.alive_workers << " alive"
             << " | total: " << metrics.total_ok << " ok / "
             << metrics.total_err << " err";

        std::cout << line.str() << std::endl;
    }

    static int64_t percentile(const std::vector<int64_t>& values, int pct) {
        if (values.empty()) {
            return 0;
        }

        const size_t index =
            ((values.size() - 1) * static_cast<size_t>(pct)) / 100;
        return values[index];
    }

    const TaskStore& tasks_;
    const WorkerPool& workers_;
    size_t window_size_;

    mutable std::mutex mutex_;
    std::deque<CompletionSample> samples_;
    int64_t total_ok_ = 0;
    int64_t total_err_ = 0;

    std::atomic<bool> running_{false};
    std::condition_variable cv_;
    std::mutex cv_mutex_;
    std::thread thread_;
};

}  // namespace orch
