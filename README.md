# FLUX OS

**Fluid Language Universal eXecution** — An intelligently hardware-agnostic operating system written in pure C where *the kernel IS the compiler*.

```
   ██████╗ ██████╗ ██████╗  ██████╗
  ██╔════╝██╔═══██╗██╔══██╗██╔═══██╗
  ██║     ██║   ██║██║  ██║██║   ██║
  ██║     ██║   ██║██║  ██║██║   ██║
  ╚██████╗╚██████╔╝██████╔╝╚██████╔╝
   ╚═════╝ ╚═════╝ ╚═════╝  ╚═════╝
```

[![Build Status](https://img.shields.io/badge/build-passing-brightgreen)]()
[![C Standard](https://img.shields.io/badge/C-C11-blue)]()
[![Version](https://img.shields.io/badge/version-0.1.0-orange)]()
[![License](https://img.shields.io/badge/license-MIT-green)]()

---

## What is FLUX OS?

FLUX OS is a microkernel operating system written entirely in C11 that fundamentally rethinks how an operating system interacts with its hardware, its applications, and its developers. Instead of the traditional model where an OS loads pre-compiled binaries, FLUX OS contains a self-compiler that can **write, compile, and execute code from kernel-space to user-space** — making it the first OS where the kernel itself acts as both developer and compiler.

The system is **intelligently hardware-agnostic**: all hardware access flows through a pluggable Hardware Abstraction Layer (HAL), enabling the same OS image and the same FLUX bytecode to run natively on x86_64 servers, ARM64 edge devices, RISC-V microcontrollers, and even WebAssembly in the browser. The self-compiler uses hardware capability data from the HAL to generate optimized native code for whichever platform it happens to be running on, meaning a single FLUX.MD specification can produce x86_64 machine code on a server and ARM64 machine code on a Raspberry Pi without any human intervention.

### Core Principles

| Principle | Description |
|-----------|-------------|
| **The Kernel IS the Compiler** | The OS contains a full compilation pipeline (FLUX.MD → FIR → bytecode/native) accessible from kernel-space via syscalls |
| **Hardware Agnostic** | All hardware access goes through the HAL; the OS adapts to x86_64, ARM64, RISC-V, WASM at boot time |
| **Agent-Native** | Every process can be an autonomous agent with built-in A2A (Agent-to-Agent) messaging protocol |
| **Bytecode-First** | FLUX bytecode is a first-class execution format, running in a 64-register VM with sandboxed regions |
| **Self-Hosting** | The OS can rebuild itself from FLUX.MD specifications — it writes its own code |
| **Edge-Ready** | Develop, build, and deploy to IoT devices from the same TUI, CLI, or Web interface |
| **Hot-Swap A/B Testing** | Compiler-generated binaries can be hot-swapped to redundant devices for instant A/B testing |

---

## Three Interfaces, One Workflow

FLUX OS provides three ways to interact with the system, all exposing the same underlying capabilities. Whether you're a human developer at a terminal, an operator monitoring a fleet of devices through a web dashboard, or an AI agent orchestrating edge deployments programmatically, the interface adapts to your workflow while keeping the same build-deploy-test cycle.

```
┌─────────────────────────────────────────────────────────────┐
│                      FLUX OS                                │
│                                                              │
│  ┌──────────┐    ┌──────────┐    ┌──────────┐              │
│  │    CLI   │    │   TUI    │    │   Web    │              │
│  │ flux cli │    │ flux tui │    │ flux web │              │
│  └────┬─────┘    └────┬─────┘    └────┬─────┘              │
│       │               │               │                     │
│       └───────────────┼───────────────┘                     │
│                       │                                     │
│            ┌──────────▼──────────┐                          │
│            │   flux-core API     │                          │
│            │ (compiler + HAL +   │                          │
│            │  agent runtime)     │                          │
│            └──────────┬──────────┘                          │
│                       │                                     │
│       ┌───────────────┼───────────────┐                     │
│       │               │               │                     │
│  ┌────▼────┐    ┌────▼────┐    ┌────▼────┐               │
│  │  x86_64 │    │  ARM64  │    │ RISC-V  │  ...          │
│  └─────────┘    └─────────┘    └─────────┘               │
│                                                              │
│  ┌─────────────────────────────────────────────────┐        │
│  │           Edge Device Fleet Manager              │        │
│  │    (hot-swap, A/B test, OTA updates)            │        │
│  └─────────────────────────────────────────────────┘        │
└─────────────────────────────────────────────────────────────┘
```

### CLI (Command Line Interface)

The CLI is designed for scripted automation, CI/CD pipelines, and developers who live in the terminal. Every operation is a single command with structured output (JSON or table format).

```bash
# Build for current host
flux build --target native

# Cross-compile for Raspberry Pi
flux build --target arm64 --board rpi4

# Deploy to device fleet
flux deploy --fleet greenhouse-sensors --strategy canary

# A/B test two compiler outputs
flux ab-test --variant-a build/v1.flux --variant-b build/v2.flux --devices 50%

# Agent-first: let an AI agent build and deploy
flux agent run --task "deploy temperature monitor to edge fleet"
```

### TUI (Terminal User Interface)

The TUI provides an interactive, terminal-based experience for developers who want a rich visual interface without leaving their terminal. It features split-pane layouts, real-time logs, and keyboard-driven workflows reminiscent of htop, tmux, and k9s.

```
┌─ FLUX OS TUI ──────────────────────────────────────────┐
│ [1] Dashboard  [2] Build  [3] Deploy  [4] Agents  [5] │
│                                                         │
│ ┌─ Build Config ──────────────────────────────────────┐ │
│ │ Target:      ARM64 (Raspberry Pi 4)                │ │
│ │ Source:      ./apps/greenhouse-monitor.flux.md      │ │
│ │ Optimization: -O2 (balanced)                        │ │
│ │ Output:      ./build/greenhouse-monitor.fluxbc     │ │
│ │                                                      │ │
│ │ ┌─ Compiler Output ──────────────────────────────┐ │ │
│ │ │ [OK] Parsing FLUX.MD...                       │ │ │
│ │ │ [OK] FIR generation (34 functions, 128 values) │ │ │
│ │ │ [OK] Codegen: ARM64 assembly                   │ │ │
│ │ │ [OK] Emitting binary (48.2 KB)                │ │ │
│ │ │ [OK] Hot-swap ready                            │ │ │
│ │ └────────────────────────────────────────────────┘ │ │
│ │                                                      │ │
│ │ [Build]  [Deploy]  [A/B Test]  [Terminal]           │ │
│ └──────────────────────────────────────────────────────┘ │
│ ┌─ Device Fleet ───────────────────────────────────────┐ │
│ │ Device              Status    Variant   Uptime       │ │
│ │ greenhouse-pi-01    ● ACTIVE   v2 (B)    14d 6h      │ │
│ │ greenhouse-pi-02    ● ACTIVE   v1 (A)    14d 6h      │ │
│ │ greenhouse-pi-03    ● ACTIVE   v2 (B)    14d 5h      │ │
│ │ greenhouse-esp-01   ◌ IDLE     v1 (A)    0d 0h      │ │
│ └──────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────┘
```

### Web Interface

The Web interface is designed for fleet operators, non-developers, and monitoring scenarios. It runs as a lightweight HTTP server embedded in FLUX OS itself and is accessible from any browser on the network.

```
┌─ FLUX OS Dashboard ─ https://flux.local:8080 ──────────┐
│                                                          │
│  ┌────────────┐  ┌────────────┐  ┌────────────┐        │
│  │   12       │  │    48.2KB  │  │   99.97%   │        │
│  │  Devices   │  │ Avg Build  │  │  Uptime    │        │
│  └────────────┘  └────────────┘  └────────────┘        │
│                                                          │
│  ┌─ A/B Test: Temperature Sensor v1 vs v2 ────────────┐ │
│  │                                                      │ │
│  │  v1 (Control)          v2 (Treatment)                │ │
│  │  ━━━━━━━━━━━━ 52%      ━━━━━━━━━━ 48%               │ │
│  │  Latency: 12ms         Latency: 8ms                  │ │
│  │  Memory: 2.1MB         Memory: 1.8MB                 │ │
│  │  Errors: 0.01%         Errors: 0.003%                │ │
│  │                                                      │ │
│  │  [Promote v2]  [Extend]  [Rollback]  [Stop]          │ │
│  └──────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────┘
```

---

## Architecture Overview

FLUX OS is structured as a microkernel with six core subsystems, each communicating through well-defined C APIs. The architecture follows a strict layering principle where upper layers never directly access lower layers — they go through the kernel's syscall interface.

```
┌──────────────────────────────────────────────────────────┐
│                    User Space                             │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐               │
│  │  FLUX.MD │  │  Agents  │  │   Apps   │               │
│  │  Source   │  │  (A2A)   │  │          │               │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘               │
│       └──────────────┼──────────────┘                     │
│                      │ Syscalls                           │
├──────────────────────┼───────────────────────────────────┤
│                    Kernel Space                           │
│  ┌───────────────────▼───────────────────────┐           │
│  │           FLUX Microkernel                │           │
│  │  ┌─────────┐ ┌──────┐ ┌────────┐ ┌─────┐ │           │
│  │  │Compiler │ │  VM  │ │ Agent  │ │ Sched│ │           │
│  │  │(FIR)    │ │(64R) │ │Runtime│ │      │ │           │
│  │  └─────────┘ └──────┘ └────────┘ └─────┘ │           │
│  │  ┌─────────┐ ┌──────┐ ┌────────┐         │           │
│  │  │ Memory  │ │ IPC  │ │Syscall│         │           │
│  │  │ Manager │ │(A2A) │ │Dispatch│         │           │
│  │  └─────────┘ └──────┘ └────────┘         │           │
│  └───────────────────────┬───────────────────┘           │
│                          │                                │
│  ┌───────────────────────▼───────────────────┐           │
│  │         Hardware Abstraction Layer        │           │
│  │  ┌─────────┐ ┌──────┐ ┌────────┐         │           │
│  │  │ x86_64  │ │ARM64 │ │RISC-V  │  WASM  │           │
│  │  │ Backend │ │Back. │ │Back.   │  Back.  │           │
│  │  └─────────┘ └──────┘ └────────┘         │           │
│  └───────────────────────────────────────────┘           │
│                    Hardware                               │
└──────────────────────────────────────────────────────────┘
```

### Subsystems

| Subsystem | Location | Description |
|-----------|----------|-------------|
| **Microkernel** | `kernel/` | Core process management, scheduling, syscalls, panic handling |
| **HAL** | `hal/` | Hardware abstraction with pluggable backends (x86_64, ARM64, RISC-V, native) |
| **VM** | `vm/` | 64-register bytecode virtual machine with sandboxed memory regions |
| **Compiler** | `fluxc/` | FLUX.MD → FIR (SSA IR) → bytecode/C/native code generation |
| **Agent Runtime** | `agent/` | A2A messaging, capability security, agent lifecycle, sandboxing |
| **Opcodes** | `include/flux/opcodes.h` | Shared 184-opcode instruction set across all implementations |

---

## Quick Start

### Prerequisites

- GCC or Clang with C11 support
- Make (or CMake for cross-compilation)
- For bare-metal targets: QEMU, cross-compiler toolchain

### Build in 60 Seconds

```bash
# Clone the repo
git clone https://github.com/SuperInstance/flux-os.git
cd flux-os

# Build the library
make

# Run hosted-mode tests (verifies kernel, HAL, VM, opcodes)
make test
```

### Expected Output

```
╔══════════════════════════════════════════════════════╗
║         FLUX OS — Hosted Mode Test Suite             ║
║         Version 0.1.0                                ║
╚══════════════════════════════════════════════════════╝

--- HAL Tests ---
  TEST: HAL boot sequence                            [PASS]
  TEST: HAL arch name                                 [PASS]
  TEST: HAL console putc                              [PASS]
  ...

--- VM Basic Tests ---
  TEST: VM init                                       [PASS]
  TEST: VM load bytecode                              [PASS]
  ...

╔══════════════════════════════════════════════════════╗
  Results: 32 PASSED, 0 FAILED, 32 Total
╚══════════════════════════════════════════════════════╝
```

---

## Edge IoT Deployment

One of FLUX OS's key capabilities is the ability to **develop, build, and deploy to edge devices** from a single interface. The self-compiler generates optimized binaries for each target architecture automatically, and the fleet manager handles distribution, hot-swapping, and A/B testing across your device fleet.

```bash
# Define your application in FLUX.MD (human-readable + agent-readable)
cat > apps/sensor.flux.md << 'EOF'
# Temperature Monitor Agent

## Compile
target: arm64
optimize: -Os (size)
board: raspberry-pi-4

## Agent
name: temp-monitor
capabilities: IO_READ, NETWORK, MEMORY
heartbeat: 30s

## Loop
1. Read temperature from I2C sensor (addr 0x48)
2. If temperature > threshold:
   - DELEGATE to alert-agent: send_notification(temp, location)
3. Every 60s: TELL cloud-agent: publish_metric(temp, device_id)
4. YIELD (sleep until next reading)
EOF

# Build for ARM64 edge devices
flux build --source apps/sensor.flux.md --target arm64

# Deploy to your device fleet
flux deploy --fleet greenhouse --strategy rolling --batch-size 25%

# Monitor deployment
flux fleet status --watch
```

### Hot-Swap A/B Testing

```bash
# Create variant A (current stable)
flux build --source apps/sensor.flux.md --target arm64 --output v1.fluxbc

# Create variant B (new version with changes)
flux build --source apps/sensor-v2.flux.md --target arm64 --output v2.fluxbc

# Run A/B test: 50/50 split across fleet
flux ab-test \
  --variant-a v1.fluxbc \
  --variant-b v2.fluxbc \
  --split 50/50 \
  --duration 24h \
  --metrics latency,memory,error_rate

# See results and promote winner
flux ab-test results --name sensor-ab-2024-04-10
flux ab-test promote --variant v2.fluxbc --name sensor-ab-2024-04-10
```

---

## Onboarding Paths

FLUX OS provides two distinct onboarding paths, recognizing that the system has two primary audiences: human developers and AI agents.

### For Humans

See **[docs/ONBOARDING.md](docs/ONBOARDING.md)** for the complete human onboarding guide, including environment setup, first build, first deploy, and progressive skill development.

### For AI Agents

See **[docs/AGENT-ONBOARDING.md](docs/AGENT-ONBOARDING.md)** for the machine-readable onboarding specification, including API contracts, capability negotiation, and programmatic workflow descriptions.

---

## Documentation Index

| Document | Description |
|----------|-------------|
| [README.md](README.md) | This document — project overview and quick start |
| [docs/QUICKSTART.md](docs/QUICKSTART.md) | 5-minute quick start for all three interfaces |
| [docs/ONBOARDING.md](docs/ONBOARDING.md) | Complete human onboarding guide |
| [docs/AGENT-ONBOARDING.md](docs/AGENT-ONBOARDING.md) | Agent-first onboarding specification |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | System architecture deep-dive |
| [docs/CLI-REFERENCE.md](docs/CLI-REFERENCE.md) | CLI command reference (all commands, flags, examples) |
| [docs/TUI-GUIDE.md](docs/TUI-GUIDE.md) | TUI keyboard shortcuts, layouts, and workflows |
| [docs/WEB-INTERFACE.md](docs/WEB-INTERFACE.md) | Web dashboard features and REST API |
| [docs/EDGE-IOT-DEPLOYMENT.md](docs/EDGE-IOT-DEPLOYMENT.md) | Edge device deployment and fleet management |
| [docs/HOTSWAP-AB-TESTING.md](docs/HOTSWAP-AB-TESTING.md) | Compiler hot-swap and A/B testing |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Contributing guidelines |

---

## Repository Structure

```
flux-os/
├── include/flux/          # Public API headers
│   ├── kernel.h           # Microkernel types, syscalls, PCB
│   ├── hal.h              # Hardware abstraction layer
│   ├── vm.h               # Bytecode VM (64 registers, regions)
│   ├── compiler.h         # Self-compiler (FIR SSA IR)
│   ├── agent.h            # Agent runtime (A2A protocol)
│   └── opcodes.h          # 184-opcode instruction set
├── kernel/                # Microkernel implementation
│   ├── main.c             # Boot sequence, banner, subsystem init
│   ├── info.c             # Kernel introspection
│   ├── log.c              # Logging subsystem
│   ├── mem.c              # Memory manager (free-list allocator)
│   ├── proc.c             # Process management (PCB table)
│   ├── sched.c            # Priority round-robin scheduler
│   ├── ipc.c              # Inter-process communication (A2A)
│   ├── syscall.c          # System call dispatcher (28 syscalls)
│   └── panic.c            # Kernel panic handler
├── vm/                    # Bytecode VM implementation
│   ├── vm.c               # VM core (init, load, run, halt)
│   ├── opcodes.c          # Opcode name lookup and execution
│   └── region.c           # Sandboxed memory regions
├── fluxc/                 # Self-compiler (FLUX.MD → FIR → output)
│   ├── lexer.c            # FLUX.MD lexer
│   ├── parser.c           # FLUX.MD parser → AST
│   └── fir.c              # FIR SSA IR construction
├── hal/                   # Hardware abstraction layer
│   ├── hal.c              # HAL core (backend selection, boot)
│   ├── arch/
│   │   ├── native/        # Hosted mode (Linux/POSIX)
│   │   ├── x86_64/        # x86_64 bare metal backend
│   │   └── ...            # ARM64, RISC-V backends
├── agent/                 # Agent runtime
│   ├── agent.c            # Agent lifecycle (spawn, terminate, schedule)
│   ├── capability.c       # Capability security model
│   ├── discovery.c        # Agent discovery and service registry
│   ├── sandbox.c          # Execution sandboxing
│   └── a2a.c              # A2A messaging protocol
├── tests/                 # Test suites
│   └── test_hosted.c      # Hosted mode integration tests (32 tests)
├── docs/                  # Documentation
│   ├── QUICKSTART.md
│   ├── ONBOARDING.md
│   ├── AGENT-ONBOARDING.md
│   ├── ARCHITECTURE.md
│   ├── CLI-REFERENCE.md
│   ├── TUI-GUIDE.md
│   ├── WEB-INTERFACE.md
│   ├── EDGE-IOT-DEPLOYMENT.md
│   └── HOTSWAP-AB-TESTING.md
├── Makefile               # Build system
└── README.md              # This file
```

---

## The FLUX Ecosystem

FLUX OS is part of a larger ecosystem of three repositories, each targeting a different abstraction level:

| Repository | Language | Purpose | Status |
|-----------|----------|---------|--------|
| [flux-runtime](https://github.com/SuperInstance/flux-runtime) | Python | Research, rapid prototyping, 1907 tests, 104 opcodes | Stable |
| [flux](https://github.com/SuperInstance/flux) | Rust | Production runtime, 286 tests, 100 opcodes | Stable |
| [flux-os](https://github.com/SuperInstance/flux-os) | C | Operating system, hardware-agnostic, self-compiling | v0.1 Alpha |

---

## Key Numbers

| Metric | Value |
|--------|-------|
| Instruction Set | 184 opcodes |
| VM Registers | 64 general-purpose |
| Syscalls | 28 (process, memory, IPC, bytecode, compiler, I/O, hardware) |
| HAL Backends | 5 (x86_64, ARM64, RISC-V, WASM, Native) |
| Agent Capabilities | 11 capability flags |
| A2A Message Types | 11 (tell, ask, reply, delegate, result, barrier, subscribe, event, error, capability, heartbeat) |
| FIR Types | 17 (including agent, capability, bytecode, region) |
| Memory Regions | 16 per VM instance (sandboxed) |
| PCB Slots | 256 processes |
| Test Coverage | 32 integration tests (hosted mode) |

---

## License

MIT License — see [LICENSE](LICENSE) for details.

---

*"The Kernel IS the Compiler"*
