# PRD: Distributed Task Orchestrator

## 1. Overview

- **Version:** 1.0
- **Date:** March 20, 2026
- **Author:** [Your Name]
- **Status:** Draft

### Problem Statement

The Google Unified Orchestration team builds infrastructure that routes incoming queries to agentic AI systems, performs query understanding and ranking, and manages data injection pipelines. Engineers on this team need deep expertise in C++, distributed systems, and low-latency service design. Most portfolio projects fail to demonstrate these skills simultaneously — frontend-heavy apps, Python-only projects, and CRUD applications provide no meaningful signal for this kind of infrastructure role.

There is no lightweight, self-contained project that clearly maps to the orchestration problem domain while showcasing the exact technical stack Google evaluates: modern C++, gRPC, distributed coordination, fault tolerance, and performance-aware design. Candidates need a project that an interviewer can look at and immediately connect to the work the team does day to day.

This PRD defines a distributed task orchestration system where clients submit jobs, a coordinator routes them to worker nodes using load-aware scheduling, and workers execute and report back — directly mirroring what the Unified Orchestration team builds at production scale. The system is scoped to be buildable in approximately 2,000–3,000 lines of C++ while still being substantive enough to anchor a 45-minute technical interview.

### Target Users

The primary audience is a Google interviewer or hiring manager evaluating the candidate's systems engineering ability. The secondary audience is the candidate themselves, who will use the project as a learning vehicle for distributed systems concepts and as a talking point during behavioral and technical interviews.

---

## 2. Goals & Non-Goals

### Business Goals

- Demonstrate C++17/20 fluency in a systems programming context that matches the job description's required qualifications
- Showcase distributed systems fundamentals (coordination, routing, fault tolerance) aligned with the team's preferred qualifications
- Produce a clean, well-documented codebase that can be walked through in an interview setting

### User Goals

- Give the candidate a concrete, working system they fully understand and can explain at any level of abstraction
- Provide measurable performance data (throughput, latency percentiles) that demonstrates engineering rigor
- Create a project that is visually demonstrable — not just code on GitHub, but a system you can run and observe

### Non-Goals (Out of Scope)

- **Production-grade security (TLS, auth)** — Rationale: Adds complexity without demonstrating the target skills; interviewers won't evaluate this for a portfolio project
- **Persistent storage / database integration** — Rationale: This is an in-memory coordination system; adding a DB shifts focus away from distributed systems toward data engineering
- **Multi-language support** — Rationale: The entire point is demonstrating C++ proficiency; polyglot implementations dilute that signal
- **Kubernetes / container orchestration** — Rationale: Deployment infrastructure is orthogonal to the distributed systems design being showcased
- **UI / web dashboard** — Rationale: Frontend work is explicitly called out as something to avoid for this role; CLI output and terminal metrics are sufficient

---

## 3. User Personas

### Key User Types

- **The Candidate (Builder):** An SWE preparing for a Google L4/L5 infrastructure role. Intermediate-to-advanced C++ skills, familiar with networking concepts, wants a project that ties together distributed systems theory with working code. Needs clear milestones so the project doesn't spiral in scope.
- **The Interviewer (Evaluator):** A senior Google engineer reviewing the candidate's GitHub or discussing the project in an interview. Has deep systems expertise and will probe design decisions, trade-offs, and failure modes. Needs to quickly understand the architecture and find interesting technical depth to discuss.
- **The Recruiter (Screener):** A non-technical or semi-technical person scanning the candidate's portfolio. Needs a clear README, a compelling project description, and obvious relevance to the target role. Won't read the code but will read the documentation.

### Role-Based Access

| Role | Permissions | Key Actions |
|------|-------------|-------------|
| Client | Submit tasks, query status | Send task payloads, poll for completion, retrieve results |
| Coordinator | Full system control | Accept tasks, route to workers, monitor health, reassign failed tasks |
| Worker | Execute assigned work | Register with coordinator, send heartbeats, execute tasks, report results |

