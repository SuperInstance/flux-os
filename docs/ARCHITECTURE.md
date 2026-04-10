# FLUX OS — Architecture

## System Architecture Deep-Dive

FLUX OS is a microkernel operating system where the kernel itself contains a compiler, a bytecode virtual machine, and an agent runtime. This is fundamentally different from traditional operating systems like Linux or Windows, where the kernel manages hardware and processes but delegates compilation and execution to user-space tools. In FLUX OS, the kernel can write code, compile it, load it as bytecode, execute it in a sandboxed VM, and manage the resulting agents — all without ever leaving kernel-space.

This architecture was chosen to enable the core principle of FLUX OS: the kernel IS the compiler. By embedding the full compilation pipeline in the kernel, FLUX OS achieves several capabilities that are impossible or impractical in traditional OS designs. An agent running inside FLUX OS can describe a new behavior in natural language, the kernel's DEVCODE syscall generates the implementation, the compiler optimizes it for the current hardware, and the VM executes it in a sandboxed region — all within a single syscall round-trip. No external compiler, no build system, no package manager. The OS is self-sufficient.

---

## Layer Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                        USER SPACE                                │
│                                                                  │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────┐  │
│  │   FLUX.MD    │  │   Agent      │  │    User Applications  │  │
│  │   Sources     │  │   Programs   │  │    (native, bytecode) │  │
│  │              │  │   (A2A)      │  │                      │  │
│  └──────┬───────┘  └──────┬───────┘  └──────────┬───────────┘  │
│         │                  │                     │              │
│         └──────────────────┼─────────────────────┘              │
│                            │                                    │
│                     Syscall Interface                            │
│                     (28 syscalls)                                │
│                                                                  │
├────────────────────────────┼────────────────────────────────────┤
│                        KERNEL SPACE                              │
│                                                                  │
│  ┌─────────────────────────▼─────────────────────────────────┐  │
│  │                    FLUX MICROKERNEL                        │  │
│  │                                                           │  │
│  │  ┌─────────────┐  ┌─────────────┐  ┌──────────────┐     │  │
│  │  │  Syscall    │  │  Process    │  │  Scheduler   │     │  │
│  │  │  Dispatcher │  │  Manager    │  │  (priority   │     │  │
│  │  │             │  │  (256 PCBs) │  │   round-robin)│    │  │
│  │  └──────┬──────┘  └──────┬──────┘  └──────┬───────┘     │  │
│  │         │                │                │              │  │
│  │  ┌──────▼──────┐  ┌─────▼───────┐  ┌─────▼──────┐     │  │
│  │  │  Self-      │  │  Bytecode   │  │  Agent      │     │  │
│  │  │  Compiler   │  │  VM (64 reg)│  │  Runtime    │     │  │
│  │  │  (FIR SSA)  │  │             │  │  (A2A)      │     │  │
│  │  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘     │  │
│  │         │                │                │              │  │
│  │  ┌──────▼────────────────▼────────────────▼──────┐      │  │
│  │  │              Memory Manager                    │      │  │
│  │  │         (free-list allocator, types:          │      │  │
│  │  │          KERNEL, USER, DEVICE, BYTECODE,      │      │  │
│  │  │          AGENT, COMPILED)                      │      │  │
│  │  └────────────────────┬──────────────────────────┘      │  │
│  │                       │                                  │  │
│  │  ┌────────────────────▼──────────────────────────┐      │  │
│  │  │               IPC (A2A Messaging)              │      │  │
│  │  │       (send, recv, broadcast, subscribe)       │      │  │
│  │  └────────────────────────────────────────────────┘      │  │
│  │                                                           │  │
│  └────────────────────┬─────────────────────────────────────┘  │
│                       │                                        │
│  ┌────────────────────▼─────────────────────────────────────┐  │
│  │              HARDWARE ABSTRACTION LAYER                  │  │
│  │                                                           │  │
│  │  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌───────────┐  │  │
│  │  │ x86_64  │  │  ARM64  │  │ RISC-V  │  │  WASM32   │  │  │
│  │  │ Backend │  │ Backend │  │ Backend │  │  Backend  │  │  │
│  │  └─────────┘  └─────────┘  └─────────┘  └───────────┘  │  │
│  │                                                           │  │
│  │  Functions: console, memory, VM, CPU, IRQ, timer,        │  │
│  │             context save/restore, port I/O, DMA,          │  │
│  │             device management, power management           │  │
│  └───────────────────────────────────────────────────────────┘  │
│                                                                  │
│                         HARDWARE                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## Subsystem Deep-Dives

