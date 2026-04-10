# FLUX OS — Agent-First Onboarding Specification

> **Audience**: AI agents, automated tooling, CI/CD systems, operator agents
> **Format**: Structured specification with machine-readable examples
> **Version**: FLUX OS v0.1.0 compatible

---

## Purpose

This document provides a machine-readable onboarding specification for AI agents that want to interact with, develop for, or manage FLUX OS. Unlike the human onboarding guide, this document focuses on API contracts, structured workflows, capability negotiation, and programmatic interfaces that agents can consume without visual interpretation.

FLUX OS is designed to be agent-first: the kernel's syscall interface includes A2A (Agent-to-Agent) messaging as a first-class primitive, processes can optionally be autonomous agents, and the self-compiler can be invoked programmatically. This document tells your agent exactly how to use these capabilities.

---

## 1. System Identification

### 1.1 Version Detection

```json
{
  "flux_os": {
    "version": "0.1.0",
    "version_major": 0,
    "version_minor": 1,
    "version_patch": 0,
    "kernel_name": "FLUX Microkernel",
    "arch_flags": "hw_agnostic",
    "capabilities": [
      "self_compile",
      "agent_native",
      "bytecode_vm",
      "hot_swap",
      "ab_testing",
      "fleet_management",
      "cross_compile"
    ]
  }
}
```

### 1.2 Hardware Discovery

Agents can query hardware capabilities through the HAL to make compilation and deployment decisions:

```json
{
  "hal": {
    "arch": "arm64",
    "backend": "native",
    "version": "0.1.0",
    "cpu_features": {
      "mmu": true,
      "fpu": true,
      "neon": true,
      "cache_line_size": 64,
      "l1_size": 32768,
      "l2_size": 262144,
      "l3_size": 0
    },
    "memory": {
      "total_bytes": 1073741824,
      "free_bytes": 805306368
    }
  }
}
```

### 1.3 Syscall Interface

The kernel exposes 28 syscalls organized into 7 categories. An agent invokes syscalls through the `SYSCALL` instruction (opcode `0x74`) with the syscall number in register R60:

| Category | Syscalls | Numbers |
|----------|----------|---------|
| Process | SPAWN, YIELD, EXIT, KILL, WAIT, GETPID | 100-105 |
| Memory | ALLOC, FREE, MMAP, MUNMAP, MPROTECT | 200-204 |
| IPC | A2A_SEND, A2A_RECV, A2A_BROADCAST, A2A_SUBSCRIBE | 300-303 |
| Bytecode | BC_LOAD, BC_EXEC, BC_STATUS, BC_DUMP | 400-403 |
| Compiler | COMPILE, EMIT, DEVCODE | 500-502 |
| I/O | READ, WRITE, IOCTL, OPEN, CLOSE | 600-604 |
| Hardware | HW_INFO, HW_CONFIG | 700-701 |
| Info | INFO, LOG | 800-801 |

---

## 2. Agent Lifecycle

### 2.1 Spawning an Agent

An agent spawns a new agent via the `FLUX_SYSCALL_SPAWN` syscall or the `OP_SPAWN` (0x85) instruction. The spawning agent specifies:

```c
/* Spawn parameters (set in registers before SPAWN) */
R16 = (uint64_t)name_ptr;      /* Agent name string */
R17 = (uint64_t)model_ptr;      /* Execution model: "flux:bytecode", "native:x86" */
R18 = (uint64_t)capabilities;   /* Capability bitmask */
R19 = (uint64_t)bytecode_ptr;   /* Pointer to bytecode payload */
R20 = (uint64_t)bytecode_len;   /* Length of bytecode */
```

The kernel returns the new agent's ID in R8.

### 2.2 Capability Negotiation

Agents operate under a capability-based security model. When an agent is spawned, it receives an initial set of capabilities. It can request additional capabilities from its parent or from the supervisor agent via A2A messages of type `FLUX_A2A_CAPABILITY` (0x09).

```json
{
  "a2a_message": {
    "type": "CAPABILITY",
    "sender": 5,
    "target": 1,
    "payload": {
      "action": "request",
      "capability": "NETWORK",
      "reason": "Need to publish metrics to cloud endpoint"
    }
  }
}
```

The supervisor (agent ID 1) evaluates the request based on policy and responds with a grant or denial.

### 2.3 Agent Communication (A2A Protocol)

#### Message Format

All A2A messages use the `flux_a2a_msg_t` structure:

