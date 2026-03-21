#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "orchestrator.grpc.pb.h"
#include "common/utils.h"

// ─────────────────────────────────────────────
// Prime checker — trial division up to sqrt(n)
// Execution time scales naturally with input:
//   n ~ 10^6  →  ~1ms
//   n ~ 10^10 →  ~100ms
//   n ~ 10^12 →  ~1s
// ─────────────────────────────────────────────

struct PrimeResult {
    bool is_prime;
    uint64_t smallest_factor;  // 0 if prime
};

PrimeResult check_prime(uint64_t n) {
    if (n < 2)  return {false, 0};
    if (n == 2) return {true, 0};
    if (n % 2 == 0) return {false, 2};
    if (n == 3) return {true, 0};
    if (n % 3 == 0) return {false, 3};

    // Trial division with 6k±1 optimisation
    for (uint64_t i = 5; i * i <= n; i += 6) {
        if (n % i == 0)     return {false, i};
        if (n % (i+2) == 0) return {false, i + 2};
    }
    return {true, 0};
}

std::string run_prime_task(const std::string& payload) {
    if (payload.empty() || payload.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("payload must be a positive integer, got: \"" + payload + "\"");

    uint64_t n = std::stoull(payload);
    auto [is_prime, factor] = check_prime(n);
    if (is_prime)
        return std::to_string(n) + " is prime";
    return std::to_string(n) + " is not prime (smallest factor: " +
           std::to_string(factor) + ")";
}

// ─────────────────────────────────────────────
// Global state
// ─────────────────────────────────────────────

static std::atomic<bool> g_shutdown{false};
static std::atomic<bool> g_draining{false};
static std::atomic<int32_t> g_active_tasks{0};
static std::atomic<int32_t> g_completed_tasks{0};

void signal_handler(int signal) {
    orch::log_info("Worker", "Shutting down (signal " + std::to_string(signal) + ")...");
    g_draining = true;
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
    int32_t capacity = 2;
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
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: worker [options]\n"
                      << "  --coordinator, -c ADDR  Coordinator address (default: localhost:50051)\n"
                      << "  --port, -p PORT         Listen port (default: 50052)\n"
                      << "  --id ID                 Worker ID (default: auto-generated)\n"
                      << "  --capacity N            Max concurrent tasks (default: 4)\n"
                      << "  --help, -h              Show this help\n"
                      << "\nTask payloads are integers to check for primality.\n"
                      << "Examples: \"7\"  \"999999937\"  \"1000000007\"\n";
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
        : config_(config), registry_(registry) {}

    grpc::Status AssignTask(
        grpc::ServerContext* context,
        const orchestrator::AssignTaskRequest* request,
        orchestrator::AssignTaskResponse* response) override {

        const auto& task = request->task();

        if (g_draining) {
            response->set_accepted(false);
            orch::log_warn("Worker", "Rejected task " +
                task.task_id().substr(0, 8) + "... (draining)");
            return grpc::Status::OK;
        }

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

        orch::log_info("Worker", "Checking prime: " + payload);

        orchestrator::ReportResultRequest report;
        report.set_worker_id(config_.worker_id);
        auto* result = report.mutable_result();
        result->set_task_id(task_id);

        try {
            std::string answer = run_prime_task(payload);
            result->set_state(orchestrator::COMPLETED);
            result->set_result(answer);
            orch::log_info("Worker", "Task " + task_id.substr(0, 8) +
                "... result: " + answer);
        } catch (const std::exception& e) {
            result->set_state(orchestrator::FAILED);
            result->set_error(std::string("invalid payload: ") + e.what());
            orch::log_warn("Worker", "Task " + task_id.substr(0, 8) +
                "... failed: " + e.what());
        }

        result->set_latency_ms(orch::now_ms() - start_ms);

        // Retry ReportResult — a transient error (coordinator busy, brief
        // network hiccup) must not silently orphan the task on the coordinator.
        constexpr int kMaxAttempts = 5;
        for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
            orchestrator::ReportResultResponse report_response;
            grpc::ClientContext ctx;
            ctx.set_deadline(std::chrono::system_clock::now() +
                             std::chrono::seconds(5));

            grpc::Status status = registry_->ReportResult(&ctx, report, &report_response);
            if (status.ok()) break;

            orch::log_warn("Worker", "ReportResult attempt " +
                std::to_string(attempt) + "/" + std::to_string(kMaxAttempts) +
                " failed for " + task_id.substr(0, 8) +
                "...: " + status.error_message());

            if (attempt < kMaxAttempts) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200 * attempt));
            } else {
                orch::log_error("Worker", "Giving up on ReportResult for " +
                    task_id.substr(0, 8) + "... after " +
                    std::to_string(kMaxAttempts) + " attempts");
            }
        }

        g_active_tasks--;
        g_completed_tasks++;
    }

    const WorkerConfig& config_;
    orchestrator::WorkerRegistryService::Stub* registry_;
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
    if (!registry) {
        orch::log_error("Worker", "Failed to create gRPC stub for coordinator");
        return 1;
    }
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

    while (g_active_tasks > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Cleanup
    server->Shutdown();
    if (hb_thread.joinable()) hb_thread.join();
    orch::log_info("Worker", "Shut down cleanly");

    return 0;
}
