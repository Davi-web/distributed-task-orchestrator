#pragma once

#include <algorithm>
#include <chrono>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/utils.h"

namespace orch {

// ─────────────────────────────────────────────
// Worker metadata tracked by the coordinator
// ─────────────────────────────────────────────

struct WorkerInfo {
    std::string worker_id;
    std::string address;       // host:port for gRPC calls
    int32_t capacity;          // max concurrent tasks
    int32_t active_tasks;      // current in-flight count
    int64_t last_heartbeat_ms; // timestamp of last heartbeat
    bool alive;
};

// ─────────────────────────────────────────────
// Thread-safe worker pool
// ─────────────────────────────────────────────

class WorkerPool {
public:
    // Register a new worker or re-register an existing one
    bool register_worker(const std::string& worker_id,
                         const std::string& address,
                         int32_t capacity) {
        std::unique_lock lock(mutex_);

        WorkerInfo info;
        info.worker_id = worker_id;
        info.address = address;
        info.capacity = capacity;
        info.active_tasks = 0;
        info.last_heartbeat_ms = now_ms();
        info.alive = true;

        workers_[worker_id] = std::move(info);
        return true;
    }

    // Update heartbeat timestamp and load info
    bool heartbeat(const std::string& worker_id, int32_t active_tasks) {
        std::unique_lock lock(mutex_);
        auto it = workers_.find(worker_id);
        if (it == workers_.end()) return false;

        it->second.last_heartbeat_ms = now_ms();
        it->second.active_tasks = active_tasks;
        it->second.alive = true;
        return true;
    }

    // Select the least-loaded alive worker with available capacity.
    // When multiple workers are tied for minimum load, one is chosen at
    // random to prevent the same worker winning every tie.
    // Returns nullopt if no workers are available.
    std::optional<WorkerInfo> select_worker() const {
        std::shared_lock lock(mutex_);

        int32_t min_load = std::numeric_limits<int32_t>::max();
        std::vector<const WorkerInfo*> candidates;

        for (const auto& [id, worker] : workers_) {
            if (!worker.alive) continue;
            if (worker.active_tasks >= worker.capacity) continue;

            if (worker.active_tasks < min_load) {
                min_load = worker.active_tasks;
                candidates.clear();
                candidates.push_back(&worker);
            } else if (worker.active_tasks == min_load) {
                candidates.push_back(&worker);
            }
        }

        if (candidates.empty()) return std::nullopt;
        if (candidates.size() == 1) return *candidates[0];

        // Break ties randomly
        static thread_local std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<size_t> dist(0, candidates.size() - 1);
        return *candidates[dist(rng)];
    }

    // Increment a worker's active task count (after assignment)
    void increment_load(const std::string& worker_id) {
        std::unique_lock lock(mutex_);
        auto it = workers_.find(worker_id);
        if (it != workers_.end()) {
            it->second.active_tasks++;
        }
    }

    // Decrement a worker's active task count (after completion/failure)
    void decrement_load(const std::string& worker_id) {
        std::unique_lock lock(mutex_);
        auto it = workers_.find(worker_id);
        if (it != workers_.end() && it->second.active_tasks > 0) {
            it->second.active_tasks--;
        }
    }

    // Mark a worker as dead
    void mark_dead(const std::string& worker_id) {
        std::unique_lock lock(mutex_);
        auto it = workers_.find(worker_id);
        if (it != workers_.end()) {
            it->second.alive = false;
        }
    }

    // Remove a worker entirely
    void remove_worker(const std::string& worker_id) {
        std::unique_lock lock(mutex_);
        workers_.erase(worker_id);
    }

    // Get info for a specific worker
    std::optional<WorkerInfo> get_worker(const std::string& worker_id) const {
        std::shared_lock lock(mutex_);
        auto it = workers_.find(worker_id);
        if (it == workers_.end()) return std::nullopt;
        return it->second;
    }

    // Get all alive workers
    std::vector<WorkerInfo> get_alive_workers() const {
        std::shared_lock lock(mutex_);
        std::vector<WorkerInfo> result;
        for (const auto& [id, worker] : workers_) {
            if (worker.alive) result.push_back(worker);
        }
        return result;
    }

    // Find workers that have missed heartbeats
    std::vector<std::string> find_dead_workers(int64_t timeout_ms) const {
        std::shared_lock lock(mutex_);
        int64_t cutoff = now_ms() - timeout_ms;
        std::vector<std::string> dead;
        for (const auto& [id, worker] : workers_) {
            if (worker.alive && worker.last_heartbeat_ms < cutoff) {
                dead.push_back(id);
            }
        }
        return dead;
    }

    size_t alive_count() const {
        std::shared_lock lock(mutex_);
        size_t count = 0;
        for (const auto& [id, worker] : workers_) {
            if (worker.alive) count++;
        }
        return count;
    }

    size_t total_count() const {
        std::shared_lock lock(mutex_);
        return workers_.size();
    }

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, WorkerInfo> workers_;
};

}  // namespace orch