---

## 4. User Stories

### US-001: Submit a Task

- **As a** client
- **I want** to submit a task with a priority level and payload to the coordinator
- **So that** it gets queued and eventually executed by a worker

**Acceptance Criteria:**
- Client sends a `SubmitTask` RPC with priority (1–10) and a string payload
- Coordinator returns a unique task ID immediately (< 5ms response time)
- Task enters `PENDING` state in the coordinator's internal tracking
- Submitting with an empty payload returns a gRPC `INVALID_ARGUMENT` error

### US-002: Poll Task Status

- **As a** client
- **I want** to check the current status of a previously submitted task
- **So that** I know whether it's pending, running, completed, or failed

**Acceptance Criteria:**
- `GetTaskStatus` returns the current state: `PENDING`, `ASSIGNED`, `RUNNING`, `COMPLETED`, or `FAILED`
- For completed tasks, the response includes the result payload
- Querying a non-existent task ID returns `NOT_FOUND`

### US-003: Route Task to Least-Loaded Worker

- **As a** coordinator
- **I want** to assign each task to the worker with the lowest current load
- **So that** work is distributed evenly and no single worker becomes a bottleneck

**Acceptance Criteria:**
- When multiple workers are available, the task is assigned to the one with the fewest in-flight tasks
- When all workers have equal load, assignment is round-robin
- When no workers are available, the task remains `PENDING` until one registers
- Load metrics are updated immediately on assignment and on task completion

### US-004: Detect Worker Failure

- **As a** coordinator
- **I want** to detect when a worker has stopped responding
- **So that** I can reassign its in-flight tasks to healthy workers

**Acceptance Criteria:**
- Workers send heartbeats every 2 seconds
- If no heartbeat is received for 3 consecutive intervals (6 seconds), the worker is marked `DEAD`
- All tasks assigned to a dead worker are moved back to `PENDING` and re-queued
- When the dead worker comes back online and re-registers, it is treated as a new worker

### US-005: Execute and Report Task Results

- **As a** worker
- **I want** to receive a task, execute it, and report the result back to the coordinator
- **So that** clients can retrieve their completed results

**Acceptance Criteria:**
- Worker receives task via `AssignTask` RPC and moves it to `RUNNING`
- Execution is simulated with a configurable sleep duration (100ms–2s) to mimic real workloads
- On completion, worker sends `ReportResult` with the task ID and output
- If execution fails (simulated), worker reports `FAILED` status with an error message

### US-006: Register and Deregister Workers

- **As a** worker
- **I want** to announce myself to the coordinator when I start and be removed when I stop
- **So that** the coordinator maintains an accurate view of available capacity

**Acceptance Criteria:**
- On startup, worker calls `RegisterWorker` with its address and capability tags
- Coordinator adds worker to the active pool and begins expecting heartbeats
- On graceful shutdown, worker calls `DeregisterWorker` and coordinator drains its tasks
- Duplicate registration from the same address is treated as a re-register (reset health state)

### US-007: View System Metrics

- **As a** candidate demoing the system
- **I want** to see real-time throughput and latency statistics printed to the terminal
- **So that** I can demonstrate the system's performance characteristics in an interview

**Acceptance Criteria:**
- Coordinator prints metrics every 5 seconds: tasks/sec, p50/p95/p99 latency, active workers, queue depth
- Metrics reset on each reporting interval (sliding window)
- Metrics are also available via a `GetMetrics` RPC for programmatic access

---

## 5. Feature Prioritization (MoSCoW)

### Must Have (MVP Critical)

