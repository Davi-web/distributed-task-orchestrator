# Distributed Task Orchestrator

A C++ distributed task orchestration system built with gRPC, demonstrating load-aware routing, fault-tolerant task management, and low-latency coordination across independent processes.

Built to mirror the infrastructure patterns used by orchestration teams at scale — routing work to available nodes, detecting failures, and recovering automatically without manual intervention.

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

The **Coordinator** receives tasks from clients, routes them to the least-loaded worker via gRPC, tracks task lifecycle (`PENDING → ASSIGNED → RUNNING → COMPLETED/FAILED`), and automatically recovers from worker failures.

**Workers** register with the coordinator, receive task assignments, execute them (prime checking via trial division), and report results back. Each worker runs a heartbeat loop so the coordinator can detect failures.

The **Client CLI** submits tasks, polls for status, and runs batch load tests with aggregate performance reporting.

---

## Quick Start

### Prerequisites

```bash
# Ubuntu / WSL2
sudo apt-get update
sudo apt-get install -y cmake g++ protobuf-compiler libprotobuf-dev \
    libgrpc++-dev libgrpc-dev protobuf-compiler-grpc pkg-config
```

### Build

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

Produces three binaries: `coordinator`, `worker`, `client`.

### Run

Open 3+ terminals:

```bash
# Terminal 1 — Coordinator
./coordinator --port 50051

# Terminal 2 — Worker (start as many as you want on different ports)
./worker --coordinator localhost:50051 --port 50052
./worker --coordinator localhost:50051 --port 50053

# Terminal 3 — Submit work
./client submit --payload 999999937 --priority 2          # single task
./client status --id <task-id>                             # check result
./client submit --batch 500 --random-priority              # load test
```

### Watch Failure Recovery

With tasks in flight, kill a worker with `Ctrl+C`. Within 6 seconds the coordinator detects the missed heartbeats, marks the worker dead, and requeues its tasks to surviving workers. Start a replacement worker on the same port — it picks up the orphaned work automatically.

---

## How It Works

### Task Routing

The coordinator maintains a priority queue (max-heap) of pending tasks. When a worker has capacity, the scheduler pops the highest-priority task and sends it via gRPC. Among workers with equal load, selection is randomized to prevent deterministic bias toward a single node.

### Fault Tolerance

The system uses two independent recovery mechanisms:

**Heartbeat timeout** — Workers heartbeat every 2 seconds. Three missed intervals (6 seconds) triggers automatic failure detection. All in-flight tasks on the dead worker are requeued.

**Execution timeout** — Tasks stuck in `ASSIGNED` or `RUNNING` for more than 3 seconds are requeued regardless of worker health. This catches the case where a worker is alive and heartbeating but fails to report a result — for example, when `ReportResult` RPCs time out under coordinator thread pool saturation.

**Re-registration** — If a worker restarts and re-registers before the heartbeat timeout fires, the coordinator immediately requeues any tasks from the previous process instance. No work is lost.

**Assignment failure** — If an `AssignTask` RPC fails with a network error, the coordinator marks the worker dead immediately rather than waiting for heartbeat timeout, and requeues the task.

### Task Execution

Workers perform prime checking via trial division on the integer payload. Execution time scales naturally with input size — small numbers complete in microseconds, large primes take tens of milliseconds. This provides a realistic mix of fast and slow tasks for load testing without artificial sleeps.

### Observability

The coordinator prints live metrics to the terminal every 5 seconds: throughput (tasks/sec), p50/p95/p99 latency, active worker count, and queue depth. Latency percentiles are calculated from a circular buffer of the most recent completions, avoiding unbounded memory growth. The same data is available programmatically via the `GetMetrics` RPC for integration with external monitoring.

### Graceful Shutdown

Workers handle `SIGTERM` by draining in-flight tasks before exiting. When a worker receives a shutdown signal, it stops accepting new assignments, waits for currently executing tasks to complete and report their results, then deregisters cleanly. This prevents unnecessary task requeuing and reduces recovery churn during planned maintenance or scaling events.

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
| `--batch N` | — | Submit N tasks with random payloads |
| `--random-priority` | — | Random priority per task in batch mode |

---

## Project Structure

```
distributed-task-orchestrator/
├── CMakeLists.txt
├── README.md
├── proto/
│   └── orchestrator.proto      # gRPC service + message definitions
├── common/
│   └── utils.h                 # UUID generation, timestamps, logging
├── coordinator/
│   ├── main.cpp                # Server setup, heartbeat monitor
│   ├── services.h/.cpp         # gRPC handler implementations
│   ├── scheduler.h             # Task dispatch + health checking
│   ├── task_store.h            # Thread-safe task state machine
│   └── worker_pool.h           # Worker registry + load tracking
├── worker/
│   └── main.cpp                # Worker server + prime checking
└── client/
    └── main.cpp                # CLI: submit, status, batch load test
```

---

## Design Decisions

| Decision | Choice | Trade-off |
|----------|--------|-----------|
| RPC framework | gRPC + Protobuf | Binary serialization is faster than JSON/REST; generated stubs provide compile-time type safety; mirrors Google's internal Stubby |
| Routing | Least-loaded with random tie-breaking | Simple, effective, avoids hot-spotting. Consistent hashing would add affinity but increases complexity |
| Task queue | Priority max-heap | O(log n) insert/pop. Stale entries from requeued tasks handled by lazy deletion on pop |
| Concurrency | `std::shared_mutex` for reads, `std::mutex` for writes | Status queries (read-heavy) don't block each other; task mutations are exclusive |
| Failure detection | Dual timeout (heartbeat + execution) | Heartbeat catches dead processes; execution timeout catches silent failures on live processes. Two independent mechanisms means no single point of failure in recovery |
| Task work | Prime checking (trial division) | Real computation with naturally variable cost. No artificial sleeps — execution time is a function of input, which makes load testing results meaningful |
| State | In-memory only | Intentional. Persistence would add complexity without demonstrating distributed coordination |

---

## Known Limitations

- **Single coordinator** — The coordinator is a single point of failure. Production systems would use leader election (Raft/Paxos) for HA. The architecture is designed so a consensus layer could be added beneath the `TaskStore` and `WorkerPool` without changing the service interfaces.
- **Sync gRPC server** — Under burst load (2000+ fast tasks), the coordinator's sync thread pool can saturate, causing `ReportResult` timeouts. The execution timeout recovers from this, but an async gRPC server would eliminate the issue entirely.
- **No task deduplication** — Each submission creates a new task regardless of payload. Idempotency would require a client-provided idempotency key and a dedup cache.
- **In-memory state** — Coordinator crash loses all task state. A write-ahead log or replicated state machine would add durability.

---

## Future Improvements

- **Async gRPC server** — The sync server saturates under burst load (2000+ fast tasks), causing `ReportResult` timeouts that the execution timeout recovers from. An async server would eliminate this class of failure entirely.
- **Consistent hashing** — Route tasks with affinity keys to the same worker for cache locality. Would require a hash ring with virtual nodes and fallback on worker failure.
- **Multi-coordinator HA** — Leader election via Raft or Paxos to eliminate the single coordinator as a point of failure. The current architecture isolates state in `TaskStore` and `WorkerPool`, so a consensus layer could be added beneath them without changing service interfaces.
- **Task deduplication** — Client-provided idempotency keys with a coordinator-side dedup cache to prevent duplicate execution on retry.

---

Built by David Ha