### 1. Microkernel (`kernel/`)

The microkernel is the heart of FLUX OS. It manages processes, dispatches syscalls, and coordinates all other subsystems. Unlike monolithic kernels, the FLUX microkernel is deliberately small — it provides only the essential abstractions (processes, memory, IPC, syscalls) and delegates everything else to subsystems that communicate through well-defined C APIs.

**Process Control Block (PCB):** Each process is represented by a `flux_pcb_t` structure that contains the process's state, register context, memory permissions, agent metadata, and bytecode execution state. The kernel maintains a table of 256 PCB slots. The PCB is the single source of truth for everything about a process — the scheduler reads it to decide what to run, the memory manager reads it for permissions, and the agent runtime reads it for capability checks.

**Process States:** FLUX OS extends the traditional process state machine with agent-specific states. Beyond the standard READY, RUNNING, BLOCKED, and ZOMBIE states, FLUX adds AGENT_IDLE (waiting for A2A messages), AGENT_THINKING (processing a task), and COMPILED (process was self-compiled by the kernel). These extra states allow the scheduler to make smarter scheduling decisions — for example, an agent in the THINKING state might be given a longer time slice than an agent in IDLE state.

**Scheduler:** The scheduler uses a priority-based round-robin algorithm. Each process has a priority value from 0 (lowest) to 255 (highest). Within each priority level, processes are scheduled round-robin with a configurable time slice (default: 10 ticks). The scheduler is preemptive — a higher-priority process can preempt a lower-priority one at any timer interrupt. Agent processes get special treatment: the scheduler can boost an agent's priority temporarily when it receives an A2A message, ensuring responsive inter-agent communication.

**Syscall Dispatcher:** All user-space to kernel-space transitions go through the syscall dispatcher. The dispatcher validates the syscall number, checks the caller's capabilities, executes the syscall, and returns the result. The dispatcher supports 28 syscalls organized into 7 categories (process, memory, IPC, bytecode, compiler, I/O, hardware). Each syscall is a C function that the dispatcher calls with the arguments from the caller's registers.

### 2. Hardware Abstraction Layer (`hal/`)

The HAL is what makes FLUX OS "intelligently hardware agnostic." It provides a uniform interface to all hardware through a function pointer table (vtable pattern). The kernel never touches hardware directly — every hardware operation goes through the HAL's function pointers. At boot time, the HAL probes the actual hardware and selects the appropriate backend (x86_64, ARM64, RISC-V, WASM, or native).

**Backend Selection:** The HAL boot sequence follows a three-phase process. First, it calls the `detect_arch()` function to identify the current architecture. Second, it selects the appropriate backend and sets the global HAL pointer. Third, it calls `init_level()` to progressively initialize hardware — starting with console output, then physical memory, virtual memory, CPU features, interrupts, and finally device drivers. Each phase can fail independently, allowing the kernel to boot in degraded mode on limited hardware.

**Function Table:** The `flux_hal_t` structure contains over 60 function pointers organized into 12 categories: identification, console, physical memory, virtual memory, CPU features, interrupts, timer, context save/restore, port I/O, DMA, device management, and power management. Every backend must implement all of these functions. If a backend doesn't support a particular operation (e.g., DMA on a platform without DMA), the function returns FLUX_ERR_GENERAL.

**Runtime Backend Swapping:** The HAL supports hot-plugging of backends. If the kernel detects new hardware at runtime (e.g., a USB device that provides additional CPU capabilities), it can swap the HAL backend without rebooting. This is critical for edge devices that might gain or lose hardware capabilities over their lifetime.