| ID | Feature | Description | Acceptance Criteria |
|----|---------|-------------|---------------------|
| FR-001 | gRPC Service Definitions | Proto files defining all service interfaces between client, coordinator, and worker | Compiles with `protoc`, generates C++ stubs, covers all 6 RPCs |
| FR-002 | Coordinator Server | Central process that accepts tasks, manages worker pool, routes assignments | Runs as a standalone binary, handles concurrent client and worker connections |
| FR-003 | Worker Server | Process that registers, heartbeats, executes tasks, reports results | Runs as a standalone binary, executes simulated work, reconnects on coordinator restart |
| FR-004 | Client CLI | Command-line tool to submit tasks and query status | Supports `submit --priority N --payload "..."` and `status --id TASK_ID` commands |
| FR-005 | Task State Machine | Internal coordinator tracking of task lifecycle | Tasks transition through PENDING → ASSIGNED → RUNNING → COMPLETED/FAILED with no invalid transitions |
| FR-006 | Load-Aware Routing | Coordinator assigns tasks to the least-loaded worker | Verifiable by running N workers with different simulated speeds and observing even distribution |

### Should Have (Important)

| ID | Feature | Description | Acceptance Criteria |
|----|---------|-------------|---------------------|
| FR-007 | Heartbeat & Failure Detection | Workers heartbeat; coordinator detects dead workers | Dead worker's tasks reassigned within 2 heartbeat intervals |
| FR-008 | Task Reassignment | Failed or orphaned tasks return to the queue | Tasks from dead workers re-enter PENDING and complete on another worker |
| FR-009 | Priority Queue | Higher-priority tasks are scheduled before lower-priority ones | Submitting priority-10 and priority-1 tasks results in priority-10 executing first |
| FR-010 | Terminal Metrics | Real-time throughput and latency stats printed to stdout | Shows tasks/sec, p50/p95/p99 latency, worker count, queue depth every 5 seconds |

### Could Have (Nice-to-Have)

| ID | Feature | Description | Acceptance Criteria |
|----|---------|-------------|---------------------|
| FR-011 | Consistent Hashing | Route tasks with affinity keys to the same worker | Tasks with the same key go to the same worker unless it fails |
| FR-012 | Load Test Script | Automated script that floods the system with tasks | Generates configurable load (100–10,000 tasks) and reports aggregate metrics |
| FR-013 | Graceful Shutdown | Workers drain in-flight tasks before exiting | SIGTERM triggers drain; worker only exits after current task completes |
| FR-014 | GetMetrics RPC | Programmatic access to coordinator metrics | Returns JSON-serializable metrics matching terminal output |

### Won't Have (Out of Scope)

- **Task persistence / WAL** — Rationale: In-memory only; persistence adds storage complexity without demonstrating distributed coordination skills
- **Multi-coordinator (leader election)** — Rationale: Would require implementing Raft/Paxos; valuable but too large for a portfolio project scope
- **Authentication / TLS** — Rationale: Security infrastructure doesn't demonstrate the target skills
- **Web-based dashboard** — Rationale: Frontend work is explicitly not relevant for this role

---

## 6. User Experience

### Entry Points & First-Time Flow

1. Clone the repo, run `cmake --build build` — single build command produces all three binaries
2. Start the coordinator: `./build/coordinator --port 50051`
3. Start 1–N workers: `./build/worker --coordinator localhost:50051 --port 50052`
4. Submit tasks via CLI: `./build/client submit --priority 5 --payload "process_image_batch_42"`
5. Observe coordinator terminal printing live metrics as tasks flow through the system

### Core Experience

| Step | Action | System Response | Success State |
|------|--------|-----------------|---------------|
| 1 | Start coordinator | Prints "Coordinator listening on :50051" | Process running, gRPC server accepting connections |
| 2 | Start worker(s) | Worker prints "Registered with coordinator"; coordinator prints "Worker {addr} registered" | Worker appears in coordinator's active pool |
| 3 | Submit task via client | Client prints "Task submitted: {task_id}"; coordinator prints "Task {id} queued (priority: N)" | Task enters PENDING state |
| 4 | Coordinator routes task | Coordinator prints "Task {id} → Worker {addr}"; worker prints "Received task {id}" | Task moves to ASSIGNED → RUNNING |
| 5 | Worker completes task | Worker prints "Task {id} completed"; coordinator prints "Task {id} COMPLETED (latency: Xms)" | Task moves to COMPLETED, result stored |
| 6 | Client polls status | Client prints task state and result payload | Client receives COMPLETED + result |
| 7 | Kill a worker (Ctrl+C) | Coordinator prints "Worker {addr} missed heartbeat... marked DEAD"; reassigns tasks | Orphaned tasks return to PENDING, picked up by another worker |

