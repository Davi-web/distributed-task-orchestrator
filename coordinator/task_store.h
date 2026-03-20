#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "orchestrator.pb.h"
#include "common/utils.h"

namespace orch {

// ─────────────────────────────────────────────
// Internal task record (richer than the proto)
// ─────────────────────────────────────────────

struct TaskRecord {
    std::string task_id;
    orchestrator::TaskPriority priority;
    std::string payload;
    orchestrator::TaskState state;
    std::string assigned_worker;
    std::string result;
    std::string error;
    int64_t created_at_ms;
    int64_t assigned_at_ms;  // when last dispatched to a worker
    int64_t completed_at_ms;
};

// ─────────────────────────────────────────────
// Priority comparator: higher priority first,
// then earlier creation time
// ─────────────────────────────────────────────

struct TaskPriorityCompare {
    bool operator()(const TaskRecord& a, const TaskRecord& b) const {
        if (a.priority != b.priority)
            return a.priority < b.priority;  // lower enum = lower priority
        return a.created_at_ms > b.created_at_ms;  // older tasks first
    }
};

// ─────────────────────────────────────────────
// Thread-safe task store
// ─────────────────────────────────────────────

class TaskStore {
public:
    // Create a new task, add to queue, return task ID
    std::string add_task(orchestrator::TaskPriority priority,
                         const std::string& payload) {
        std::unique_lock lock(mutex_);

        TaskRecord record;
        record.task_id = generate_uuid();
        record.priority = priority;
        record.payload = payload;
        record.state = orchestrator::PENDING;
        record.created_at_ms = now_ms();
        record.completed_at_ms = 0;

        std::string id = record.task_id;
        pending_queue_.push(record);
        tasks_[id] = std::move(record);

        return id;
    }

    // Pop the highest-priority pending task
    std::optional<TaskRecord> pop_pending() {
        std::unique_lock lock(mutex_);

        while (!pending_queue_.empty()) {
            TaskRecord top = pending_queue_.top();
            pending_queue_.pop();

            // Task might have been reassigned or cancelled — skip stale entries
            auto it = tasks_.find(top.task_id);
            if (it != tasks_.end() && it->second.state == orchestrator::PENDING) {
                return it->second;
            }
        }
        return std::nullopt;
    }

    // Mark task as assigned to a worker
    bool assign_task(const std::string& task_id, const std::string& worker_id) {
        std::unique_lock lock(mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) return false;

        it->second.state = orchestrator::ASSIGNED;
        it->second.assigned_worker = worker_id;
        it->second.assigned_at_ms = now_ms();
        return true;
    }

    // Mark task as running
    bool set_running(const std::string& task_id) {
        std::unique_lock lock(mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) return false;

        it->second.state = orchestrator::RUNNING;
        return true;
    }

    // Mark task as completed with result
    bool complete_task(const std::string& task_id, const std::string& result) {
        std::unique_lock lock(mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) return false;

        it->second.state = orchestrator::COMPLETED;
        it->second.result = result;
        it->second.completed_at_ms = now_ms();
        return true;
    }

    // Mark task as failed with error
    bool fail_task(const std::string& task_id, const std::string& error) {
        std::unique_lock lock(mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) return false;

        it->second.state = orchestrator::FAILED;
        it->second.error = error;
        it->second.completed_at_ms = now_ms();
        return true;
    }

    // Re-queue a task (e.g., after worker failure).
    // Returns false without modifying if the task already reached a terminal
    // state — guards against a race where a worker completes a task just as
    // the health checker is requeueing it.
    bool requeue_task(const std::string& task_id) {
        std::unique_lock lock(mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) return false;

        auto state = it->second.state;
        if (state == orchestrator::COMPLETED || state == orchestrator::FAILED)
            return false;

        it->second.state = orchestrator::PENDING;
        it->second.assigned_worker.clear();
        pending_queue_.push(it->second);
        return true;
    }

    // Get task status (read-only)
    std::optional<TaskRecord> get_task(const std::string& task_id) const {
        std::shared_lock lock(mutex_);
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) return std::nullopt;
        return it->second;
    }

    // Get task IDs stuck in ASSIGNED/RUNNING for longer than timeout_ms.
    // Catches tasks on workers that are alive but silently failing to report.
    std::vector<std::string> get_timed_out_tasks(int64_t timeout_ms) const {
        std::shared_lock lock(mutex_);
        std::vector<std::string> result;
        int64_t cutoff = now_ms() - timeout_ms;
        for (const auto& [id, record] : tasks_) {
            if ((record.state == orchestrator::ASSIGNED ||
                 record.state == orchestrator::RUNNING) &&
                record.assigned_at_ms > 0 &&
                record.assigned_at_ms < cutoff) {
                result.push_back(id);
            }
        }
        return result;
    }

    // Get all task IDs assigned to a specific worker
    std::vector<std::string> get_tasks_for_worker(const std::string& worker_id) const {
        std::shared_lock lock(mutex_);
        std::vector<std::string> result;
        for (const auto& [id, record] : tasks_) {
            if (record.assigned_worker == worker_id &&
                (record.state == orchestrator::ASSIGNED ||
                 record.state == orchestrator::RUNNING)) {
                result.push_back(id);
            }
        }
        return result;
    }

    // Queue depth
    size_t pending_count() const {
        std::shared_lock lock(mutex_);
        return pending_queue_.size();
    }

    size_t total_count() const {
        std::shared_lock lock(mutex_);
        return tasks_.size();
    }

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, TaskRecord> tasks_;
    std::priority_queue<TaskRecord, std::vector<TaskRecord>, TaskPriorityCompare> pending_queue_;
};

}  // namespace orch
