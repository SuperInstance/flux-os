/*
 * FLUX OS — Intelligently Hardware-Agnostic Operating System
 * Copyright (c) 2025 SuperInstance
 *
 * The FLUX Kernel is a microkernel designed around agent-first computing.
 * It manages processes, memory, IPC, and provides syscall interfaces that
 * allow the FLUX VM and self-compiler to operate from kernel-space to
 * user-space seamlessly.
 *
 * Architecture Principles:
 *   1. The kernel IS the compiler — it can write, compile, and execute code
 *   2. Hardware agnostic — all hardware access goes through the HAL
 *   3. Agent-native — processes are agents with A2A protocol built in
 *   4. Bytecode-first — FLUX bytecode is a first-class execution format
 *   5. Self-hosting — the OS can rebuild itself from FLUX.MD specifications
 */

#ifndef FLUX_KERNEL_H
#define FLUX_KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Version & Build Metadata
 * ======================================================================== */

#define FLUX_OS_VERSION_MAJOR    0
#define FLUX_OS_VERSION_MINOR    1
#define FLUX_OS_VERSION_PATCH    0
#define FLUX_OS_VERSION_STRING   "0.1.0"

#define FLUX_KERNEL_NAME         "FLUX Microkernel"
#define FLUX_KERNEL_ARCH_FLAGS   "hw_agnostic"

/* ========================================================================
 * Fundamental Types
 * ======================================================================== */

typedef uint64_t    flux_addr_t;
typedef uint64_t    flux_size_t;
typedef uint64_t    flux_ticks_t;
typedef uint32_t    flux_pid_t;
typedef uint32_t    flux_tid_t;
typedef uint64_t    flux_cap_t;      /* Capability token for A2A security */
typedef int32_t     flux_status_t;

#define FLUX_PID_INVALID   ((flux_pid_t)0)
#define FLUX_PID_KERNEL    ((flux_pid_t)1)
#define FLUX_PID_FIRST_APP ((flux_pid_t)2)

#define FLUX_OK             0
#define FLUX_ERR_GENERAL   -1
#define FLUX_ERR_NOMEM     -2
#define FLUX_ERR_INVALID   -3
#define FLUX_ERR_DENIED    -4
#define FLUX_ERR_TIMEOUT   -5
#define FLUX_ERR_BUSY      -6
#define FLUX_ERR_NOTFOUND  -7
#define FLUX_ERR_EXISTS    -8
#define FLUX_ERR_OVERFLOW  -9
#define FLUX_ERR_DEADLOCK  -10

/* ========================================================================
 * Process States
 * ======================================================================== */

typedef enum {
    FLUX_PROC_UNUSED = 0,
    FLUX_PROC_READY,
    FLUX_PROC_RUNNING,
    FLUX_PROC_BLOCKED,
    FLUX_PROC_ZOMBIE,
    FLUX_PROC_AGENT_IDLE,    /* Agent waiting for A2A message */
    FLUX_PROC_AGENT_THINKING,/* Agent processing reasoning */
    FLUX_PROC_COMPILED,      /* Process was compiled by self-compiler */
} flux_proc_state_t;

/* ========================================================================
 * Memory Types
 * ======================================================================== */

typedef enum {
    FLUX_MEM_ANY       = 0,
    FLUX_MEM_KERNEL    = 1,
    FLUX_MEM_USER      = 2,
    FLUX_MEM_DEVICE    = 3,
    FLUX_MEM_BYTECODE  = 4,  /* FLUX bytecode region */
    FLUX_MEM_AGENT     = 5,  /* Agent workspace */
    FLUX_MEM_COMPILED  = 6,  /* Self-compiled code region */
} flux_mem_type_t;

typedef enum {
    FLUX_PERM_NONE     = 0,
    FLUX_PERM_READ     = (1 << 0),
    FLUX_PERM_WRITE    = (1 << 1),
    FLUX_PERM_EXEC     = (1 << 2),
    FLUX_PERM_RW       = (FLUX_PERM_READ | FLUX_PERM_WRITE),
    FLUX_PERM_RWX      = (FLUX_PERM_READ | FLUX_PERM_WRITE | FLUX_PERM_EXEC),
} flux_perm_t;

/* ========================================================================
 * System Call Numbers
 * ======================================================================== */

typedef enum {
    /* Process management */
    FLUX_SYSCALL_SPAWN      = 100,
    FLUX_SYSCALL_YIELD      = 101,
    FLUX_SYSCALL_EXIT       = 102,
    FLUX_SYSCALL_KILL       = 103,
    FLUX_SYSCALL_WAIT       = 104,
    FLUX_SYSCALL_GETPID     = 105,

    /* Memory management */
    FLUX_SYSCALL_ALLOC      = 200,
    FLUX_SYSCALL_FREE       = 201,
    FLUX_SYSCALL_MMAP       = 202,
    FLUX_SYSCALL_MUNMAP     = 203,
    FLUX_SYSCALL_MPROTECT   = 204,

    /* IPC (Agent-to-Agent) */
    FLUX_SYSCALL_A2A_SEND   = 300,
    FLUX_SYSCALL_A2A_RECV   = 301,
    FLUX_SYSCALL_A2A_BROADCAST = 302,
    FLUX_SYSCALL_A2A_SUBSCRIBE = 303,

    /* FLUX Bytecode execution */
    FLUX_SYSCALL_BC_LOAD    = 400,
    FLUX_SYSCALL_BC_EXEC    = 401,
    FLUX_SYSCALL_BC_STATUS  = 402,
    FLUX_SYSCALL_BC_DUMP    = 403,

    /* Self-Compiler */
    FLUX_SYSCALL_COMPILE    = 500,  /* Compile FLUX.MD → bytecode/C/native */
    FLUX_SYSCALL_EMIT       = 501,  /* Emit generated code */
    FLUX_SYSCALL_DEVCODE    = 502,  /* OS acts as developer: write new code */

    /* I/O */
    FLUX_SYSCALL_READ       = 600,
    FLUX_SYSCALL_WRITE      = 601,
    FLUX_SYSCALL_IOCTL      = 602,
    FLUX_SYSCALL_OPEN       = 603,
    FLUX_SYSCALL_CLOSE      = 604,

    /* Hardware queries */
    FLUX_SYSCALL_HW_INFO    = 700,  /* Query hardware capabilities */
    FLUX_SYSCALL_HW_CONFIG  = 701,  /* Reconfigure hardware abstraction */

    /* Info */
    FLUX_SYSCALL_INFO       = 800,
    FLUX_SYSCALL_LOG        = 801,
} flux_syscall_t;

