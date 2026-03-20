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
        log_info("Scheduler", "Dispatch loop started");
    }

    void stop() {
        running_ = false;
        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
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

    bool dispatch_pending() {
        bool dispatched_any = false;

        while (running_) {
            // Check if a worker is available
            auto worker_opt = workers_.select_worker();
            if (!worker_opt) break;  // no workers available

            // Check if a task is pending
            auto task_opt = tasks_.pop_pending();
            if (!task_opt) break;  // no tasks pending

            const auto& worker = *worker_opt;
            const auto& task = *task_opt;

            // Try to assign via gRPC call to the worker
            bool success = assign_to_worker(worker, task);

            if (success) {
                tasks_.assign_task(task.task_id, worker.worker_id);
                workers_.increment_load(worker.worker_id);
                log_info("Scheduler",
                    "Task " + task.task_id.substr(0, 8) + "... -> Worker " +
                    worker.worker_id + " (priority: " +
                    std::to_string(task.priority) + ")");
                dispatched_any = true;
            } else {
                // Worker rejected or unreachable — requeue the task
                tasks_.requeue_task(task.task_id);
                log_warn("Scheduler",
                    "Failed to assign task " + task.task_id.substr(0, 8) +
                    "... to worker " + worker.worker_id + ", requeuing");
            }
        }

        return dispatched_any;
    }

    bool assign_to_worker(const WorkerInfo& worker, const TaskRecord& task) {
        // Get or create a gRPC channel to the worker
        auto* stub = get_worker_stub(worker.address);
        if (!stub) return false;

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

        return status.ok() && response.accepted();
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

    TaskStore& tasks_;
    WorkerPool& workers_;
    std::atomic<bool> running_;
    std::thread thread_;
    std::mutex wait_mutex_;
    std::condition_variable cv_;

    std::mutex stubs_mutex_;
    std::unordered_map<std::string,
        std::unique_ptr<orchestrator::WorkerService::Stub>> stubs_;
};

}  // namespace orch