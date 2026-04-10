# FLUX OS — Hot-Swap and A/B Testing Guide

## Overview

FLUX OS's hot-swap capability is one of its most distinctive features. Unlike traditional operating systems that require a reboot to update running code, FLUX OS can replace the bytecode of a running agent atomically — without stopping the agent, without losing its state, and without any downtime. Combined with the A/B testing framework, this enables a continuous experimentation workflow where you can test new versions of your application on a live fleet, compare performance metrics, and make data-driven rollout decisions.

This guide explains how the hot-swap mechanism works, how to configure and run A/B tests, how to interpret results, and how to promote or rollback variants. Whether you're a human operator making decisions or an AI agent autonomously optimizing a fleet, the same tools and protocols apply.

---

## 1. How Hot-Swap Works

### 1.1 The Hot-Swap Mechanism

Hot-swapping in FLUX OS replaces the bytecode of a running agent without restarting the process. The mechanism works in four phases:

**Phase 1: Pre-Load (Shadow Region)**
The new bytecode is loaded into a shadow memory region alongside the currently executing bytecode. The VM allocates a new region, copies the new bytecode into it, and performs validation checks (checksum verification, capability validation, dependency checking). During this phase, the agent continues executing the old bytecode normally.

**Phase 2: Verify**
The kernel verifies the new bytecode's integrity and compatibility. This includes:
- SHA-256 checksum verification against the expected hash
- Capability validation (new bytecode requires no additional capabilities beyond what the agent already has)
- Memory region size check (new bytecode fits within the allocated shadow region)
- Opcode validation (all opcodes in the new bytecode are recognized by this VM version)
- Dependency check (imported modules and agent references exist)

**Phase 3: Atomic Swap**
If verification passes, the kernel atomically updates the agent's execution context:
- The bytecode base pointer is switched to the shadow region
- The program counter (PC) is adjusted to the corresponding position in the new bytecode
- The call stack is preserved (function calls that were in progress continue in the new code)
- The old bytecode region is marked for garbage collection

The atomic swap happens during a scheduler tick boundary, ensuring no instruction is partially executed. The swap is a single pointer assignment at the C level, which is atomic on all supported architectures.

**Phase 4: Garbage Collection**
After the swap, the old bytecode region is kept alive for a configurable grace period (default: 5 minutes). If any agent is still referencing the old bytecode (e.g., a breakpoint or tracing buffer pointing to the old code), the region is not freed until all references are released. After the grace period, the region is freed and the memory is returned to the kernel's free list.

### 1.2 What Gets Preserved

During a hot-swap, the following state is preserved:
- **Register file**: All 64 registers retain their values
- **Memory regions**: Agent workspace regions (data, heap, stack) are untouched
- **Call stack**: Function calls in progress continue in the new code
- **A2A connections**: All agent-to-agent connections remain active
- **Capabilities**: Agent's capability set is unchanged
- **Timers and alarms**: Pending timers and alarms continue running
- **Subscriptions**: Pub-sub topic subscriptions are preserved

### 1.3 What Gets Reset

The following state is reset during a hot-swap:
- **Program counter**: Adjusted to the corresponding position in the new bytecode
- **Breakpoints**: All breakpoints are cleared (they referred to old bytecode offsets)
- **Trace buffer**: The execution trace buffer is cleared
- **Profile counters**: Opcode execution counts are reset to zero

### 1.4 Rollback on Failure

If the new bytecode fails verification (Phase 2), the hot-swap is aborted and the old bytecode continues executing. If the new bytecode passes verification but crashes after the swap (Phase 3), the kernel detects the crash and automatically rolls back to the old bytecode. The rollback restores the previous bytecode base pointer and PC, and resumes execution from the point of the swap.

---

## 2. Running A/B Tests

### 2.1 A/B Test Concepts

An A/B test in FLUX OS compares two versions of an application by running them simultaneously on different devices in a fleet. The two versions are called "variants":

- **Variant A (Control)**: The current stable version
- **Variant B (Treatment)**: The new version being tested

