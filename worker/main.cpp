#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>

#include <grpcpp/grpcpp.h>

#include "orchestrator.grpc.pb.h"
#include "common/utils.h"

// ─────────────────────────────────────────────
// Global state
// ─────────────────────────────────────────────

static std::atomic<bool> g_shutdown{false};
static std::atomic<int32_t> g_active_tasks{0};
static std::atomic<int32_t> g_completed_tasks{0};

void signal_handler(int signal) {
    orch::log_info("Worker", "Shutting down (signal " + std::to_string(signal) + ")...");
    g_shutdown = true;
}

// ─────────────────────────────────────────────
// Config
// ─────────────────────────────────────────────

struct WorkerConfig {
    std::string coordinator_address = "localhost:50051";
    std::string listen_address = "0.0.0.0";
    std::string listen_port = "50052";
    std::string worker_id;
    int32_t capacity = 4;
    int32_t min_exec_ms = 100;
    int32_t max_exec_ms = 2000;
};

WorkerConfig parse_args(int argc, char* argv[]) {
    WorkerConfig config;
    config.worker_id = "worker-" + orch::generate_uuid().substr(0, 8);

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--coordinator" || arg == "-c") && i + 1 < argc) {
            config.coordinator_address = argv[++i];
        } else if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
            config.listen_port = argv[++i];
        } else if ((arg == "--id") && i + 1 < argc) {
            config.worker_id = argv[++i];
        } else if ((arg == "--capacity") && i + 1 < argc) {
            config.capacity = std::stoi(argv[++i]);
        } else if ((arg == "--min-exec") && i + 1 < argc) {
            config.min_exec_ms = std::stoi(argv[++i]);
        } else if ((arg == "--max-exec") && i + 1 < argc) {
            config.max_exec_ms = std::stoi(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: worker [options]\n"
                      << "  --coordinator, -c ADDR  Coordinator address (default: localhost:50051)\n"
                      << "  --port, -p PORT         Listen port (default: 50052)\n"
                      << "  --id ID                 Worker ID (default: auto-generated)\n"
                      << "  --capacity N            Max concurrent tasks (default: 4)\n"
                      << "  --min-exec MS           Min execution time in ms (default: 100)\n"
                      << "  --max-exec MS           Max execution time in ms (default: 2000)\n"
                      << "  --help, -h              Show this help\n";
            std::exit(0);
        }
    }
    return config;
}

// ─────────────────────────────────────────────
// WorkerService: handles Coordinator → Worker RPCs
// ─────────────────────────────────────────────

class WorkerServiceImpl final : public orchestrator::WorkerService::Service {
public:
    WorkerServiceImpl(const WorkerConfig& config,
                      orchestrator::WorkerRegistryService::Stub* registry)
        : config_(config), registry_(std::move(registry)),
          rng_(std::random_device{}()) {}

    grpc::Status AssignTask(
        grpc::ServerContext* context,
        const orchestrator::AssignTaskRequest* request,
        orchestrator::AssignTaskResponse* response) override {

        const auto& task = request->task();

        // Check capacity
        if (g_active_tasks >= config_.capacity) {
            response->set_accepted(false);
            orch::log_warn("Worker", "Rejected task " +
                task.task_id().substr(0, 8) + "... (at capacity)");
            return grpc::Status::OK;
        }

        response->set_accepted(true);
        g_active_tasks++;

        orch::log_info("Worker", "Received task " +
            task.task_id().substr(0, 8) + "... (payload: \"" +
            task.payload().substr(0, 40) + "\")");

        // Execute asynchronously
        std::thread(&WorkerServiceImpl::execute_task, this,
                    task.task_id(), task.payload()).detach();

        return grpc::Status::OK;
    }

private:
    void execute_task(std::string task_id, std::string payload) {
        int64_t start_ms = orch::now_ms();

        // Simulate work with random duration
        int exec_ms;
        {
            std::lock_guard lock(rng_mutex_);
            std::uniform_int_distribution<int> dist(
                config_.min_exec_ms, config_.max_exec_ms);
            exec_ms = dist(rng_);
        }

        orch::log_info("Worker", "Executing task " +
            task_id.substr(0, 8) + "... (simulated " +
            std::to_string(exec_ms) + "ms)");

        std::this_thread::sleep_for(std::chrono::milliseconds(exec_ms));

        int64_t latency_ms = orch::now_ms() - start_ms;

        // Report result back to coordinator
        orchestrator::ReportResultRequest report;
        report.set_worker_id(config_.worker_id);
        auto* result = report.mutable_result();
        result->set_task_id(task_id);
        result->set_state(orchestrator::COMPLETED);
        result->set_result("Processed: " + payload);
        result->set_latency_ms(latency_ms);

        orchestrator::ReportResultResponse report_response;
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::seconds(5));