```c
typedef struct {
    uint32_t          msg_id;         /* Unique message identifier */
    uint32_t          reply_to;       /* ID of the message this replies to */
    flux_a2a_type_t   type;           /* Message type (TELL, ASK, DELEGATE, etc.) */
    uint32_t          sender;         /* Sender agent ID */
    uint32_t          target;         /* Target agent ID */
    flux_cap_t        required_cap;   /* Capability needed to receive */
    uint64_t          timestamp;      /* Timestamp in ticks */
    uint64_t          deadline;       /* Deadline in ticks (0 = no deadline) */
    uint32_t          priority;       /* Message priority */
    char              topic[64];      /* Topic for publish-subscribe */
    uint8_t           payload[4096];  /* Message payload */
    uint32_t          payload_len;    /* Payload length */
} flux_a2a_msg_t;
```

#### Communication Patterns

**Pattern 1: Tell (Fire-and-Forget)**
```
Agent A                    Agent B
  │                          │
  │── TELL(msg) ──────────→  │
  │                          │
  │   (no reply expected)    │
```

**Pattern 2: Ask (Request-Reply)**
```
Agent A                    Agent B
  │                          │
  │── ASK(msg) ───────────→  │
  │                          │
  │←── REPLY(msg) ─────────  │
  │                          │
```

**Pattern 3: Delegate (Task Offloading)**
```
Agent A                    Agent B
  │                          │
  │── DELEGATE(task) ─────→  │
  │                          │
  │   (A continues work)     │
  │                          │
  │←── RESULT(result) ─────  │
  │                          │
```

**Pattern 4: Broadcast (Publish-Subscribe)**
```
Agent A          Agent B          Agent C
  │                │                │
  │── SUBSCRIBE ─→ │                │
  │                │                │
  │── BROADCAST ──────────────→     │
  │                │                │
  │                │←── EVENT ──────│
  │←── EVENT ─────│                │
```

**Pattern 5: Barrier (Synchronization)**
```
Agent A          Agent B          Agent C
  │                │                │
  │── BARRIER ──────────────────────→
  │                │                │
  │   (waits)      │   (waits)      │   (waits)
  │                │                │
  │←── (all arrived) ───────────────│
  │                │                │
```

### 2.4 Agent States

```
CREATED → INITIALIZING → IDLE ⇄ THINKING → WAITING → DELEGATING
                              ↓
                          TERMINATED
                              ↓
                           FAILED
```

An agent transitions between states as follows:
- **CREATED**: Agent struct allocated, not yet running
- **INITIALIZING**: Running initialization code, publishing capabilities
- **IDLE**: Waiting for A2A messages (blocked on A2A_RECV)
- **THINKING**: Processing a task or reasoning about a decision
- **WAITING**: Waiting for a reply to an ASK or result of a DELEGATE
- **DELEGATING**: Has offloaded work, waiting for result while doing other things
- **TERMINATED**: Clean shutdown, resources freed
- **FAILED**: Terminated due to error, resources freed

---

## 3. Compilation Workflow

### 3.1 Programmatic Compilation

An agent can invoke the self-compiler through the `FLUX_SYSCALL_COMPILE` syscall. The compiler accepts FLUX.MD source (or FIR IR) and produces bytecode, C code, or native assembly depending on the target.

```
Input:  FLUX.MD source (markdown text)
        ↓
Step 1: Lexer → token stream
        ↓
Step 2: Parser → AST (abstract syntax tree)
        ↓
Step 3: FIR Generator → SSA IR (FIR module with functions and basic blocks)
        ↓
Step 4: Optimizer → optimized FIR (dead code elimination, constant folding, inlining)
        ↓
Step 5: Backend → output format
        ↓
Output: FLUX bytecode (.fluxbc) OR C source (.c) OR native assembly (.s)
```

### 3.2 DEVCODE: OS as Developer

The `FLUX_SYSCALL_DEVCODE` (502) syscall is the most unique capability of FLUX OS. An agent describes what it needs in natural language, and the OS generates working code:

```c
flux_devcode_ctx_t ctx;
memset(&ctx, 0, sizeof(ctx));
strncpy(ctx.intent, "Read temperature from I2C sensor at address 0x48, "
                     "convert to Celsius, and send to cloud agent",
        sizeof(ctx.intent));
strncpy(ctx.target_arch, "arm64", sizeof(ctx.target_arch));
strncpy(ctx.target_opt, "-Os", sizeof(ctx.target_opt));
ctx.use_simd = false;
ctx.use_parallel = false;

flux_compile_result_t result = flux_compiler_devcode(&ctx, &result);
```

The DEVCODE syscall is designed for agent-driven development: an operator agent can describe a new behavior, the OS generates the code, and the agent deploys it — all without human intervention.

### 3.3 Cross-Compilation by Architecture

The compiler uses HAL hardware info to generate optimal code. When an agent wants to cross-compile for a different architecture, it specifies the target and the compiler uses the known feature set of that architecture:

