#pragma once

#include <grpcpp/grpcpp.h>
#include <thread>
#include <atomic>

#include "orchestrator.grpc.pb.h"
#include "coordinator/metrics.h"
#include "coordinator/task_store.h"
#include "coordinator/worker_pool.h"
#include "common/utils.h"

namespace orch {

// ─────────────────────────────────────────────
// Forward declare the scheduler so services
// can trigger task dispatch
// ─────────────────────────────────────────────

class Scheduler;

// ─────────────────────────────────────────────
// CoordinatorService: handles Client → Coordinator RPCs
// (SubmitTask, GetTaskStatus)
// ─────────────────────────────────────────────

class CoordinatorServiceImpl final
    : public orchestrator::CoordinatorService::Service {
public:
    CoordinatorServiceImpl(TaskStore& tasks, Scheduler& scheduler,
                           MetricsCollector& metrics)
        : tasks_(tasks), scheduler_(scheduler), metrics_(metrics) {}

    grpc::Status SubmitTask(
        grpc::ServerContext* context,
        const orchestrator::SubmitTaskRequest* request,
        orchestrator::SubmitTaskResponse* response) override;

    grpc::Status GetTaskStatus(
        grpc::ServerContext* context,
        const orchestrator::GetTaskStatusRequest* request,
        orchestrator::GetTaskStatusResponse* response) override;

    grpc::Status GetMetrics(
        grpc::ServerContext* context,
        const orchestrator::GetMetricsRequest* request,
        orchestrator::GetMetricsResponse* response) override;

private:
    TaskStore& tasks_;
    Scheduler& scheduler_;
    MetricsCollector& metrics_;
};

// ─────────────────────────────────────────────
// WorkerRegistryService: handles Worker → Coordinator RPCs
// (RegisterWorker, Heartbeat, ReportResult)
// ─────────────────────────────────────────────

class WorkerRegistryServiceImpl final
    : public orchestrator::WorkerRegistryService::Service {
public:
    WorkerRegistryServiceImpl(TaskStore& tasks, WorkerPool& workers,
                              Scheduler& scheduler,
                              MetricsCollector& metrics)
        : tasks_(tasks),
          workers_(workers),
          scheduler_(scheduler),
          metrics_(metrics) {}

    grpc::Status RegisterWorker(
        grpc::ServerContext* context,
        const orchestrator::RegisterWorkerRequest* request,
        orchestrator::RegisterWorkerResponse* response) override;

    grpc::Status Heartbeat(
        grpc::ServerContext* context,
        const orchestrator::HeartbeatRequest* request,
        orchestrator::HeartbeatResponse* response) override;

    grpc::Status ReportResult(
        grpc::ServerContext* context,
        const orchestrator::ReportResultRequest* request,
        orchestrator::ReportResultResponse* response) override;

private:
    TaskStore& tasks_;
    WorkerPool& workers_;
    Scheduler& scheduler_;
    MetricsCollector& metrics_;
};

}  // namespace orch