**Hardware Info for Self-Compiler:** The HAL provides two special functions for the self-compiler: `hw_info_dump()` returns a comprehensive description of the hardware, and `hw_optimal_config()` returns the compiler settings that will produce the fastest code for this specific hardware. The self-compiler uses these functions to generate optimized code that takes full advantage of the current platform's capabilities — using AVX512 on Intel, NEON on ARM, or RISC-V Vector extensions.

### 3. Bytecode VM (`vm/`)

The FLUX VM is a register-based virtual machine with 64 general-purpose registers. It is designed to execute FLUX bytecode as a first-class citizen of the operating system, running in kernel-space for performance and supporting sandboxed user-space execution for untrusted code.

**Register File:** The 64 registers follow a calling convention similar to RISC-V:
- R0 (ZERO): Hardwired to zero, writes are ignored
- R1 (RA): Return address
- R2 (SP): Stack pointer
- R3 (BP): Base pointer
- R4 (PC): Program counter
- R5 (FLAGS): Condition flags (zero, negative, carry, overflow, equal, less, greater, trap)
- R6 (FP): Frame pointer
- R7 (T0): Temporary register
- R8-R15 (S0-S7): Callee-saved registers
- R16-R19 (A0-A3): Argument registers
- R20-R22 (T1-T3): Temporary registers
- R32-R47: Agent-specific registers (used by A2A instructions)
- R56-R63: Special-purpose (VM state, cycle counter, region management, syscall number, error code, capability, trap handler)

**Memory Regions:** The VM supports up to 16 independent memory regions per instance. Each region is a contiguous block of memory that can be marked read-only and associated with an owning process. Regions provide the sandboxing mechanism for agents — each agent gets its own region, and the VM enforces memory isolation between regions. Region operations (create, destroy, read, write) are system calls that go through the capability check.

**Execution Model:** The VM supports both step-by-step execution (for debugging) and continuous execution with a cycle limit (for production). When running in continuous mode, the VM executes instructions until it encounters a HALT instruction, hits a breakpoint, exhausts its cycle budget, or encounters an error. The cycle budget mechanism prevents runaway agents from consuming unlimited CPU time.

**Tracing and Profiling:** The VM has built-in execution tracing and profiling. When tracing is enabled, each instruction execution records the PC, opcode, and a snapshot of R0-R7 into a circular trace buffer. Profiling counts the number of executions per opcode, allowing performance analysis. Both tracing and profiling can be enabled and disabled at runtime without restarting the VM.

### 4. Self-Compiler (`fluxc/`)

The self-compiler is the most architecturally distinctive subsystem of FLUX OS. It translates FLUX.MD source (markdown) into an SSA-based intermediate representation called FIR (FLUX IR), and then generates bytecode, C code, or native assembly from the FIR.

**FIR (FLUX Intermediate Representation):** FIR is an SSA-based IR where every value is defined exactly once. Programs are organized into modules, which contain functions, which contain basic blocks, which contain values (instructions). Each value has an ID, a type, an opcode, and operands (references to other values by ID). The SSA property simplifies optimization because every use of a value can be traced back to exactly one definition.

FIR supports 17 types including not just the usual integer, float, pointer, and boolean types, but also domain-specific types like `FIR_TYPE_AGENT` (agent handle), `FIR_TYPE_CAPABILITY` (capability token), `FIR_TYPE_BYTECODE` (bytecode reference), and `FIR_TYPE_REGION` (memory region). These types are first-class in the IR, meaning the optimizer and codegen understand their semantics.