### Edge Cases & Error States

- **No workers available:** Tasks remain in PENDING; coordinator logs "No workers available, task {id} queued". Tasks are assigned as soon as a worker registers.
- **Worker dies mid-task:** Heartbeat timeout triggers failure detection. In-flight tasks move to PENDING and are reassigned. Client polling sees state revert from RUNNING to PENDING.
- **Duplicate task submission:** Each submission gets a unique UUID regardless of payload content. No deduplication (intentional simplification).
- **Coordinator crash:** All state is lost (in-memory only). Workers detect broken connection, retry registration on configurable interval. Documented as a known limitation in README.
- **Client queries invalid task ID:** Returns gRPC `NOT_FOUND` status with descriptive error message.
- **Worker re-registers after being marked dead:** Treated as fresh registration. Previously reassigned tasks are not sent back.

*Pages & Screens section omitted — this is a CLI/backend service with no UI.*

---

## 7. Technical Considerations

### Integration Points

- **gRPC + Protocol Buffers:** All inter-process communication. Proto files are the source of truth for the API contract.
- **CMake:** Build system. Single `CMakeLists.txt` with targets for coordinator, worker, and client binaries.
- **No external dependencies beyond gRPC/protobuf.** No databases, no message queues, no cloud services.

### Data Storage & Privacy

- **All data is in-memory only.** Task state, worker registry, and metrics live in coordinator memory.
- **No persistence.** Coordinator restart means full state loss. This is an intentional scoping decision.
- **No user data.** Task payloads are opaque strings used for simulation. No PII or sensitive data.
- **No retention requirements.** Completed task results are kept in memory until coordinator shutdown.

### Performance Requirements

- **Task submission latency:** < 5ms p99 (coordinator enqueue time, not execution time)
- **Routing decision latency:** < 1ms (selecting a worker from the pool)
- **Heartbeat overhead:** < 0.1% CPU per worker at 2-second intervals
- **Throughput target:** System should sustain 1,000+ tasks/sec with 5 workers on a single machine
- **Failure detection:** Dead worker identified within 6 seconds (3 missed heartbeats at 2s intervals)
- **Task reassignment:** Orphaned tasks re-queued within 1 second of failure detection

### Potential Challenges

- **Thread safety in the coordinator:** The coordinator handles concurrent gRPC calls from multiple clients and workers simultaneously. The task queue, worker registry, and metrics collector all need proper synchronization. Plan: use `std::shared_mutex` for read-heavy structures (worker registry) and `std::mutex` for write-heavy ones (task queue).
- **gRPC async vs sync server:** Sync is simpler but blocks a thread per connection. Async is more performant but significantly more complex. Plan: start with sync server + thread pool; migrate to async only if throughput testing reveals it as a bottleneck.
- **Accurate latency measurement:** `std::chrono::steady_clock` for timestamps. Percentile calculation with a fixed-size circular buffer (last 10,000 tasks) to avoid unbounded memory growth.
- **Simulating realistic failure modes:** Need to test worker death mid-task, network partitions (simulated via sleep), and slow workers. Plan: add `--fail-rate` and `--slow-rate` flags to the worker binary for chaos testing.
- **CMake + gRPC build setup:** Getting gRPC and protobuf to compile cleanly with CMake is notoriously finicky. Plan: use `FetchContent` or document `vcpkg`/system package setup clearly in the README.