        grpc::Status status = registry_->ReportResult(
            &ctx, report, &report_response);

        if (status.ok()) {
            orch::log_info("Worker", "Task " + task_id.substr(0, 8) +
                "... completed (" + std::to_string(latency_ms) + "ms)");
        } else {
            orch::log_error("Worker", "Failed to report result for " +
                task_id.substr(0, 8) + "...: " + status.error_message());
        }

        g_active_tasks--;
        g_completed_tasks++;
    }

    const WorkerConfig& config_;
    orchestrator::WorkerRegistryService::Stub* registry_;
    std::mt19937 rng_;
    std::mutex rng_mutex_;
};

// ─────────────────────────────────────────────
// Heartbeat loop
// ─────────────────────────────────────────────

void heartbeat_loop(const WorkerConfig& config,
                    orchestrator::WorkerRegistryService::Stub* registry) {
    while (!g_shutdown) {
        orchestrator::HeartbeatRequest request;
        request.set_worker_id(config.worker_id);
        request.set_active_tasks(g_active_tasks);
        request.set_completed_tasks(g_completed_tasks);

        orchestrator::HeartbeatResponse response;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() +
                             std::chrono::seconds(2));

        grpc::Status status = registry->Heartbeat(&context, request, &response);
        if (!status.ok()) {
            orch::log_warn("Worker", "Heartbeat failed: " + status.error_message());
        }

        // Sleep 2 seconds between heartbeats
        for (int i = 0; i < 20 && !g_shutdown; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

// ─────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────

int main(int argc, char* argv[]) {
    auto config = parse_args(argc, argv);

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Connect to coordinator
    auto channel = grpc::CreateChannel(
        config.coordinator_address, grpc::InsecureChannelCredentials());
    auto registry = orchestrator::WorkerRegistryService::NewStub(channel);

    // Register with coordinator
    std::string full_address = config.listen_address + ":" + config.listen_port;

    orchestrator::RegisterWorkerRequest reg_request;
    reg_request.set_worker_id(config.worker_id);
    reg_request.set_address("localhost:" + config.listen_port);  // coordinator connects back
    reg_request.set_capacity(config.capacity);

    orchestrator::RegisterWorkerResponse reg_response;
    grpc::ClientContext reg_context;

    grpc::Status reg_status = registry->RegisterWorker(
        &reg_context, reg_request, &reg_response);

    if (!reg_status.ok()) {
        orch::log_error("Worker",
            "Failed to register with coordinator: " + reg_status.error_message());
        return 1;
    }

    orch::log_info("Worker",
        "Registered as " + config.worker_id + " (capacity: " +
        std::to_string(config.capacity) + ")");

    // Start gRPC server for incoming task assignments
    WorkerServiceImpl service(config, registry.get());
    grpc::ServerBuilder builder;
    builder.AddListeningPort(full_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
    if (!server) {
        orch::log_error("Worker", "Failed to start server on " + full_address);
        return 1;
    }

    orch::log_info("Worker", "Listening on " + full_address);

    // Start heartbeat thread
    std::thread hb_thread(heartbeat_loop, std::cref(config), registry.get());

    // Wait for shutdown
    while (!g_shutdown) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Cleanup
    server->Shutdown();
    if (hb_thread.joinable()) hb_thread.join();
    orch::log_info("Worker", "Shut down cleanly");

    return 0;
}
