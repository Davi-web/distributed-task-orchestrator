# PRD: Distributed Task Orchestrator

## 1. Overview

- **Version:** 1.1
- **Date:** March 20, 2026
- **Author:** David Ha
- **Status:** Phase 2 Complete

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
- Client sends a `SubmitTask` RPC with priority (0–3) and a numeric string payload
- Coordinator returns a unique task ID immediately (< 5ms response time)
- Task enters `PENDING` state in the coordinator's internal tracking
- Submitting with an empty payload returns a gRPC `INVALID_ARGUMENT` error

### US-002: Poll Task Status

- **As a** client
- **I want** to check the current status of a previously submitted task
- **So that** I know whether it's pending, running, completed, or failed

**Acceptance Criteria:**
- `GetTaskStatus` returns the current state: `PENDING`, `ASSIGNED`, `RUNNING`, `COMPLETED`, or `FAILED`
- For completed tasks, the response includes the result payload and end-to-end latency
- Querying a non-existent task ID returns `NOT_FOUND`

### US-003: Route Task to Least-Loaded Worker

- **As a** coordinator
- **I want** to assign each task to the worker with the lowest current load
- **So that** work is distributed evenly and no single worker becomes a bottleneck

**Acceptance Criteria:**
- When multiple workers are available, the task is assigned to the one with the fewest in-flight tasks
- When multiple workers are tied for lowest load, assignment is random among tied workers (prevents deterministic bias toward the same worker)
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
- If a worker re-registers before the 6-second timeout, any tasks in-flight on the previous process instance are immediately requeued (the new process has no memory of them)

### US-005: Execute and Report Task Results

- **As a** worker
- **I want** to receive a task, execute it, and report the result back to the coordinator
- **So that** clients can retrieve their completed results

**Acceptance Criteria:**
- Worker receives task via `AssignTask` RPC
- Task payload is a positive integer; worker determines whether it is prime using trial division
- Result is `"N is prime"` or `"N is not prime (smallest factor: X)"`
- On completion, worker sends `ReportResult` with the task ID and result string
- If `ReportResult` fails transiently, the worker retries with exponential backoff before giving up
- Invalid payloads (non-numeric strings) result in a `FAILED` status with a descriptive error

### US-006: Register Workers

- **As a** worker
- **I want** to announce myself to the coordinator when I start
- **So that** the coordinator maintains an accurate view of available capacity

**Acceptance Criteria:**
- On startup, worker calls `RegisterWorker` with its address and capacity
- Coordinator adds worker to the active pool and begins expecting heartbeats
- If a worker with the same ID re-registers, the coordinator requeues any in-flight tasks from the previous incarnation and resets that worker's health state

### US-007: Load Test the System

- **As a** candidate demoing the system
- **I want** to flood the system with hundreds of tasks at once
- **So that** I can observe routing, queuing, and throughput in real time

**Acceptance Criteria:**
- `--batch N` submits N tasks in rapid succession and waits for all to complete
- `--random-priority` assigns a random priority (0–3) to each task, exercising the priority queue
- On completion, client prints: tasks submitted, completed, failed, total wall time, avg/min/max latency

---

## 5. Feature Prioritization (MoSCoW)

### Must Have (MVP Critical)

| ID | Feature | Status | Description | Acceptance Criteria |
|----|---------|--------|-------------|---------------------|
| FR-001 | gRPC Service Definitions | ✅ Done | Proto files defining all service interfaces between client, coordinator, and worker | Compiles with `protoc`, generates C++ stubs, covers all RPCs |
| FR-002 | Coordinator Server | ✅ Done | Central process that accepts tasks, manages worker pool, routes assignments | Runs as a standalone binary, handles concurrent client and worker connections |
| FR-003 | Worker Server | ✅ Done | Process that registers, heartbeats, executes tasks, reports results | Runs as a standalone binary, checks primality of payload, reports result |
| FR-004 | Client CLI | ✅ Done | Command-line tool to submit tasks and query status | Supports `submit --priority N --payload "..."` and `status --id TASK_ID` commands |
| FR-005 | Task State Machine | ✅ Done | Internal coordinator tracking of task lifecycle | Tasks transition through PENDING → ASSIGNED → RUNNING → COMPLETED/FAILED with no invalid transitions |
| FR-006 | Load-Aware Routing | ✅ Done | Coordinator assigns tasks to the least-loaded worker | Random tie-breaking among equally-loaded workers ensures even distribution |

### Should Have (Important)

| ID | Feature | Status | Description | Acceptance Criteria |
|----|---------|--------|-------------|---------------------|
| FR-007 | Heartbeat & Failure Detection | ✅ Done | Workers heartbeat; coordinator detects dead workers | Dead worker's tasks reassigned within 6 seconds |
| FR-008 | Task Reassignment | ✅ Done | Failed or orphaned tasks return to the queue | Tasks from dead workers re-enter PENDING and complete on another worker |
| FR-009 | Priority Queue | ✅ Done | Higher-priority tasks are scheduled before lower-priority ones | URGENT tasks execute before LOW tasks; equal priorities dispatched FIFO |
| FR-010 | Terminal Metrics | 🔲 Planned | Real-time throughput and latency stats printed to stdout | Shows tasks/sec, p50/p95/p99 latency, worker count, queue depth every 5 seconds |