/* ========================================================================
 * Process Control Block
 * ======================================================================== */

#define FLUX_PROC_NAME_LEN    64
#define FLUX_MAX_REGS         64
#define FLUX_STACK_SIZE       (64 * 1024)  /* 64KB default stack */

typedef struct {
    flux_pid_t         pid;
    flux_pid_t         parent_pid;
    flux_proc_state_t  state;
    char               name[FLUX_PROC_NAME_LEN];
    uint8_t            priority;          /* 0 = lowest, 255 = highest */
    flux_ticks_t       cpu_time;
    flux_ticks_t       create_time;
    flux_cap_t         capabilities;      /* Capability bitmask */
    flux_perm_t        mem_permissions;

    /* Execution context (platform-neutral, populated by HAL) */
    uint64_t           regs[FLUX_MAX_REGS];
    flux_addr_t        stack_ptr;
    flux_addr_t        instr_ptr;
    flux_addr_t        page_table;

    /* Agent metadata */
    uint32_t           agent_id;
    bool               is_agent;
    bool               is_compiled;       /* Was this process self-compiled? */
    char               agent_model[32];   /* e.g., "flux:bytecode", "native:x86" */

    /* Bytecode execution state */
    struct {
        bool           active;
        flux_addr_t    bytecode_base;
        flux_size_t    bytecode_len;
        flux_size_t    pc;               /* Program counter */
        uint64_t       vm_regs[FLUX_MAX_REGS];
    } bc;

    /* Wait queue */
    flux_pid_t         waiting_for;
} flux_pcb_t;

/* ========================================================================
 * Kernel Boot State
 * ======================================================================== */

typedef struct {
    bool               initialized;
    bool               hal_ready;
    bool               vm_ready;
    bool               compiler_ready;
    bool               agent_ready;

    uint32_t           num_processes;
    flux_pid_t         next_pid;
    flux_pid_t         current_pid;

    flux_size_t        total_memory;
    flux_size_t        free_memory;

    uint32_t           tick_count;
    uint32_t           syscall_count;
    uint32_t           compile_count;    /* Times the self-compiler was invoked */

    /* Hardware info (populated by HAL) */
    struct {
        char           arch[32];
        char           cpu_vendor[32];
        uint32_t       num_cores;
        uint64_t       cpu_freq_hz;
        uint64_t       total_ram;
        char           hal_version[16];
    } hw;
} flux_kernel_state_t;

/* ========================================================================
 * Kernel API — Core Functions
 * ======================================================================== */

/* Initialize all kernel subsystems (called from boot) */
flux_status_t flux_kernel_init(void);

/* Main kernel loop — never returns */
void flux_kernel_run(void);

/* Process management */
flux_pid_t     flux_spawn(const char *name, flux_addr_t entry, flux_size_t stack_size, bool is_agent);
flux_status_t  flux_yield(void);
flux_status_t  flux_exit(int code);
flux_status_t  flux_kill(flux_pid_t pid);
flux_pid_t     flux_getpid(void);
flux_pcb_t    *flux_get_pcb(flux_pid_t pid);

/* Memory management */
void          *flux_alloc(flux_size_t size, flux_mem_type_t type);
void           flux_free(void *ptr);
flux_status_t  flux_mem_protect(flux_addr_t addr, flux_size_t size, flux_perm_t perm);

/* IPC */
flux_status_t  flux_a2a_send(flux_pid_t target, const void *msg, flux_size_t len);
flux_status_t  flux_a2a_recv(flux_pid_t *sender, void *buf, flux_size_t *len, flux_ticks_t timeout);
flux_status_t  flux_a2a_broadcast(const void *msg, flux_size_t len);

/* Self-Compiler interface */
flux_status_t  flux_compile_source(const char *flux_md, flux_size_t len,
                                   char *output, flux_size_t out_len, bool to_c);
flux_status_t  flux_devcode(const char *description, char *output, flux_size_t out_len);

/* Bytecode execution */
flux_status_t  flux_bc_load(flux_pid_t pid, const uint8_t *bytecode, flux_size_t len);
flux_status_t  flux_bc_exec(flux_pid_t pid);
flux_status_t  flux_bc_dump(flux_pid_t pid, uint8_t *buf, flux_size_t *len);

/* Kernel state queries */
flux_kernel_state_t *flux_kernel_state(void);
const char *flux_kernel_info(void);
void flux_panic(const char *reason) __attribute__((noreturn));

/* System call dispatcher */
flux_status_t flux_syscall_dispatch(flux_syscall_t num, void *arg0, void *arg1,
                                    void *arg2, void *arg3, void *arg4);

/* Logging */
void flux_log(const char *fmt, ...);
void flux_log_debug(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* FLUX_KERNEL_H */
