# Distributed Task Orchestrator

A C++ distributed task orchestration system demonstrating gRPC communication, load-aware routing, and fault-tolerant task management.

## Architecture

```
┌─────────┐   SubmitTask    ┌──────────────┐   AssignTask    ┌──────────┐
│  Client  │ ─────────────> │  Coordinator  │ ─────────────> │ Worker 1 │
│  (CLI)   │ <───────────── │              │ <───────────── │          │
└─────────┘   GetStatus     │  - TaskStore  │   ReportResult  └──────────┘
                            │  - WorkerPool │
                            │  - Scheduler  │   AssignTask    ┌──────────┐
                            │              │ ─────────────> │ Worker 2 │
                            └──────────────┘ <───────────── │          │
                                  ▲          ReportResult    └──────────┘
                                  │ Heartbeat / Register          ...
                                  └──────────────────────────────────┘
```

**Coordinator** receives tasks from clients, routes them to the least-loaded worker via gRPC, and tracks task lifecycle (PENDING → ASSIGNED → RUNNING → COMPLETED/FAILED).

**Workers** register with the coordinator, receive task assignments, execute them (simulated), and report results back.

**Client CLI** submits tasks and polls for status.

---

## Prerequisites (Windows)

### Option A: vcpkg (Recommended)

```powershell
# 1. Install vcpkg if you don't have it
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
cd C:\vcpkg
.\bootstrap-vcpkg.bat

# 2. Install dependencies
.\vcpkg install grpc:x64-windows protobuf:x64-windows

# 3. Set environment variable (add to your profile)
$env:CMAKE_TOOLCHAIN_FILE = "C:\vcpkg\scripts\buildsystems\vcpkg.cmake"
```

### Option B: WSL2 (Ubuntu)

```bash
sudo apt-get update
sudo apt-get install -y cmake g++ protobuf-compiler libprotobuf-dev \
    libgrpc++-dev libgrpc-dev protobuf-compiler-grpc pkg-config
```

---

## Build

```bash
# From the project root
mkdir build && cd build

# If using vcpkg on Windows:
cmake .. -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake

# If using WSL2 / Linux:
cmake ..

# Build all targets
cmake --build . --config Release
```

This produces three binaries: `coordinator`, `worker`, `client`

---

## Quick Start Demo

Open **3 terminals**.

### Terminal 1: Start Coordinator
```bash
./coordinator --port 50051
```

### Terminal 2: Start a Worker
```bash
./worker --coordinator localhost:50051 --port 50052
```

### Terminal 3: Submit Tasks
```bash
# Single task
./client submit --payload "process_image_42" --priority 2

# Check status (use the task ID from the submit output)
./client status --id <task-id>

# Batch submit for load testing
./client submit --batch 50 --payload "load_test" --priority 1
```

### Try Multiple Workers

Open more terminals and start additional workers on different ports:

```bash
./worker --coordinator localhost:50051 --port 50053
./worker --coordinator localhost:50051 --port 50054
```

Watch the coordinator terminal — you'll see tasks distributed across workers based on load.

---

## CLI Reference

### Coordinator
| Flag | Default | Description |
|------|---------|-------------|
| `--port, -p` | 50051 | Listen port |

### Worker
| Flag | Default | Description |
|------|---------|-------------|
| `--coordinator, -c` | localhost:50051 | Coordinator address |
| `--port, -p` | 50052 | Listen port |
| `--id` | auto-generated | Worker ID |
| `--capacity` | 4 | Max concurrent tasks |
| `--min-exec` | 100 | Min execution time (ms) |
| `--max-exec` | 2000 | Max execution time (ms) |

### Client
| Flag | Default | Description |
|------|---------|-------------|
| `--coordinator, -c` | localhost:50051 | Coordinator address |
| `--payload` | (required) | Task payload string |
| `--priority` | 1 (NORMAL) | 0=LOW, 1=NORMAL, 2=HIGH, 3=URGENT |
| `--id` | — | Task ID (for status command) |
| `--batch N` | — | Submit N tasks rapidly |

---

## Project Structure

```
distributed-task-orchestrator/
├── CMakeLists.txt              # Build configuration
├── README.md
├── proto/
│   └── orchestrator.proto      # gRPC service + message definitions
├── common/
│   └── utils.h                 # UUID, timestamps, logging
├── coordinator/
│   ├── main.cpp                # Entry point, server setup
│   ├── services.h              # gRPC service declarations
│   ├── services.cpp            # RPC handler implementations
│   ├── scheduler.h             # Task dispatch loop
│   ├── task_store.h            # Thread-safe task state management
│   └── worker_pool.h           # Worker registry + load tracking
├── worker/
│   └── main.cpp                # Worker server + task execution
└── client/
    └── main.cpp                # CLI for submit + status
```

---

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| RPC framework | gRPC | Mirrors Google's internal Stubby; shows protobuf fluency |
| Routing strategy | Least-loaded | Simple, effective, easy to reason about in interviews |
| Task queue | Priority queue | Demonstrates data structure choice with clear trade-offs |
| Thread safety | shared_mutex | Read-heavy workload (status queries) benefits from shared locks |
| Task execution | Simulated sleep | Keeps focus on the orchestration layer, not the work itself |
| State management | In-memory | Intentional simplification; persistence is a documented non-goal |

---

## What's Next (Phase 2)

- **Heartbeat-based failure detection** — Mark workers dead after missed heartbeats, reassign their tasks
- **Task reassignment** — Orphaned tasks return to the pending queue automatically
- **Terminal metrics** — Real-time throughput, p50/p95/p99 latency, queue depth
- **Graceful shutdown** — Workers drain in-flight tasks before exiting