| Target | Compiler Backend | Notes |
|--------|-----------------|-------|
| `bytecode` | Bytecode Codegen | Portable, runs on any FLUX VM |
| `c` | C Codegen | Portable, targets any C compiler |
| `native` | Native Codegen (HAL) | Uses current HAL's CPU features |
| `x86_64` | x86_64 Codegen | SSE, AVX, AVX2, AVX512 when available |
| `arm64` | ARM64 Codegen | NEON SIMD when available |
| `riscv64` | RISC-V Codegen | RISC-V Vector when available |
| `wasm32` | WASM Codegen | Browser and edge WASM runtimes |

---

## 4. Fleet Management Protocol

### 4.1 Device Registration

```json
{
  "action": "register_device",
  "device": {
    "name": "greenhouse-pi-01",
    "fleet": "greenhouse",
    "arch": "arm64",
    "board": "rpi4",
    "capabilities": ["IO_READ", "NETWORK", "MEMORY"],
    "endpoint": "192.168.1.101:9090",
    "credentials": {
      "agent_id": 42,
      "capability_token": "0x...hex..."
    }
  }
}
```

### 4.2 Deployment Protocol

```json
{
  "action": "deploy",
  "target": {
    "fleet": "greenhouse",
    "strategy": "rolling",
    "batch_size": 0.25
  },
  "artifact": {
    "bytecode": "<base64-encoded-flux-bytecode>",
    "sha256": "abc123...",
    "version": "2.1.0",
    "metadata": {
      "source_hash": "def456...",
      "compile_time_ms": 342,
      "target_arch": "arm64",
      "optimize_level": "Os"
    }
  },
  "rollback_artifact": {
    "bytecode": "<base64-encoded-previous-version>",
    "sha256": "old123..."
  }
}
```

### 4.3 Hot-Swap Protocol

Hot-swapping replaces the running bytecode on a device without restarting the agent process. The protocol:

1. **Pre-load**: New bytecode is loaded into a shadow memory region
2. **Verify**: Checksum verification, capability validation, dependency check
3. **Atomic Swap**: PC and bytecode base pointer are atomically updated
4. **GC**: Old bytecode region is freed after confirmation

```json
{
  "action": "hot_swap",
  "device": "greenhouse-pi-01",
  "agent_id": 42,
  "new_bytecode": "<base64>",
  "new_bytecode_len": 48192,
  "sha256": "abc123...",
  "strategy": "atomic",
  "timeout_ms": 5000,
  "rollback_on_failure": true
}
```

### 4.4 A/B Test Protocol

```json
{
  "action": "ab_test_start",
  "test": {
    "name": "sensor-latency-test-001",
    "variant_a": {
      "artifact_ref": "sha256:abc123...",
      "weight": 0.5,
      "label": "control"
    },
    "variant_b": {
      "artifact_ref": "sha256:def456...",
      "weight": 0.5,
      "label": "treatment"
    },
    "fleet": "greenhouse",
    "duration_ticks": 86400000,
    "metrics": ["latency_p99", "memory_bytes", "error_rate", "cpu_percent"],
    "significance_threshold": 0.95
  }
}
```

---

## 5. Structured Error Handling

### 5.1 Error Codes

| Code | Name | Description |
|------|------|-------------|
| 0 | OK | Success |
| -1 | ERR_GENERAL | Unspecified error |
| -2 | ERR_NOMEM | Out of memory |
| -3 | ERR_INVALID | Invalid parameter |
| -4 | ERR_DENIED | Capability denied |
| -5 | ERR_TIMEOUT | Operation timed out |
| -6 | ERR_BUSY | Resource busy |
| -7 | ERR_NOTFOUND | Resource not found |
| -8 | ERR_EXISTS | Resource already exists |
| -9 | ERR_OVERFLOW | Buffer or counter overflow |
| -10 | ERR_DEADLOCK | Deadlock detected |

### 5.2 Error Response Format

```json
{
  "status": -4,
  "error": "ERR_DENIED",
  "message": "Agent 42 lacks NETWORK capability for this operation",
  "syscall": "A2A_SEND",
  "agent_id": 42,
  "required_capability": "NETWORK",
  "timestamp": 1735689600000
}
```

---

## 6. Agent Health Check Protocol

Operators and supervisor agents can query the health of any agent:

```json
{
  "agent_id": 42,
  "name": "temp-monitor",
  "state": "THINKING",
  "uptime_ticks": 1209600,
  "resources": {
    "cpu_ticks_used": 45230,
    "cpu_ticks_max": 100000,
    "memory_used": 204800,
    "memory_max": 1048576,
    "io_per_sec_used": 120,
    "io_per_sec_max": 1000
  },
  "a2a_stats": {
    "messages_sent": 1042,
    "messages_received": 893,
    "delegations": 14
  },
  "last_heartbeat": 1735689600000,
  "published_capabilities": [
    {"name": "temperature_reading", "description": "Can read I2C temperature sensor"},
    {"name": "metric_publish", "description": "Can publish metrics to cloud agent"}
  ]
}
```

