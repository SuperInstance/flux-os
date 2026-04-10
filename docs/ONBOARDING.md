# FLUX OS — Human Onboarding Guide

## Welcome to FLUX OS

This guide walks you through everything you need to get started with FLUX OS as a human developer or operator. Whether you want to build your first FLUX application, deploy it to an edge device, or manage a fleet of IoT sensors running FLUX OS, this guide covers every step. The onboarding is designed as a progressive journey: you start with the basics of building and running FLUX in hosted mode on your development machine, then move to cross-compiling for edge hardware, and finally graduate to fleet management and A/B testing across distributed devices.

FLUX OS is deliberately different from other operating systems you may have worked with. Rather than loading pre-compiled binaries, FLUX OS contains a compiler inside its kernel that can translate human-readable FLUX.MD specifications into optimized bytecode or native machine code. This means your workflow is not "write C, compile, upload binary" but rather "describe what you want in markdown, let the OS figure out the best implementation, and deploy it." The OS adapts to whatever hardware it finds itself on, so the same description produces different but equally optimal code on a Raspberry Pi, an ESP32, or a server.

---

## Chapter 1: Environment Setup

### 1.1 Prerequisites

Before you begin, ensure your development machine meets these requirements. FLUX OS is designed to build cleanly on any modern Unix-like system, and the hosted mode allows you to develop and test without any bare-metal hardware.

**Required:**
- GCC 7+ or Clang 6+ with C11 support (check with `gcc --version` or `clang --version`)
- GNU Make 4.0+ or BSD Make (check with `make --version`)
- Git for version control

**Optional (for bare-metal targets):**
- QEMU 6.0+ for emulated bare-metal testing
- ARM64 cross-compiler (`aarch64-linux-gnu-gcc`) for Raspberry Pi targets
- RISC-V cross-compiler (`riscv64-unknown-elf-gcc`) for RISC-V targets
- OpenOCD for flashing to physical devices

**Optional (for Web interface):**
- A modern web browser (Chrome, Firefox, Safari, Edge)
- No additional server software needed — FLUX OS embeds its own HTTP server

### 1.2 Clone and Build

```bash
# Clone the repository
git clone https://github.com/SuperInstance/flux-os.git
cd flux-os

# Build the static library (libflux-os.a)
make

# Verify the build produced the library
ls -la build/libflux-os.a

# Run the hosted-mode test suite
make test
```

You should see all tests pass. The test suite verifies the HAL (Hardware Abstraction Layer), VM (Virtual Machine), kernel info, memory regions, and instruction encoding — all running in hosted mode on your development machine without requiring any real hardware.

### 1.3 Understanding the Build

FLUX OS builds as a static library (`libflux-os.a`) that contains all kernel subsystems. This library can be linked into a hosted-mode test binary (as the test suite does), linked into a bare-metal kernel image, or linked into the TUI/Web interface tools. The build system is deliberately simple: a single Makefile with no external dependencies beyond a C compiler.

```
make          → builds build/libflux-os.a
make clean    → removes all build artifacts
make hosted   → builds and runs the hosted-mode test binary
make test     → alias for 'make hosted'
```

For cross-compilation, you set the `CC` variable to your cross-compiler and `CFLAGS` for architecture-specific flags:

```bash
# Cross-compile for ARM64 (Raspberry Pi)
make clean
make CC=aarch64-linux-gnu-gcc CFLAGS="-Wall -Wextra -std=c11 -O2 -Iinclude -target aarch64-linux-gnu"
```

---

## Chapter 2: Your First FLUX Application

### 2.1 Understanding FLUX.MD

FLUX.MD is the source format for FLUX OS applications. It looks like markdown (because it is markdown) but contains structured sections that the FLUX compiler understands. The compiler extracts compilation directives, agent specifications, and behavioral descriptions from the markdown, translates them into FIR (FLUX Intermediate Representation), and then generates optimized bytecode or native code.

Here is a minimal FLUX.MD file — a "Hello, World" that runs as an agent:

```markdown
# Hello World Agent

## Compile
target: bytecode
optimize: -O1

## Agent
name: hello-agent
capabilities: IO_WRITE
heartbeat: 60s

## Init
Write "Hello from FLUX OS!" to console.

## Loop
YIELD (wait forever)
```

