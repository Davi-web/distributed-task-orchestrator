#pragma once

#include <atomic>
#include <condition_variable>
#include <grpcpp/grpcpp.h>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "orchestrator.grpc.pb.h"
#include "coordinator/task_store.h"
#include "coordinator/worker_pool.h"
#include "common/utils.h"

namespace orch {

// ─────────────────────────────────────────────
// Scheduler: runs a background loop that
// pulls pending tasks and assigns them to
// available workers via gRPC
// ─────────────────────────────────────────────

class Scheduler {
public:
    Scheduler(TaskStore& tasks, WorkerPool& workers)
        : tasks_(tasks), workers_(workers), running_(false) {}

    ~Scheduler() { stop(); }

    void start() {
        running_ = true;
        thread_ = std::thread(&Scheduler::run, this);
        health_thread_ = std::thread(&Scheduler::run_health_check, this);
        log_info("Scheduler", "Dispatch loop started");
    }

    void stop() {
        running_ = false;
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
        if (health_thread_.joinable()) health_thread_.join();
        log_info("Scheduler", "Dispatch loop stopped");
    }

    // Notify the scheduler that new work is available
    void notify() {
        cv_.notify_one();
    }

private:
    void run() {
        while (running_) {
            // Try to dispatch all available work
            bool dispatched_any = dispatch_pending();

            // If nothing to do, wait for notification or timeout
            if (!dispatched_any) {
                std::unique_lock lock(wait_mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(100));
            }
        }
    }

    enum class AssignResult { SUCCESS, REJECTED, UNREACHABLE };

    bool dispatch_pending() {
        bool dispatched_any = false;

        while (running_) {
            auto worker_opt = workers_.select_worker();
            if (!worker_opt) {
                // Log if tasks are waiting but no worker is available
                if (tasks_.pending_count() > 0) {
                    log_warn("Scheduler",
                        "No available workers — " +
                        std::to_string(tasks_.pending_count()) +
                        " task(s) pending, " +
                        std::to_string(workers_.alive_count()) +
                        " worker(s) alive");
                }
                break;
            }

            auto task_opt = tasks_.pop_pending();
            if (!task_opt) break;

            const auto& worker = *worker_opt;
            const auto& task = *task_opt;

            auto result = assign_to_worker(worker, task);

            if (result == AssignResult::SUCCESS) {
                tasks_.assign_task(task.task_id, worker.worker_id);
                workers_.increment_load(worker.worker_id);
                log_info("Scheduler",
                    "Task " + task.task_id.substr(0, 8) + "... -> Worker " +
                    worker.worker_id + " (priority: " +
                    std::to_string(task.priority) + ")");
                dispatched_any = true;

            } else if (result == AssignResult::UNREACHABLE) {
                // gRPC error — worker's server is down. Mark it dead immediately
                // so it stops being selected. Requeue all its in-flight tasks,
                // not just this one. The worker's next heartbeat will revive it.
                log_warn("Scheduler",
                    "Worker " + worker.worker_id +
                    " unreachable — marking dead, requeuing tasks");
                workers_.mark_dead(worker.worker_id);
                tasks_.requeue_task(task.task_id);
                for (const auto& tid : tasks_.get_tasks_for_worker(worker.worker_id)) {
                    tasks_.requeue_task(tid);
                }
                // Don't break — other workers may be available

            } else {
                // Soft rejection (worker at capacity) — requeue and stop this
                // cycle to avoid hammering the same worker immediately.
                tasks_.requeue_task(task.task_id);
                log_warn("Scheduler",
                    "Worker " + worker.worker_id +
                    " rejected task " + task.task_id.substr(0, 8) +
                    "... (capacity), requeuing");
                break;
            }
        }

        return dispatched_any;
    }

    AssignResult assign_to_worker(const WorkerInfo& worker, const TaskRecord& task) {
        auto* stub = get_worker_stub(worker.address);
        if (!stub) return AssignResult::UNREACHABLE;

        orchestrator::AssignTaskRequest request;
        auto* task_payload = request.mutable_task();
        task_payload->set_task_id(task.task_id);
        task_payload->set_priority(task.priority);
        task_payload->set_payload(task.payload);

        orchestrator::AssignTaskResponse response;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() +
                             std::chrono::seconds(2));

        grpc::Status status = stub->AssignTask(&context, request, &response);

        if (!status.ok())          return AssignResult::UNREACHABLE;
        if (!response.accepted())  return AssignResult::REJECTED;
        return AssignResult::SUCCESS;
    }

    // Cache gRPC stubs to avoid reconnecting on every dispatch
    orchestrator::WorkerService::Stub*
    get_worker_stub(const std::string& address) {
        std::lock_guard lock(stubs_mutex_);
        auto it = stubs_.find(address);
        if (it == stubs_.end()) {
            auto channel = grpc::CreateChannel(
                address, grpc::InsecureChannelCredentials());
            stubs_[address] = orchestrator::WorkerService::NewStub(channel);
        }
        return stubs_[address].get();
    }

    void run_health_check() {
        while (running_) {
            // Sleep in 100ms increments so shutdown exits promptly
            for (int i = 0; i < 20 && running_; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!running_) break;

            // Find workers that missed 3 heartbeat intervals (6 seconds)
            auto dead = workers_.find_dead_workers(6000);
            for (const auto& worker_id : dead) {
                workers_.mark_dead(worker_id);
                log_warn("Scheduler",
                    "Worker " + worker_id + " missed heartbeats — marked DEAD");

                // Requeue all tasks that were assigned to this worker
                auto orphaned = tasks_.get_tasks_for_worker(worker_id);
                for (const auto& task_id : orphaned) {
                    tasks_.requeue_task(task_id);
                    log_warn("Scheduler",
                        "Task " + task_id.substr(0, 8) +
                        "... orphaned by dead worker, requeuing");
                }

                if (!orphaned.empty()) {
                    cv_.notify_one();
                }
            }

            // Requeue tasks stuck in ASSIGNED/RUNNING for over 3 seconds.
            // Catches workers that are alive and heartbeating but silently
            // failing to report results (e.g. ReportResult timeout under load).
            auto timed_out = tasks_.get_timed_out_tasks(3000);
            for (const auto& task_id : timed_out) {
                if (tasks_.requeue_task(task_id)) {
                    log_warn("Scheduler",
                        "Task " + task_id.substr(0, 8) +
                        "... exceeded execution timeout, requeuing");
                    cv_.notify_one();
                }
            }
        }
    }

    TaskStore& tasks_;
    WorkerPool& workers_;
    std::atomic<bool> running_;
    std::thread thread_;
    std::thread health_thread_;
    std::mutex wait_mutex_;
    std::condition_variable cv_;

    std::mutex stubs_mutex_;
    std::unordered_map<std::string,
        std::unique_ptr<orchestrator::WorkerService::Stub>> stubs_;
};

}  // namespace orch