**Compilation Pipeline:** The pipeline has four stages:
1. **Lexing**: The FLUX.MD lexer tokenizes the markdown source into a stream of tokens, recognizing both standard markdown syntax and FLUX-specific directives (## Compile, ## Agent, ## Loop, etc.)
2. **Parsing**: The parser consumes the token stream and produces an abstract syntax tree (AST) that represents the program's structure
3. **FIR Generation**: The FIR generator walks the AST and produces SSA-form IR, inserting phi nodes at basic block boundaries for variable renaming
4. **Optimization**: The optimizer applies a series of passes to the FIR module: dead code elimination, constant folding, and inlining of small functions. Three optimization levels are available: 0 (none), 1 (speed), 2 (size), and 3 (aggressive)

**Backend Code Generation:** The optimized FIR is then handed to a backend that produces the desired output format:
- **Bytecode Codegen**: Translates FIR instructions directly to FLUX bytecode opcodes
- **C Codegen**: Emits C source code that can be compiled with any C compiler
- **Native Codegen**: Uses HAL hardware info to emit architecture-specific assembly

**DEVCODE Mode:** The DEVCODE syscall enables the OS to act as a developer. When invoked, the kernel receives a natural language description of what code to generate, along with constraints (target architecture, optimization level, SIMD usage, complexity budget). The DEVCODE subsystem uses the FIR pipeline to generate working code from the description. This is the mechanism that enables agent-driven development: an operator agent can describe a new behavior, the OS generates the implementation, and the agent deploys it.

### 5. Agent Runtime (`agent/`)

The agent runtime provides the A2A (Agent-to-Agent) protocol layer that makes FLUX OS agent-native. Every process can optionally be an autonomous agent with the ability to communicate, delegate, and compose solutions with other agents.

**Agent Lifecycle:** Agents go through a well-defined lifecycle: CREATED (allocated), INITIALIZING (running init code, publishing capabilities), IDLE (waiting for messages), THINKING (processing a task), WAITING (waiting for a reply), DELEGATING (has offloaded work), TERMINATED (clean shutdown), or FAILED (error shutdown). The agent runtime manages transitions between these states and enforces lifecycle rules.

**Capability Security:** FLUX OS uses a capability-based security model instead of the traditional user/group/permission model. Each agent holds a bitmask of capabilities that control what it can do. Capabilities are not inherited from a parent process — they are explicitly granted, and can be revoked or transferred between agents. The 11 capabilities (SPAWN, COMMUNICATE, COMPILE, IO_READ, IO_WRITE, MEMORY, HARDWARE, NETWORK, FILESYSTEM, DEBUG, SUPERVISOR) cover all operations an agent might want to perform.

**A2A Message Format:** Agents communicate through typed messages. Each message has a unique ID, a type (TELL, ASK, REPLY, DELEGATE, RESULT, BARRIER, SUBSCRIBE, EVENT, ERROR, CAPABILITY, HEARTBEAT), a sender and target agent ID, a topic (for pub-sub), a payload (up to 4096 bytes), a timestamp, and an optional deadline. Messages pass through the kernel's IPC subsystem, which validates that the sender has the COMMUNICATE capability and that the payload does not exceed size limits.

**Agent Discovery:** Agents can publish their capabilities (what they can do) and discover other agents by capability. The discovery system allows an agent to say "I need an agent that can read temperature sensors" and get back a list of agents that have published that capability. This enables dynamic composition of agent teams without hardcoded dependencies.

**Sandboxing:** Each agent runs in an isolated VM region with its own memory space and register file. The VM enforces memory isolation — an agent cannot read or write another agent's memory without going through the A2A messaging system. The agent runtime also enforces resource limits: CPU time, memory usage, and I/O bandwidth are all capped per-agent, preventing a misbehaving agent from affecting the rest of the system.

---

## Data Flow Examples

### Example 1: Compiling and Running a FLUX.MD Application

```
User writes FLUX.MD
        │
        ▼
flux build --source app.flux.md --target arm64
        │
        ▼
┌───────────────────┐
│ 1. Lexer          │  FLUX.MD → token stream
├───────────────────┤
│ 2. Parser         │  token stream → AST
├───────────────────┤
│ 3. FIR Generator  │  AST → FIR module (SSA)
├───────────────────┤
│ 4. Optimizer      │  FIR → optimized FIR
│    - DCE          │    (dead code elimination)
│    - ConstFold    │    (constant folding)
│    - Inline       │    (small function inlining)
├───────────────────┤
│ 5. Codegen        │  FIR → ARM64 assembly
├───────────────────┤
│ 6. Emitter        │  assembly → .fluxbc binary
└───────────────────┘
        │
        ▼
flux deploy --fleet my-fleet
        │
        ▼
Binary pushed to devices → hot-swapped into running agents
```

### Example 2: Agent A2A Communication

```
Agent A (temp-monitor)          Agent B (alert-handler)
        │                              │
        │── ASK: temperature at site? ─→│
        │                              │
        │                     [reads sensor]
        │                              │
        │←── REPLY: 42.5°C ───────────│
        │                              │
        │  [checks threshold: 42.5 > 40]
        │                              │
        │── TELL: alert: high temp ────→│
        │                              │
        │                     [sends notification]
        │                              │
```

---

## Memory Layout

```
Low Address                              High Address
┌──────────────────────────────────────────────────┐
│  Boot Code (loaded by bootloader)                 │
│  ─────────────────────────────────────            │
│  Kernel Image (.text, .rodata, .data, .bss)      │
│  ─────────────────────────────────────            │
│  Kernel Heap (free-list allocator)                │
│    ├── Process Control Blocks (256 × PCB)         │
│    ├── Agent Descriptors                          │
│    ├── IPC Message Queues                         │
│    └── Compiler Working Memory                    │
│  ─────────────────────────────────────            │
│  Bytecode Region (FLUX_MEM_BYTECODE)              │
│    ├── Agent 10 bytecode                          │
│    ├── Agent 11 bytecode                          │
│    └── Agent 12 bytecode                          │
│  ─────────────────────────────────────            │
│  VM Memory Regions (per-instance)                 │
│    ├── Region 0: Agent 10 workspace               │
│    ├── Region 1: Agent 11 workspace               │
│    └── Region 2: Agent 12 workspace               │
│  ─────────────────────────────────────            │
│  User Space Stacks (64KB per process)             │
│  ─────────────────────────────────────            │
│  Device Memory Mappings (FLUX_MEM_DEVICE)         │
│  ─────────────────────────────────────            │
│  Free Memory                                     │
└──────────────────────────────────────────────────┘
```

---

## Key Design Decisions

### Why C11?
Pure C was chosen for maximum portability and minimal runtime overhead. FLUX OS targets bare-metal environments where there is no C++ runtime, no Rust standard library, and no garbage collector. C11 provides just enough abstraction (type safety, designated initializers, static assertions) to write clean, maintainable code while producing the smallest possible binary. Every embedded platform in existence has a C compiler; the same cannot be said for C++ or Rust.

### Why a Register-Based VM?
Register-based VMs (like Lua VM and RISC-V) generally outperform stack-based VMs (like the JVM) because they avoid the overhead of pushing and popping operands for every instruction. The 64-register design gives the compiler enough working registers to keep values in registers across multiple operations, reducing memory traffic. The calling convention (R0=zero, R1=RA, R2-R7=special, R8-R15=callee-saved, R16-R19=args, R20-R22=temps, R32-R47=agent, R56-R63=system) provides a natural division of labor that maps well to both the ISA and to the high-level concepts (agents, capabilities, memory regions) that FLUX OS needs to express.

### Why Capability-Based Security?
Traditional Unix-style permissions (user/group/read/write/execute) are too coarse for an agent-native OS. Agents need fine-grained, delegatable, and composable permissions. Capabilities provide this: they are unforgeable tokens that can be granted, revoked, and transferred between agents. An agent that can read sensors (IO_READ) cannot automatically write to them (IO_WRITE) — it must explicitly request that capability. This model also extends naturally to A2A communication: an agent can transfer a subset of its capabilities to another agent for delegation, and revoke them when the delegation is complete.

### Why SSA-Based IR?
SSA (Static Single Assignment) is the standard IR form for modern compilers (LLVM uses it, GCC uses it internally). The key advantage is that every value is defined exactly once, which makes data flow analysis trivial and enables powerful optimizations like constant propagation, dead code elimination, and global value numbering. FIR extends the standard SSA form with domain-specific types (agent, capability, bytecode, region) that allow the optimizer to reason about agent interactions at the IR level.