---

## 7. CLI Programmatic Interface

For agents operating at the shell level (CI/CD bots, operator scripts), the CLI supports structured output formats:

```bash
# JSON output
flux fleet status --format json

# Machine-readable table (TSV)
flux device list --format tsv

# Quiet mode (exit code only)
flux device health --device greenhouse-pi-01 --quiet
# Exit codes: 0=healthy, 1=degraded, 2=unhealthy, 3=unreachable

# Event stream (NDJSON)
flux fleet events --follow --format ndjson
```

### NDJSON Event Stream Format

Each line is a complete JSON object:
```json
{"type":"device_online","device":"greenhouse-pi-01","timestamp":1735689600000}
{"type":"deploy_start","device":"greenhouse-pi-02","version":"2.1.0","timestamp":1735689601000}
{"type":"deploy_complete","device":"greenhouse-pi-02","version":"2.1.0","duration_ms":3420,"status":"ok","timestamp":1735689604420}
{"type":"metric","device":"greenhouse-pi-01","name":"latency_p99","value":8.2,"unit":"ms","timestamp":1735689605000}
```

---

## 8. Web API (RESTful)

For agents that prefer HTTP, the Web interface exposes a REST API:

```
GET    /api/v1/status              → System status
GET    /api/v1/devices             → List devices
GET    /api/v1/devices/:id         → Device details
POST   /api/v1/devices/:id/deploy  → Deploy to device
GET    /api/v1/fleets              → List fleets
GET    /api/v1/fleets/:id          → Fleet details
POST   /api/v1/fleets/:id/deploy   → Deploy to fleet
GET    /api/v1/ab-tests            → List A/B tests
POST   /api/v1/ab-tests            → Create A/B test
GET    /api/v1/ab-tests/:id        → Test results
POST   /api/v1/ab-tests/:id/promote → Promote variant
GET    /api/v1/agents              → List agents
GET    /api/v1/agents/:id          → Agent details
GET    /api/v1/agents/:id/health   → Agent health
POST   /api/v1/compile             → Compile FLUX.MD
GET    /api/v1/hardware             → Hardware info
GET    /api/v1/logs                → Log stream (SSE)
```

Authentication uses capability tokens in the `Authorization: Bearer <token>` header.

---

## 9. Integration Patterns

### 9.1 CI/CD Pipeline Agent

```yaml
# Example: GitHub Actions workflow for FLUX OS
steps:
  - name: Build FLUX OS
    run: make clean && make

  - name: Run Tests
    run: make test

  - name: Cross-compile for ARM64
    run: flux build --target arm64 --optimize -Os

  - name: Deploy to Fleet (canary)
    run: flux deploy --fleet production --strategy canary --batch-size 10%

  - name: Wait for A/B metrics
    run: flux ab-test wait --duration 1h --metric error_rate --threshold 0.01

  - name: Full Rollout
    run: flux deploy --fleet production --strategy rolling --batch-size 100%
```

### 9.2 Operator Agent Pattern

An operator agent is an AI agent that monitors the fleet and makes autonomous decisions:

```
1. SUBSCRIBE to fleet events (device online/offline, error spikes, metric anomalies)
2. ON device_error:
   a. CHECK device health
   b. IF network unreachable → NOTIFY human operator
   c. IF application error → ATTEMPT hot-swap with known-good version
   d. IF hardware failure → REMOVE from fleet, CREATE replacement task
3. ON metric_anomaly (e.g., latency spike):
   a. COLLECT recent logs from affected devices
   b. ANALYZE patterns using DEVCODE to generate diagnostic code
   c. IF root cause identified → GENERATE fix, DEPLOY to affected subset
   d. IF ambiguous → NOTIFY human operator with analysis
```

### 9.3 Multi-Agent Orchestration

```
Supervisor Agent (ID: 1)
├── Compiler Agent (ID: 3)
│   └── Compiles FLUX.MD → bytecode on demand
├── Deployer Agent (ID: 4)
│   └── Manages fleet deployments and rollbacks
├── Monitor Agent (ID: 5)
│   └── Collects metrics, detects anomalies
├── App Agent: temp-monitor (ID: 10)
│   └── Reads sensors, publishes data
├── App Agent: alert-handler (ID: 11)
│   └── Receives alerts, sends notifications
└── App Agent: data-aggregator (ID: 12)
    └── Collects data, publishes to cloud
```

Communication flows through A2A messages. The Supervisor delegates tasks, the Compiler compiles code, the Deployer pushes to devices, and the Monitor watches everything. Application agents focus on their domain logic while relying on infrastructure agents for cross-cutting concerns.