The fleet is split according to the configured ratio (e.g., 50/50, 80/20), and each device is assigned a variant. The A/B test framework collects metrics from both variants and provides statistical analysis to determine if the treatment variant is significantly better, worse, or indistinguishable from the control.

### 2.2 Creating an A/B Test

```bash
# Build both variants
flux build --source app-v1.flux.md --output build/v1.fluxbc --target arm64
flux build --source app-v2.flux.md --output build/v2.fluxbc --target arm64

# Run the A/B test
flux ab-test run \
  --name sensor-latency-001 \
  --variant-a build/v1.fluxbc \
  --variant-b build/v2.fluxbc \
  --label-a "control" \
  --label-b "treatment" \
  --split 50/50 \
  --fleet greenhouse \
  --duration 24h \
  --metrics latency_p99,memory_bytes,error_rate,cpu_percent \
  --significance 0.95 \
  --min-sample-size 1000
```

### 2.3 Test Parameters

| Parameter | Description | Default |
|-----------|-------------|---------|
| `--name` | Unique test name (for reference and reporting) | Auto-generated |
| `--variant-a` | Path to control variant bytecode | Required |
| `--variant-b` | Path to treatment variant bytecode | Required |
| `--label-a` | Human-readable label for variant A | "control" |
| `--label-b` | Human-readable label for variant B | "treatment" |
| `--split` | Device allocation ratio (e.g., 50/50, 80/20) | 50/50 |
| `--fleet` | Target fleet name | Required |
| `--duration` | Test duration (e.g., 1h, 24h, 7d) | 24h |
| `--metrics` | Comma-separated list of metrics to collect | latency_p99,error_rate |
| `--significance` | Statistical significance threshold | 0.95 |
| `--min-sample-size` | Minimum data points per variant before analysis | 100 |
| `--auto-promote` | Automatically promote winner if significant | false |
| `--auto-rollback` | Automatically rollback if treatment is worse | true |

### 2.4 Device Assignment

Devices are assigned to variants using consistent hashing on the device ID. This means:
- The same device always gets the same variant for a given test (even after restarts)
- Adding or removing devices doesn't cause reassignment of existing devices
- The actual split ratio converges to the target ratio as the fleet size grows

```bash
# See which devices are in which variant
flux ab-test devices --name sensor-latency-001

# Manually reassign a specific device
flux ab-test reassign --name sensor-latency-001 --device greenhouse-pi-05 --variant b
```

### 2.5 Monitoring a Running Test

```bash
# Watch test progress in real-time
flux ab-test watch --name sensor-latency-001

# Get current metrics snapshot
flux ab-test metrics --name sensor-latency-001

# Get detailed statistical analysis
flux ab-test analysis --name sensor-latency-001
```

The watch command shows a live dashboard:

```
┌─ A/B Test: sensor-latency-001 ─────────────────────────────┐
│ Status: RUNNING (12h / 24h remaining)                       │
│ Confidence: 87.3% (need 95% to conclude)                    │
│                                                              │
│ ┌─ Variant A (control) ──────┐  ┌─ Variant B (treatment) ─┐│
│ │ Devices:  6                │  │ Devices:  6              ││
│ │ Samples:  14,432           │  │ Samples:  14,298         ││
│ │                            │  │                          ││
│ │ latency_p99: 12.3ms       │  │ latency_p99: 7.8ms      ││
│ │ memory_bytes: 2.1MB       │  │ memory_bytes: 1.8MB      ││
│ │ error_rate: 0.012%        │  │ error_rate: 0.003%       ││
│ │ cpu_percent: 4.2%         │  │ cpu_percent: 3.1%        ││
│ └────────────────────────────┘  └──────────────────────────┘│
│                                                              │
│ ┌─ Analysis ──────────────────────────────────────────────┐ │
│ │ latency_p99:  B is 36.6% faster (p < 0.001) ★          │ │
│ │ memory_bytes: B uses 14.3% less (p < 0.01)             │ │
│ │ error_rate:   B has 75.0% fewer errors (p < 0.05)      │ │
│ │ cpu_percent:  B uses 26.2% less CPU (p < 0.01)         │ │
│ └─────────────────────────────────────────────────────────┘ │
│                                                              │
│ [Promote B]  [Extend]  [Stop]  [Rollback to A]              │
└──────────────────────────────────────────────────────────────┘
```

