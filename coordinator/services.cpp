#include "coordinator/services.h"
#include "coordinator/scheduler.h"

namespace orch {

// ─────────────────────────────────────────────
// CoordinatorService (Client-facing)
// ─────────────────────────────────────────────

grpc::Status CoordinatorServiceImpl::SubmitTask(
    grpc::ServerContext* context,
    const orchestrator::SubmitTaskRequest* request,
    orchestrator::SubmitTaskResponse* response) {

    // Validate
    if (request->payload().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "Payload cannot be empty");
    }

    // Create task
    std::string task_id = tasks_.add_task(request->priority(), request->payload());
    response->set_task_id(task_id);

    log_info("Coordinator",
        "Task " + task_id.substr(0, 8) + "... submitted (priority: " +
        std::to_string(request->priority()) + ", payload: \"" +
        request->payload().substr(0, 40) + "\")");

    // Wake the scheduler to dispatch
    scheduler_.notify();

    return grpc::Status::OK;
}

grpc::Status CoordinatorServiceImpl::GetTaskStatus(
    grpc::ServerContext* context,
    const orchestrator::GetTaskStatusRequest* request,
    orchestrator::GetTaskStatusResponse* response) {

    auto task = tasks_.get_task(request->task_id());
    if (!task) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "Task not found: " + request->task_id());
    }

    response->set_task_id(task->task_id);
    response->set_state(task->state);
    response->set_result(task->result);
    response->set_error(task->error);

    if (task->completed_at_ms > 0) {
        response->set_latency_ms(task->completed_at_ms - task->created_at_ms);
    }

    return grpc::Status::OK;
}

grpc::Status CoordinatorServiceImpl::GetMetrics(
    grpc::ServerContext* context,
    const orchestrator::GetMetricsRequest* request,
    orchestrator::GetMetricsResponse* response) {

    const MetricsSnapshot metrics = metrics_.snapshot();
    response->set_throughput_tasks_per_sec(metrics.throughput_tasks_per_sec);
    response->set_p50_latency_ms(metrics.p50_latency_ms);
    response->set_p95_latency_ms(metrics.p95_latency_ms);
    response->set_p99_latency_ms(metrics.p99_latency_ms);
    response->set_queue_depth(metrics.queue_depth);
    response->set_alive_workers(metrics.alive_workers);
    response->set_total_ok(metrics.total_ok);
    response->set_total_err(metrics.total_err);

    return grpc::Status::OK;
}

// ─────────────────────────────────────────────
// WorkerRegistryService (Worker-facing)
// ─────────────────────────────────────────────

grpc::Status WorkerRegistryServiceImpl::RegisterWorker(
    grpc::ServerContext* context,
    const orchestrator::RegisterWorkerRequest* request,
    orchestrator::RegisterWorkerResponse* response) {

    // If this worker was previously registered with in-flight tasks (e.g. it
    // crashed and restarted before the health check fired), requeue those tasks
    // now — the new process has no memory of them.
    auto existing = workers_.get_worker(request->worker_id());
    if (existing) {
        auto orphaned = tasks_.get_tasks_for_worker(request->worker_id());
        for (const auto& task_id : orphaned) {
            tasks_.requeue_task(task_id);
            log_warn("Coordinator",
                "Task " + task_id.substr(0, 8) +
                "... requeued on worker re-registration");
        }
    }

    bool ok = workers_.register_worker(
        request->worker_id(), request->address(), request->capacity());
    response->set_success(ok);

    log_info("Coordinator",
        "Worker " + request->worker_id() + " registered at " +
        request->address() + " (capacity: " +
        std::to_string(request->capacity()) + ")");

    // New worker available — try to dispatch pending tasks
    scheduler_.notify();

    return grpc::Status::OK;
}

grpc::Status WorkerRegistryServiceImpl::Heartbeat(
    grpc::ServerContext* context,
    const orchestrator::HeartbeatRequest* request,
    orchestrator::HeartbeatResponse* response) {

    bool ok = workers_.heartbeat(
        request->worker_id(), request->active_tasks());
    response->set_acknowledged(ok);

    if (!ok) {
        log_warn("Coordinator",
            "Heartbeat from unknown worker: " + request->worker_id());
    }

    return grpc::Status::OK;
}

grpc::Status WorkerRegistryServiceImpl::ReportResult(
    grpc::ServerContext* context,
    const orchestrator::ReportResultRequest* request,
    orchestrator::ReportResultResponse* response) {

    const auto& result = request->result();
    bool ok = false;

    if (result.state() == orchestrator::COMPLETED) {
        ok = tasks_.complete_task(result.task_id(), result.result());
        if (ok) {
            metrics_.record_completion(result.latency_ms(), true);
            log_info("Coordinator",
                "Task " + result.task_id().substr(0, 8) +
                "... COMPLETED (latency: " +
                std::to_string(result.latency_ms()) + "ms)");
        }
    } else if (result.state() == orchestrator::FAILED) {
        ok = tasks_.fail_task(result.task_id(), result.error());
        if (ok) {
            metrics_.record_completion(result.latency_ms(), false);
            log_warn("Coordinator",
                "Task " + result.task_id().substr(0, 8) +
                "... FAILED: " + result.error());
        }
    }

    // Decrement worker load and wake the scheduler — a slot just opened up
    workers_.decrement_load(request->worker_id());
    scheduler_.notify();
    response->set_acknowledged(ok);

    return grpc::Status::OK;
}

}  // namespace orch
