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

**Workers** register with the coordinator, receive task assignments, execute them (prime checking), and report results back.

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

### Terminal 2: Start Workers
```bash
# Start 2–5 workers on different ports
./worker --coordinator localhost:50051 --port 50052
./worker --coordinator localhost:50051 --port 50053
```

### Terminal 3: Submit Tasks
```bash
# Single task — payload is a number to check for primality
./client submit --payload 999999937 --priority 2

# Check status (use the task ID from the submit output)
./client status --id <task-id>

# Batch load test — submits N tasks with random numbers and random priorities
./client submit --batch 100 --random-priority
```

### Task Payloads

The worker checks whether the payload is a prime number using trial division. Execution time scales naturally with input size:

| Payload range | Example | Approx time |
|---------------|---------|-------------|
| Small (< 10,000) | `9973` | < 1ms |
| Medium (< 10^9) | `999999937` | ~1ms |
| Large (10^10+) | `9999999967` | ~5–50ms |

This gives a realistic mix of fast and slow tasks for load testing without artificial sleeps.

### Watch Failure Recovery

```bash
# With tasks in flight, kill a worker (Ctrl+C in its terminal)
# The coordinator will detect the missed heartbeat within 6 seconds and requeue its tasks
# Start a new worker — it picks up the orphaned tasks automatically
./worker --coordinator localhost:50051 --port 50052
```

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
| `--capacity` | 2 | Max concurrent tasks |

### Client
| Flag | Default | Description |
|------|---------|-------------|
| `--coordinator, -c` | localhost:50051 | Coordinator address |
| `--payload` | (required for single) | Number to check for primality |
| `--priority` | 1 (NORMAL) | 0=LOW, 1=NORMAL, 2=HIGH, 3=URGENT |
| `--id` | — | Task ID (for status command) |
| `--batch N` | — | Submit N tasks with auto-generated payloads |
| `--random-priority` | — | Assign random priority (0–3) to each batch task |

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
│   ├── scheduler.h             # Task dispatch + health check loop
│   ├── task_store.h            # Thread-safe task state management
│   └── worker_pool.h           # Worker registry + load tracking
├── worker/
│   └── main.cpp                # Worker server + prime checking
└── client/
    └── main.cpp                # CLI for submit, status, batch load test
```

---

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| RPC framework | gRPC | Mirrors Google's internal Stubby; shows protobuf fluency |
| Routing strategy | Least-loaded + random tie-breaking | Even distribution under equal load without deterministic bias |
| Task queue | Priority queue (max-heap) | Higher-priority tasks preempt lower-priority ones; ties broken by submission time |
| Thread safety | `shared_mutex` | Read-heavy workload (status queries) benefits from shared locks |
| Task execution | Prime checking via trial division | Real computation with naturally variable execution time; no artificial sleeps |
| State management | In-memory | Intentional simplification; persistence is a documented non-goal |
| Failure detection | Heartbeat timeout (6s) + execution timeout (3s) | Heartbeat catches dead workers; execution timeout catches live workers that silently fail to report |

---

## Fault Tolerance

The system has two independent recovery mechanisms:

**Heartbeat-based detection** — Workers send heartbeats every 2 seconds. If the coordinator receives no heartbeat for 6 seconds (3 missed intervals), the worker is marked dead and all its in-flight tasks are requeued.

**Execution timeout** — If a task stays in `ASSIGNED` or `RUNNING` for more than 3 seconds, the health checker requeues it regardless of worker health. This catches the case where a worker is alive and heartbeating but fails to report a result (e.g., `ReportResult` RPC times out under coordinator load).

**Re-registration handling** — If a worker restarts and re-registers before the heartbeat timeout fires, the coordinator detects the re-registration and immediately requeues any tasks that were in-flight on the previous process instance.

**Assignment failure handling** — If a `AssignTask` RPC fails with a network error (worker unreachable), the coordinator immediately marks the worker dead and requeues the task, rather than waiting for the heartbeat timeout.

---

## What's Next (Phase 3)

- **Terminal metrics** — Real-time throughput, p50/p95/p99 latency, queue depth printed every 5 seconds
- **Graceful shutdown** — Workers drain in-flight tasks before exiting on SIGTERM
- **GetMetrics RPC** — Programmatic access to coordinator metrics



Created by David Ha