### Could Have (Nice-to-Have)

| ID | Feature | Status | Description | Acceptance Criteria |
|----|---------|--------|-------------|---------------------|
| FR-011 | Consistent Hashing | 🔲 Planned | Route tasks with affinity keys to the same worker | Tasks with the same key go to the same worker unless it fails |
| FR-012 | Load Test Script | ✅ Done | Batch submit mode that floods the system with tasks | `--batch N --random-priority` generates N tasks and reports aggregate metrics on completion |
| FR-013 | Graceful Shutdown | 🔲 Planned | Workers drain in-flight tasks before exiting | SIGTERM triggers drain; worker only exits after current task completes |
| FR-014 | GetMetrics RPC | 🔲 Planned | Programmatic access to coordinator metrics | Returns metrics matching terminal output |

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
4. Submit tasks via CLI: `./build/client submit --payload 999999937 --priority 2`
5. Observe coordinator terminal printing task assignments and completions as tasks flow through the system

### Core Experience

| Step | Action | System Response | Success State |
|------|--------|-----------------|---------------|
| 1 | Start coordinator | Prints "Coordinator listening on :50051" | Process running, gRPC server accepting connections |
| 2 | Start worker(s) | Worker prints "Registered as worker-XXXX"; coordinator prints "Worker registered" | Worker appears in coordinator's active pool |
| 3 | Submit task via client | Client prints task ID; coordinator logs task queued | Task enters PENDING state |
| 4 | Coordinator routes task | Coordinator prints "Task → Worker"; worker prints "Checking prime: N" | Task moves to ASSIGNED |
| 5 | Worker completes task | Worker prints result; coordinator prints "Task COMPLETED (latency: Xms)" | Task moves to COMPLETED, result stored |
| 6 | Client polls status | Client prints task state and result | Client receives COMPLETED + result |
| 7 | Kill a worker (Ctrl+C) | Coordinator prints "Worker missed heartbeats — marked DEAD"; requeues tasks | Orphaned tasks return to PENDING, picked up by another worker |

### Edge Cases & Error States

- **No workers available:** Tasks remain in PENDING; coordinator logs "No workers available". Tasks are assigned as soon as a worker registers.
- **Worker dies mid-task:** Heartbeat timeout (6s) triggers failure detection. In-flight tasks requeued. Client polling sees task eventually complete on another worker.
- **Worker alive but ReportResult fails:** Task execution timeout (3s) fires in the health checker. Task is requeued even though the worker is still heartbeating. Catches coordinator thread pool saturation under burst load.
- **Worker restarts before heartbeat timeout:** Re-registration triggers immediate requeue of in-flight tasks from the previous process instance. No tasks are lost.
- **Assignment fails with network error:** Coordinator immediately marks the worker dead (does not wait for heartbeat timeout) and requeues the task. Prevents the scheduler from repeatedly selecting an unreachable worker.
- **Duplicate task submission:** Each submission gets a unique UUID regardless of payload content. No deduplication (intentional simplification).
- **Coordinator crash:** All state is lost (in-memory only). Workers detect broken connection on next heartbeat. Documented as a known limitation.
- **Client queries invalid task ID:** Returns gRPC `NOT_FOUND` status with descriptive error message.
- **Non-numeric payload:** Worker returns `FAILED` state with error "payload must be a positive integer".

---

## 7. Technical Considerations

### Integration Points

- **gRPC + Protocol Buffers:** All inter-process communication. Proto files are the source of truth for the API contract.
- **CMake:** Build system. Single `CMakeLists.txt` with targets for coordinator, worker, and client binaries.
- **No external dependencies beyond gRPC/protobuf.** No databases, no message queues, no cloud services.

### Data Storage & Privacy

- **All data is in-memory only.** Task state, worker registry, and metrics live in coordinator memory.
- **No persistence.** Coordinator restart means full state loss. This is an intentional scoping decision.
- **No user data.** Task payloads are integers used for prime checking. No PII or sensitive data.
- **No retention requirements.** Completed task results are kept in memory until coordinator shutdown.

### Performance Requirements

- **Task submission latency:** < 5ms p99 (coordinator enqueue time, not execution time)
- **Routing decision latency:** < 1ms (selecting a worker from the pool)
- **Heartbeat overhead:** < 0.1% CPU per worker at 2-second intervals
- **Throughput target:** System should sustain 1,000+ tasks/sec with 5 workers on a single machine (small payloads)
- **Failure detection:** Dead worker identified within 6 seconds (3 missed heartbeats at 2s intervals)
- **Execution timeout recovery:** Tasks stuck in ASSIGNED/RUNNING requeued within 3 seconds of timeout threshold
- **Task reassignment:** Orphaned tasks re-queued within 1 second of failure detection