---

## 3. Metrics and Statistical Analysis

### 3.1 Supported Metrics

| Metric | Type | Description | Collection Method |
|--------|------|-------------|------------------|
| `latency_p50` | Float | 50th percentile latency | Agent heartbeat timing |
| `latency_p99` | Float | 99th percentile latency | Agent heartbeat timing |
| `latency_avg` | Float | Average latency | Agent heartbeat timing |
| `memory_bytes` | Integer | Memory usage in bytes | Kernel memory stats |
| `memory_peak` | Integer | Peak memory usage | Kernel memory stats |
| `error_rate` | Float | Error percentage | Agent error counter |
| `error_count` | Integer | Total error count | Agent error counter |
| `cpu_percent` | Float | CPU usage percentage | Scheduler stats |
| `cpu_ticks` | Integer | CPU ticks consumed | Scheduler stats |
| `io_per_sec` | Integer | I/O operations per second | I/O counter |
| `msg_count` | Integer | A2A messages sent/received | IPC stats |
| `uptime` | Integer | Agent uptime in ticks | Kernel timer |
| `compile_time_ms` | Integer | Time to compile bytecode | Compiler timer |
| `hotswap_time_ms` | Integer | Time to hot-swap bytecode | Swap timer |

### 3.2 Statistical Tests

The A/B test framework uses appropriate statistical tests for each metric type:

- **Latency and timing metrics**: Mann-Whitney U test (non-parametric, robust to outliers and non-normal distributions)
- **Error rates and proportions**: Chi-squared test or Fisher's exact test (for small samples)
- **Memory and CPU metrics**: Welch's t-test (handles unequal variances between variants)
- **Count metrics**: Poisson rate comparison test

The framework reports:
- **p-value**: Probability that the observed difference is due to chance
- **confidence interval**: Range within which the true difference likely lies
- **effect size**: Magnitude of the difference (as a percentage)
- **statistical power**: Probability of detecting a real difference if one exists

### 3.3 Interpretation

The framework classifies results into four categories:

| Category | Condition | Recommendation |
|----------|-----------|---------------|
| **B is better** | p < threshold AND B's metric is better | Consider promoting B |
| **A is better** | p < threshold AND A's metric is better | Keep A, discard B |
| **Inconclusive** | p >= threshold | Continue test or increase sample size |
| **Regression** | B is significantly worse on any metric | Rollback to A |

When multiple metrics are tracked, the framework applies a Bonferroni correction to account for multiple comparisons. A variant must be better (or equal) on ALL metrics and significantly better on at least one to be recommended for promotion.

---

## 4. Test Lifecycle

### 4.1 Creating a Test

```bash
flux ab-test run --name my-test --variant-a v1.fluxbc --variant-b v2.fluxbc --fleet production
```

This command:
1. Validates both bytecode artifacts
2. Assigns devices to variants using consistent hashing
3. Hot-swaps variant A onto control devices
4. Hot-swaps variant B onto treatment devices
5. Starts metric collection
6. Returns the test ID

### 4.2 Monitoring a Test

```bash
# Real-time watch
flux ab-test watch --name my-test

# Snapshot
flux ab-test metrics --name my-test

# Detailed analysis
flux ab-test analysis --name my-test

# Export data for external analysis
flux ab-test export --name my-test --format csv --output my-test-data.csv
```

### 4.3 Concluding a Test

```bash
# Promote the winning variant (all devices get the winner)
flux ab-test promote --name my-test --winner b

# Extend the test duration
flux ab-test extend --name my-test --duration 12h

# Stop the test (all devices revert to pre-test version)
flux ab-test stop --name my-test

# Rollback to the control variant
flux ab-test rollback --name my-test
```

### 4.4 Post-Test Analysis

```bash
# Get full test report
flux ab-test report --name my-test

# Export all data
flux ab-test export --name my-test --format json --output report.json

# Compare with previous tests
flux ab-test compare --tests sensor-latency-001,sensor-latency-002
```

