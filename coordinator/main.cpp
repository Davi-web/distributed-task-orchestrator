#include <csignal>
#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "coordinator/services.h"
#include "coordinator/metrics.h"
#include "coordinator/scheduler.h"
#include "coordinator/task_store.h"
#include "coordinator/worker_pool.h"
#include "common/utils.h"

// ─────────────────────────────────────────────
// Global shutdown signal
// ─────────────────────────────────────────────

static std::atomic<bool> g_shutdown{false};

void signal_handler(int signal) {
    orch::log_info("Coordinator", "Shutting down (signal " + std::to_string(signal) + ")...");
    g_shutdown = true;
}

// ─────────────────────────────────────────────
// Parse CLI args
// ─────────────────────────────────────────────

struct CoordinatorConfig {
    std::string address = "0.0.0.0:50051";
};

CoordinatorConfig parse_args(int argc, char* argv[]) {
    CoordinatorConfig config;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
            config.address = "0.0.0.0:" + std::string(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: coordinator [options]\n"
                      << "  --port, -p PORT   Listen port (default: 50051)\n"
                      << "  --help, -h        Show this help\n";
            std::exit(0);
        }
    }
    return config;
}

// ─────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────

int main(int argc, char* argv[]) {
    auto config = parse_args(argc, argv);

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Core components
    orch::TaskStore tasks;
    orch::WorkerPool workers;
    orch::MetricsCollector metrics(tasks, workers);
    orch::Scheduler scheduler(tasks, workers);

    // gRPC services
    orch::CoordinatorServiceImpl coordinator_service(tasks, scheduler);
    orch::WorkerRegistryServiceImpl registry_service(
        tasks, workers, scheduler, metrics);

    // Build and start server
    grpc::ServerBuilder builder;
    builder.AddListeningPort(config.address, grpc::InsecureServerCredentials());
    builder.RegisterService(&coordinator_service);
    builder.RegisterService(&registry_service);

    std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
    if (!server) {
        orch::log_error("Coordinator", "Failed to start server on " + config.address);
        return 1;
    }

    orch::log_info("Coordinator", "Listening on " + config.address);

    // Start the dispatch loop
    metrics.start();
    scheduler.start();

    // Wait for shutdown signal
    while (!g_shutdown) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Cleanup
    scheduler.stop();
    metrics.stop();
    server->Shutdown();
    orch::log_info("Coordinator", "Server shut down cleanly");

    return 0;
}