The key sections are:
- **Compile**: Tells the self-compiler what target and optimization level to use
- **Agent**: Defines the agent's name, capabilities, and heartbeat interval
- **Init**: Code that runs once when the agent starts
- **Loop**: Code that runs repeatedly (the agent's main loop)

### 2.2 Building Your First Application

In the current v0.1 release, the full FLUX.MD parser is under active development. You can already work with FLUX at the bytecode level:

```c
// hello.c — A minimal FLUX bytecode program
#include "flux/kernel.h"
#include "flux/vm.h"
#include "flux/opcodes.h"

int main(void) {
    flux_vm_t vm;
    flux_vm_init(&vm);

    // Simple bytecode: NOP, HALT
    uint8_t bytecode[] = {
        flux_encode_c(OP_NOP, 0),    // NOP
        flux_encode_c(OP_HALT, 0),   // HALT
    };

    flux_vm_load(&vm, bytecode, sizeof(bytecode));
    flux_vm_run(&vm, 1000);

    printf("Instructions executed: %lu\n", vm.insn_count);
    flux_vm_destroy(&vm);
    return 0;
}
```

### 2.3 Running in Hosted Mode

Hosted mode runs the FLUX kernel on top of your existing OS (Linux, macOS, etc.). This is the primary development mode — you write, build, and test FLUX applications on your development machine before deploying to real hardware.

The hosted mode uses the "native" HAL backend, which maps HAL operations to POSIX system calls. The kernel boots, initializes all subsystems, and runs the scheduler just as it would on bare metal, but with the safety net of your host OS underneath.

```bash
# Build and run hosted mode
make hosted
```

---

## Chapter 3: Working with the Three Interfaces

### 3.1 CLI — For Scripts and Automation

The CLI (`flux`) is your primary tool for day-to-day development. It's designed for composability — every command produces structured output that can be piped to other tools or parsed by scripts.

```bash
# Build for current architecture
flux build

# Build for a specific target
flux build --target arm64 --optimize -Os

# List connected devices
flux device list

# Deploy to a device
flux deploy ./build/myapp.fluxbc --device greenhouse-pi-01

# Deploy to entire fleet
flux deploy ./build/myapp.fluxbc --fleet greenhouse --strategy rolling

# Check fleet status
flux fleet status

# Run A/B test
flux ab-test run --variant-a v1.fluxbc --variant-b v2.fluxbc --split 50/50

# Get structured JSON output
flux fleet status --format json | jq '.devices[] | select(.status == "error")'
```

See **[CLI-REFERENCE.md](CLI-REFERENCE.md)** for the complete command reference.

### 3.2 TUI — For Interactive Development

The TUI (`flux tui`) provides a rich, keyboard-driven interface for interactive development sessions. It's designed for developers who want to see build output, device status, and logs simultaneously without switching between terminal windows.

Launch it with:
```bash
flux tui
```

Key features:
- **Split-pane layout**: Build output on the left, device fleet on the right, logs at the bottom
- **Real-time updates**: Device status, compile progress, and A/B test metrics update live
- **Keyboard-driven**: All actions accessible via keyboard shortcuts (no mouse required)
- **Tabs**: Dashboard, Build, Deploy, Agents, Logs, Settings

See **[TUI-GUIDE.md](TUI-GUIDE.md)** for the complete TUI guide.

### 3.3 Web Interface — For Fleet Operations

The Web interface (`flux web`) is a browser-based dashboard designed for fleet operators, monitoring, and non-developers who need to oversee deployments and A/B tests.

```bash
# Start the web interface on port 8080
flux web --port 8080

# Start on all interfaces (accessible from other machines on the network)
flux web --bind 0.0.0.0 --port 8080
```

Then open `http://localhost:8080` in your browser.

Key features:
- **Dashboard**: Device fleet overview with live status indicators
- **A/B Testing**: Visual comparison of test variants with metrics and controls
- **Deployment**: One-click deployment to devices or fleets
- **Logs**: Real-time log streaming from any device in the fleet
- **REST API**: Full programmatic access to all features (see WEB-INTERFACE.md)

See **[WEB-INTERFACE.md](WEB-INTERFACE.md)** for the complete Web interface guide.

---

## Chapter 4: Edge IoT Deployment

This is where FLUX OS truly shines. The workflow is: describe your application once in FLUX.MD, build it for any target architecture, and deploy it to your device fleet — all from the same CLI, TUI, or Web interface.

### 4.1 Setting Up Your First Device

1. **Flash FLUX OS to the device** (Raspberry Pi, ESP32, or any supported board)
2. **Connect the device to your network** (WiFi or Ethernet)
3. **Register the device with the fleet manager**

```bash
# Flash FLUX OS to a Raspberry Pi
flux flash --device /dev/sdX --target arm64 --board rpi4

# After flashing and booting the device:
flux device register --name greenhouse-pi-01 --fleet greenhouse

# Verify the device is online
flux device list --fleet greenhouse
```

### 4.2 Deploying Your Application

```bash
# Build your FLUX.MD application for ARM64
flux build --source apps/greenhouse.flux.md --target arm64

# Deploy to the entire greenhouse fleet
flux deploy --fleet greenhouse --strategy rolling --batch-size 25%

# Monitor the deployment
flux deploy status --watch
```

### 4.3 Monitoring Devices

```bash
# Real-time logs from a specific device
flux device logs --device greenhouse-pi-01 --follow

# Device health check
flux device health --device greenhouse-pi-01

# Fleet-wide metrics
flux fleet metrics --fleet greenhouse --interval 5m
```

See **[EDGE-IOT-DEPLOYMENT.md](EDGE-IOT-DEPLOYMENT.md)** for the complete deployment guide.

---

## Chapter 5: Hot-Swap A/B Testing

FLUX OS's hot-swap capability is one of its most powerful features. When you build a new version of your application, the compiler produces a FLUX bytecode binary that can be hot-swapped onto running devices without rebooting. Combined with the A/B testing framework, you can test two versions of your application simultaneously on your fleet and make data-driven rollout decisions.

### 5.1 Running Your First A/B Test

```bash
# Build both variants
flux build --source app-v1.flux.md --output v1.fluxbc
flux build --source app-v2.flux.md --output v2.fluxbc

# Run the A/B test (50/50 split, 24-hour duration)
flux ab-test run \
  --name sensor-test-001 \
  --variant-a v1.fluxbc \
  --variant-b v2.fluxbc \
  --split 50/50 \
  --fleet greenhouse \
  --duration 24h \
  --metrics latency,memory,error_rate,cpu_usage

# Watch the results come in
flux ab-test watch --name sensor-test-001
```

### 5.2 Interpreting Results

After the test completes (or at any time during), you can view detailed metrics comparing the two variants:

```bash
# Get test results
flux ab-test results --name sensor-test-001

# Promote the winning variant to 100%
flux ab-test promote --name sensor-test-001 --winner v2.fluxbc

# Or rollback to the control
flux ab-test rollback --name sensor-test-001
```

See **[HOTSWAP-AB-TESTING.md](HOTSWAP-AB-TESTING.md)** for the complete A/B testing guide.

---

## Chapter 6: The Agent System

FLUX OS treats every process as a potential autonomous agent. Agents can communicate with each other using the A2A (Agent-to-Agent) protocol, delegate tasks to specialized agents, and compose solutions together. This is not an add-on library — it's built into the kernel's syscall interface.

### 6.1 Agent Capabilities

Every agent has a set of capabilities that control what it can do. Capabilities are granted, not assumed — an agent must possess a capability token to perform an action.

| Capability | Hex | Description |
|-----------|-----|-------------|
| SPAWN | 0x01 | Create new agents |
| COMMUNICATE | 0x02 | Send/receive A2A messages |
| COMPILE | 0x04 | Invoke the self-compiler |
| IO_READ | 0x08 | Read from I/O devices |
| IO_WRITE | 0x10 | Write to I/O devices |
| MEMORY | 0x20 | Allocate/deallocate memory |
| HARDWARE | 0x40 | Access hardware directly |
| NETWORK | 0x80 | Send/receive network packets |
| FILESYSTEM | 0x100 | Access the filesystem |
| DEBUG | 0x200 | Use debugging facilities |
| SUPERVISOR | 0x400 | Manage other agents |

### 6.2 A2A Communication Patterns

Agents communicate using typed messages through the A2A protocol. The protocol supports several communication patterns:

**Tell (fire-and-forget):** Send a notification to another agent without expecting a reply. This is the lightest-weight communication pattern and is ideal for event notifications, status updates, and logging.

**Ask (request-reply):** Send a request to another agent and wait for a response. This is used for queries, data requests, and any situation where you need a result back.

**Delegate (task offloading):** Assign a task to a specialized agent and receive the result when it completes. The delegating agent can continue doing other work while the delegate works.

**Broadcast (publish-subscribe):** Send a message to all agents subscribed to a topic. This is used for event distribution and group coordination.

**Barrier (synchronization):** Wait for a group of agents to all reach the same point before continuing. This is used for coordinated phases and distributed consensus.

---

## Chapter 7: Progressive Learning Path

Here's a recommended learning path for new FLUX OS developers:

### Level 1: Observer (Day 1)
- Build and run the hosted-mode tests
- Explore the kernel info output
- Understand the HAL abstraction and why it matters

### Level 2: Builder (Days 2-5)
- Write your first FLUX bytecode program using the instruction encoding helpers
- Load and run it in the VM
- Understand the VM's register file and memory regions
- Build and run a simple agent that communicates via A2A

### Level 3: Compiler (Days 6-14)
- Understand the FIR (SSA IR) and how the compiler pipeline works
- Write a FLUX.MD specification and compile it to bytecode
- Explore the optimization passes (dead code elimination, constant folding)
- Build for multiple targets (native, ARM64, bytecode)

### Level 4: Deployer (Days 15-21)
- Set up your first edge device with FLUX OS
- Deploy an application to the device
- Monitor device health and application logs
- Run your first A/B test

### Level 5: Fleet Operator (Days 22-30)
- Manage a fleet of 10+ devices
- Use the TUI and Web interface for fleet management
- Set up automated deployment pipelines
- Implement monitoring and alerting

### Level 6: Architect (Day 30+)
- Design multi-agent systems with complex A2A communication patterns
- Extend FLUX OS with new HAL backends for custom hardware
- Contribute new compiler optimizations or new opcodes
- Build new tools on top of the FLUX OS platform

---

## Getting Help

- **Documentation**: Start with the [Documentation Index](README.md#documentation-index) in the README
- **Architecture**: See [docs/ARCHITECTURE.md](ARCHITECTURE.md) for deep architectural details
- **CLI Reference**: See [docs/CLI-REFERENCE.md](CLI-REFERENCE.md) for all commands
- **GitHub Issues**: Report bugs and request features at [github.com/SuperInstance/flux-os/issues](https://github.com/SuperInstance/flux-os/issues)