---

## 5. Advanced Patterns

### 5.1 Multi-Variant Testing (A/B/C/...)

FLUX OS supports testing more than two variants simultaneously. This is useful when you want to compare multiple implementations:

```bash
flux ab-test run \
  --name multi-algo-test \
  --variants "v1:build/v1.fluxbc:50,v2:build/v2.fluxbc:25,v3:build/v3.fluxbc:25" \
  --fleet production \
  --duration 48h
```

### 5.2 Sequential Testing (Chained A/B Tests)

For progressive improvements, you can chain A/B tests so that each test uses the winner of the previous test as the new control:

```bash
# Test 1: v1 vs v2 → v2 wins
flux ab-test run --name test-1 --variant-a v1.fluxbc --variant-b v2.fluxbc ...
flux ab-test promote --name test-1 --winner b

# Test 2: v2 (new control) vs v3
flux ab-test run --name test-2 --variant-a v2.fluxbc --variant-b v3.fluxbc ...
flux ab-test promote --name test-2 --winner b
```

### 5.3 Automated Optimization (Agent-Driven)

An operator agent can autonomously run A/B tests to optimize application performance. The pattern:

1. Agent generates a variant with a potential improvement (using DEVCODE)
2. Agent creates an A/B test comparing current version to new variant
3. Agent monitors results using statistical analysis
4. If variant is significantly better, agent promotes it
5. If variant is worse or inconclusive, agent generates a new variant
6. Repeat

This creates an evolutionary optimization loop where the application continuously improves without human intervention.

### 5.4 Fractional Rollouts

For gradual rollouts, you can use A/B tests with asymmetric splits:

```bash
# Start with 5% on new version
flux ab-test run --name rollout-v2 --variant-a v1.fluxbc --variant-b v2.fluxbc --split 95/5 --duration 1h

# If no regressions, increase to 25%
flux ab-test update --name rollout-v2 --split 75/25 --duration 6h

# Then to 50%
flux ab-test update --name rollout-v2 --split 50/50 --duration 12h

# Finally promote to 100%
flux ab-test promote --name rollout-v2 --winner b
```

### 5.5 Canary Deployments with A/B Testing

Combine canary deployments with A/B testing for maximum safety:

```bash
# Deploy to 1 device first (canary)
flux ab-test run --name canary-v2 --variant-a v1.fluxbc --variant-b v2.fluxbc \
  --split 99/1 --duration 30m --min-sample-size 10

# Monitor canary health closely
flux ab-test watch --name canary-v2

# If canary is healthy, expand
flux ab-test update --name canary-v2 --split 90/10 --duration 2h

# Continue expanding if healthy
flux ab-test update --name canary-v2 --split 75/25 --duration 6h
```

---

## 6. Hot-Swap API (Programmatic)

For agents and automation tools, hot-swap operations are available through the CLI with structured output:

```bash
# Hot-swap a single device (JSON output)
flux hot-swap --device greenhouse-pi-01 \
  --bytecode build/v2.fluxbc \
  --format json

# Response:
# {
#   "status": "ok",
#   "device": "greenhouse-pi-01",
#   "agent_id": 42,
#   "old_bytecode_sha256": "abc123...",
#   "new_bytecode_sha256": "def456...",
#   "swap_time_ms": 2.3,
#   "preserved_state": {
#     "registers": true,
#     "memory_regions": true,
#     "a2a_connections": true,
#     "subscriptions": true
#   }
# }
```

### Web API Endpoints

```
POST /api/v1/devices/:id/hotswap     → Hot-swap bytecode on a device
GET  /api/v1/devices/:id/bytecode    → Get current bytecode info
POST /api/v1/ab-tests                → Create A/B test
GET  /api/v1/ab-tests/:id            → Get test results
POST /api/v1/ab-tests/:id/promote    → Promote variant
POST /api/v1/ab-tests/:id/rollback   → Rollback test
GET  /api/v1/ab-tests/:id/metrics    → Get current metrics
```