### Potential Challenges

- **Thread safety in the coordinator:** The coordinator handles concurrent gRPC calls from multiple clients and workers simultaneously. The task queue, worker registry, and metrics collector all need proper synchronization. Plan: use `std::shared_mutex` for read-heavy structures (worker registry) and `std::mutex` for write-heavy ones (task queue).
- **gRPC sync server saturation:** Sync gRPC allocates one thread per in-flight RPC. Under burst load (many tasks completing simultaneously), the thread pool can be exhausted, causing `ReportResult` calls to time out on the worker side. Mitigated by the 3-second execution timeout which requeues stuck tasks automatically. Phase 3 may migrate to async gRPC if this remains a bottleneck.
- **Priority queue stale entries:** Tasks requeued multiple times accumulate duplicate entries in the priority queue. Handled by lazy deletion — `pop_pending` checks the task's current state and skips entries that are no longer PENDING.
- **Heartbeat/completion race condition:** The health checker can attempt to requeue a task that was just completed by the worker. Guarded by checking task state in `requeue_task` — COMPLETED and FAILED tasks cannot be requeued.
- **Accurate latency measurement:** `std::chrono::steady_clock` for timestamps. End-to-end latency = `completed_at_ms - created_at_ms` (submission to completion, including queue wait time).

---

## 8. Success Metrics

### User-Centric Metrics (Interview Effectiveness)

- **README clarity:** A reader (recruiter or engineer) can understand what the project does, why it matters, and how to run it within 2 minutes — measured by peer review
- **Interview talking points:** The project generates at least 5 substantive discussion topics (routing strategy, failure handling, execution timeout design, thread safety, gRPC design) — measured by mock interview
- **Code walkability:** An interviewer can open any file and understand its purpose within 30 seconds — measured by consistent naming, clear structure, and inline comments on non-obvious decisions

### Technical Metrics

- **Throughput:** ≥ 1,000 tasks/sec sustained with 5 workers (small payloads) — measured by `--batch` load test
- **Submission latency (p99):** < 5ms — measured by client-side timestamps
- **End-to-end latency:** Scales with payload size; sub-millisecond for composites, tens of milliseconds for large primes — measured by coordinator completion logs
- **Failure recovery time:** < 8 seconds from worker death to orphaned task reassignment — measured by kill test
- **Build time:** < 60 seconds from clean build on a modern machine — measured by `time cmake --build build`
- **Lines of code:** 2,000–3,000 total across all source files — measured by `cloc`

---

## 9. Milestones

### Phase 1: MVP ✅ Complete

- FR-001: Proto definitions and generated C++ stubs
- FR-002: Coordinator server with task queue and worker registry
- FR-003: Worker server with registration and task execution (prime checking)
- FR-004: Client CLI with submit and status commands
- FR-005: Task state machine (PENDING → ASSIGNED → RUNNING → COMPLETED/FAILED)
- FR-006: Load-aware routing (least-loaded worker, random tie-breaking)
- Deliverable: All three binaries compile and run; tasks flow end-to-end

### Phase 2: Fault Tolerance ✅ Complete

- FR-007: Heartbeat and failure detection (6-second timeout, 2-second interval)
- FR-008: Task reassignment on worker failure
- FR-009: Priority queue scheduling (4 priority levels, FIFO within priority)
- FR-012: Batch load test with `--batch N` and `--random-priority`
- Worker re-registration handling (requeue in-flight tasks before resetting state)
- Execution timeout (3s) for tasks stuck on live-but-silent workers
- Assignment failure fast-path (mark worker dead immediately on gRPC error, not after heartbeat timeout)
- Deliverable: System recovers from worker death, task execution hangs, and ReportResult failures

### Phase 3: Observability & Polish

- FR-010: Terminal metrics (throughput, latency percentiles, queue depth every 5 seconds)
- FR-013: Graceful worker shutdown with task draining
- FR-014: GetMetrics RPC
- README finalized with architecture diagram and demo walkthrough
- Deliverable: System is demo-ready and fully documented for GitHub

---

## 10. Open Questions

- **Async vs sync gRPC server:** The sync server saturates under burst load (2000+ fast tasks), causing `ReportResult` timeouts that the execution timeout recovers from. For Phase 3, consider migrating the coordinator to an async gRPC server to eliminate this class of failure entirely.
- **Execution timeout value:** Currently hardcoded at 3 seconds. Should this be configurable via `--task-timeout` on the coordinator? 3 seconds works for the current prime checking workload but would be wrong for legitimately long-running tasks.
- **Metrics storage:** Percentile calculation requires storing recent latencies. A fixed-size circular buffer (last 10,000 tasks) avoids unbounded memory growth.
- **Coordinator high availability:** The README should discuss how you *would* add multi-coordinator support (Raft, leader election) even though it's out of scope — this is a strong interview talking point.
- **Testing strategy:** Unit tests for coordinator logic (routing, state machine) vs integration tests (multi-process gRPC flows) vs both? Recommendation: unit tests for coordinator internals, one integration test script that boots the full system.
