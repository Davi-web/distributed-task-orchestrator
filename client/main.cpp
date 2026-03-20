#include <iostream>
#include <string>

#include <grpcpp/grpcpp.h>

#include "orchestrator.grpc.pb.h"
#include "common/utils.h"

// ─────────────────────────────────────────────
// Config
// ─────────────────────────────────────────────

struct ClientConfig {
    std::string coordinator_address = "localhost:50051";
    std::string command;           // "submit" or "status"
    std::string payload;
    int priority = 1;             // NORMAL
    std::string task_id;          // for status queries
    bool batch = false;
    int batch_count = 10;
};

void print_usage() {
    std::cout
        << "Usage: client [options] <command>\n\n"
        << "Commands:\n"
        << "  submit    Submit a new task\n"
        << "  status    Query task status\n\n"
        << "Options:\n"
        << "  --coordinator, -c ADDR   Coordinator address (default: localhost:50051)\n"
        << "  --payload TEXT           Task payload (for submit)\n"
        << "  --priority N             Priority 0-3: LOW/NORMAL/HIGH/URGENT (default: 1)\n"
        << "  --id TASK_ID             Task ID (for status)\n"
        << "  --batch N                Submit N tasks rapidly (for testing)\n"
        << "  --help, -h               Show this help\n\n"
        << "Examples:\n"
        << "  client submit --payload \"process_image_42\" --priority 2\n"
        << "  client status --id abc12345-...\n"
        << "  client submit --batch 50 --payload \"load_test\"\n";
}

ClientConfig parse_args(int argc, char* argv[]) {
    ClientConfig config;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "submit") {
            config.command = "submit";
        } else if (arg == "status") {
            config.command = "status";
        } else if ((arg == "--coordinator" || arg == "-c") && i + 1 < argc) {
            config.coordinator_address = argv[++i];
        } else if (arg == "--payload" && i + 1 < argc) {
            config.payload = argv[++i];
        } else if (arg == "--priority" && i + 1 < argc) {
            config.priority = std::stoi(argv[++i]);
        } else if (arg == "--id" && i + 1 < argc) {
            config.task_id = argv[++i];
        } else if (arg == "--batch" && i + 1 < argc) {
            config.batch = true;
            config.batch_count = std::stoi(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(0);
        }
    }
    return config;
}

// ─────────────────────────────────────────────
// State name helper
// ─────────────────────────────────────────────

const char* state_name(orchestrator::TaskState state) {
    switch (state) {
        case orchestrator::PENDING:   return "PENDING";
        case orchestrator::ASSIGNED:  return "ASSIGNED";
        case orchestrator::RUNNING:   return "RUNNING";
        case orchestrator::COMPLETED: return "COMPLETED";
        case orchestrator::FAILED:    return "FAILED";
        default: return "UNKNOWN";
    }
}

const char* priority_name(int p) {
    switch (p) {
        case 0: return "LOW";
        case 1: return "NORMAL";
        case 2: return "HIGH";
        case 3: return "URGENT";
        default: return "UNKNOWN";
    }
}

// ─────────────────────────────────────────────
// Submit command
// ─────────────────────────────────────────────

void do_submit(const ClientConfig& config,
               orchestrator::CoordinatorService::Stub& stub) {
    if (config.payload.empty()) {
        std::cerr << "Error: --payload is required for submit\n";
        std::exit(1);
    }

    if (config.batch) {
        // Batch submit
        std::cout << "Submitting " << config.batch_count << " tasks...\n";
        int64_t start = orch::now_ms();

        for (int i = 0; i < config.batch_count; ++i) {
            orchestrator::SubmitTaskRequest request;
            request.set_priority(
                static_cast<orchestrator::TaskPriority>(config.priority));
            request.set_payload(config.payload + "_" + std::to_string(i));

            orchestrator::SubmitTaskResponse response;
            grpc::ClientContext context;

            grpc::Status status = stub.SubmitTask(&context, request, &response);
            if (!status.ok()) {
                std::cerr << "  [" << i << "] FAILED: "
                          << status.error_message() << "\n";
            }
        }

        int64_t elapsed = orch::now_ms() - start;
        double rate = (config.batch_count * 1000.0) / elapsed;
        std::cout << "Submitted " << config.batch_count << " tasks in "
                  << elapsed << "ms (" << std::fixed << std::setprecision(1)
                  << rate << " tasks/sec)\n";
    } else {
        // Single submit
        orchestrator::SubmitTaskRequest request;
        request.set_priority(
            static_cast<orchestrator::TaskPriority>(config.priority));
        request.set_payload(config.payload);

        orchestrator::SubmitTaskResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub.SubmitTask(&context, request, &response);
        if (status.ok()) {
            std::cout << "Task submitted successfully\n"
                      << "  ID:       " << response.task_id() << "\n"
                      << "  Priority: " << priority_name(config.priority) << "\n"
                      << "  Payload:  " << config.payload << "\n";
        } else {
            std::cerr << "Submit failed: " << status.error_message() << "\n";
            std::exit(1);
        }
    }
}

// ─────────────────────────────────────────────
// Status command
// ─────────────────────────────────────────────

void do_status(const ClientConfig& config,
               orchestrator::CoordinatorService::Stub& stub) {
    if (config.task_id.empty()) {
        std::cerr << "Error: --id is required for status\n";
        std::exit(1);
    }

    orchestrator::GetTaskStatusRequest request;
    request.set_task_id(config.task_id);

    orchestrator::GetTaskStatusResponse response;
    grpc::ClientContext context;

    grpc::Status status = stub.GetTaskStatus(&context, request, &response);
    if (status.ok()) {
        std::cout << "Task Status\n"
                  << "  ID:      " << response.task_id() << "\n"
                  << "  State:   " << state_name(response.state()) << "\n";
        if (!response.result().empty()) {
            std::cout << "  Result:  " << response.result() << "\n";
        }
        if (!response.error().empty()) {
            std::cout << "  Error:   " << response.error() << "\n";
        }
        if (response.latency_ms() > 0) {
            std::cout << "  Latency: " << response.latency_ms() << "ms\n";
        }
    } else {
        std::cerr << "Status query failed: " << status.error_message() << "\n";
        std::exit(1);
    }
}

// ─────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    auto config = parse_args(argc, argv);

    if (config.command.empty()) {
        std::cerr << "Error: specify a command (submit or status)\n";
        print_usage();
        return 1;
    }

    // Connect to coordinator
    auto channel = grpc::CreateChannel(
        config.coordinator_address, grpc::InsecureChannelCredentials());
    auto stub = orchestrator::CoordinatorService::NewStub(channel);

    if (config.command == "submit") {
        do_submit(config, *stub);
    } else if (config.command == "status") {
        do_status(config, *stub);
    } else {
        std::cerr << "Unknown command: " << config.command << "\n";
        print_usage();
        return 1;
    }

    return 0;
}