---

## 8. Success Metrics

### User-Centric Metrics (Interview Effectiveness)

- **README clarity:** A reader (recruiter or engineer) can understand what the project does, why it matters, and how to run it within 2 minutes — measured by peer review
- **Interview talking points:** The project generates at least 5 substantive discussion topics (routing strategy, failure handling, latency measurement, thread safety, gRPC design) — measured by mock interview
- **Code walkability:** An interviewer can open any file and understand its purpose within 30 seconds — measured by consistent naming, clear structure, and inline comments on non-obvious decisions

### Technical Metrics

- **Throughput:** ≥ 1,000 tasks/sec sustained with 5 workers — measured by load test script (FR-012)
- **Submission latency (p99):** < 5ms — measured by client-side timestamps
- **End-to-end latency (p99):** < 500ms with default simulated execution time — measured by coordinator metrics
- **Failure recovery time:** < 8 seconds from worker death to orphaned task reassignment — measured by chaos test
- **Build time:** < 60 seconds from clean build on a modern machine — measured by `time cmake --build build`
- **Binary size:** < 50MB per binary (statically linked) — measured by `ls -lh`
- **Lines of code:** 2,000–3,000 total across all source files — measured by `cloc`

---

## 9. Milestones

### MVP Scope

Based on Must Have requirements: FR-001 through FR-006. A working system where clients submit tasks, the coordinator routes them to workers using load-aware scheduling, and workers execute and report back. Single coordinator, multiple workers, all running locally.

### Phase 1: MVP

**Complexity:** Medium (estimated 1–2 weeks)

- FR-001: Proto definitions and generated C++ stubs
- FR-002: Coordinator server with task queue and worker registry
- FR-003: Worker server with registration and task execution
- FR-004: Client CLI with submit and status commands
- FR-005: Task state machine (PENDING → ASSIGNED → RUNNING → COMPLETED/FAILED)
- FR-006: Load-aware routing (least-loaded worker selection)
- Deliverable: All three binaries compile and run; tasks flow end-to-end

### Phase 2: Fault Tolerance

**Complexity:** Medium (estimated 1 week)

- FR-007: Heartbeat and failure detection
- FR-008: Task reassignment on worker failure
- FR-009: Priority queue scheduling
- Deliverable: System recovers gracefully when workers are killed mid-task

### Phase 3: Observability & Polish

**Complexity:** Small (estimated 3–5 days)

- FR-010: Terminal metrics (throughput, latency percentiles, queue depth)
- FR-012: Load test script for demo and benchmarking
- FR-013: Graceful worker shutdown with task draining
- FR-014: GetMetrics RPC
- README with architecture diagram, build instructions, and demo walkthrough
- Deliverable: System is demo-ready and documented for GitHub

---

## 10. Open Questions

- **Async vs sync gRPC server:** Should Phase 1 start with sync for simplicity, or invest in async upfront? Recommendation: sync first, profile later.
- **Task payload semantics:** Should workers actually parse and "do something" with payloads (e.g., string reversal, prime calculation), or is simulated sleep sufficient? Simulated sleep is simpler but real computation is more demo-friendly.
- **Worker capability tags:** Should routing consider worker capabilities (e.g., "GPU" vs "CPU" tasks), or is load-only routing sufficient for the portfolio scope? Capability routing is a good Phase 3 extension but may over-scope MVP.
- **Coordinator high availability:** Should the README discuss how you *would* add multi-coordinator support (Raft, leader election) even if it's out of scope? This could be a strong interview talking point.
- **CI/CD:** Is a GitHub Actions workflow for build + test worth including, or is a Makefile sufficient? CI adds polish but costs setup time.
- **Testing strategy:** Unit tests for the coordinator logic (routing, state machine) vs integration tests (multi-process gRPC flows) vs both? Recommendation: unit tests for coordinator internals, one integration test script that boots the full system.