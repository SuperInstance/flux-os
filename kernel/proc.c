/*
 * FLUX OS — Process Management Subsystem
 *
 * Manages the Process Control Block (PCB) table, process creation,
 * termination, and lifecycle transitions. Every entity in FLUX OS —
 * whether a user process, an agent, or a bytecode VM instance — is
 * represented as a process with a PCB.
 *
 * Design:
 *   - Fixed-size PCB table (max 256 processes)
 *   - PID allocation via linear scan with wrap-around
 *   - Stack allocation from the kernel memory manager
 *   - Parent-child relationships for process trees
 *   - Agent-aware: agents are processes with special metadata
 *   - Bytecode-aware: processes can have an active VM state
 *   - Cleanup on exit: frees stack, unregisters from scheduler
 *
 * Process States:
 *   UNUSED → READY → RUNNING → (BLOCKED/AGENT_IDLE/AGENT_THINKING) → ZOMBIE
 */

#include "flux/kernel.h"
#include "flux/hal.h"
#include "flux/vm.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* Capability constants (from agent.h — replicated here to avoid header conflict) */
#ifndef FLUX_CAP_ALL
#define FLUX_CAP_NONE           0x0000000000000000ULL
#define FLUX_CAP_SPAWN          0x0000000000000001ULL
#define FLUX_CAP_COMMUNICATE    0x0000000000000002ULL
#define FLUX_CAP_COMPILE        0x0000000000000004ULL
#define FLUX_CAP_IO_READ        0x0000000000000008ULL
#define FLUX_CAP_IO_WRITE       0x0000000000000010ULL
#define FLUX_CAP_MEMORY         0x0000000000000020ULL
#define FLUX_CAP_HARDWARE       0x0000000000000040ULL
#define FLUX_CAP_NETWORK        0x0000000000000080ULL
#define FLUX_CAP_FILESYSTEM     0x0000000000000100ULL
#define FLUX_CAP_DEBUG          0x0000000000000200ULL
#define FLUX_CAP_SUPERVISOR     0x0000000000000400ULL
#define FLUX_CAP_ALL            0xFFFFFFFFFFFFFFFFULL
#endif

/* ========================================================================
 * Configuration
 * ======================================================================== */

#ifndef FLUX_MAX_PROCESSES
#define FLUX_MAX_PROCESSES     256
#endif

#ifndef FLUX_STACK_SIZE_DEFAULT
#define FLUX_STACK_SIZE_DEFAULT FLUX_STACK_SIZE  /* 64KB from kernel.h */
#endif

#ifndef FLUX_STACK_SIZE_MIN
#define FLUX_STACK_SIZE_MIN    (4 * 1024)   /* 4 KB minimum */
#endif

/* ========================================================================
 * Static State — Process Table
 * ======================================================================== */

/* The global PCB table */
static flux_pcb_t s_pcb_table[FLUX_MAX_PROCESSES];

/* PID allocation watermark — next PID to try */
static flux_pid_t s_next_pid = FLUX_PID_FIRST_APP;

/* Spinlock for process table access */
static volatile int s_proc_lock = 0;

/* Count of active (non-UNUSED) processes */
static uint32_t s_active_count = 0;

/* Track agent processes separately */
static uint32_t s_agent_count = 0;

/* ========================================================================
 * Internal Helpers
 * ======================================================================== */

/*
 * proc_lock / proc_unlock — Spinlock for process table operations.
 */
static inline void proc_lock(void)
{
    while (__atomic_exchange_n(&s_proc_lock, 1, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause" ::: "memory");
    }
}

static inline void proc_unlock(void)
{
    __atomic_store_n(&s_proc_lock, 0, __ATOMIC_RELEASE);
}

/*
 * proc_pid_to_index — Convert a PID to a PCB table index.
 * Since PIDs are assigned sequentially starting from 1, the index is
 * PID - 1 (with wrap-around). This is NOT the same as PID for PID=0
 * (which is invalid).
 */
static inline int proc_pid_to_index(flux_pid_t pid)
{
    if (pid == FLUX_PID_INVALID || pid == 0)
        return -1;

    int idx = (int)(pid - 1) % FLUX_MAX_PROCESSES;
    return idx;
}

/*
 * proc_allocate_pid — Find the next available PID.
 * Scans forward from s_next_pid looking for an UNUSED slot.
 * Returns FLUX_PID_INVALID if the table is full.
 */
static flux_pid_t proc_allocate_pid(void)
{
    for (uint32_t i = 0; i < FLUX_MAX_PROCESSES; i++) {
        flux_pid_t candidate = s_next_pid;

        /* Skip kernel PID */
        if (candidate <= FLUX_PID_KERNEL) {
            s_next_pid = FLUX_PID_FIRST_APP;
            continue;
        }

        int idx = proc_pid_to_index(candidate);
        if (idx >= 0 && s_pcb_table[idx].state == FLUX_PROC_UNUSED) {
            s_next_pid = candidate + 1;
            if (s_next_pid <= FLUX_PID_KERNEL)
                s_next_pid = FLUX_PID_FIRST_APP;
            return candidate;
        }

        s_next_pid++;
        if (s_next_pid <= FLUX_PID_KERNEL)
            s_next_pid = FLUX_PID_FIRST_APP;
    }

    return FLUX_PID_INVALID;
}

/*
 * proc_find_unused_slot — Find a free slot in the PCB table.
 * This is a backup allocation strategy that scans by index.
 */
static int proc_find_unused_slot(void)
{
    for (int i = 0; i < FLUX_MAX_PROCESSES; i++) {
        if (s_pcb_table[i].state == FLUX_PROC_UNUSED)
            return i;
    }
    return -1;
}

/*
 * proc_state_name — Human-readable process state name.
 */
static const char *proc_state_name(flux_proc_state_t state)
{
    switch (state) {
        case FLUX_PROC_UNUSED:       return "UNUSED";
        case FLUX_PROC_READY:        return "READY";
        case FLUX_PROC_RUNNING:      return "RUNNING";
        case FLUX_PROC_BLOCKED:      return "BLOCKED";
        case FLUX_PROC_ZOMBIE:       return "ZOMBIE";
        case FLUX_PROC_AGENT_IDLE:   return "AGENT_IDLE";
        case FLUX_PROC_AGENT_THINKING: return "AGENT_THINKING";
        case FLUX_PROC_COMPILED:     return "COMPILED";
        default:                     return "UNKNOWN";
    }
}

/*
 * proc_cleanup — Clean up a process's resources.
 * Frees the stack memory and marks the PCB as UNUSED.
 */
static void proc_cleanup(flux_pcb_t *pcb)
{
    if (!pcb)
        return;

    /* Free stack memory if allocated */
    if (pcb->stack_ptr != 0) {
        /* Stack was allocated via flux_alloc; free it */
        /* In our scheme, stack_ptr points to the stack base (top) */
        /* The actual allocation started at stack_ptr - stack_size */
        /* For now we just zero the pointer since stack tracking
         * is handled by the memory manager via owner_pid cleanup */
        pcb->stack_ptr = 0;
    }

    /* Clear bytecode state */
    if (pcb->bc.active) {
        pcb->bc.active = false;
        pcb->bc.bytecode_base = 0;
        pcb->bc.bytecode_len = 0;
        pcb->bc.pc = 0;
        memset(pcb->bc.vm_regs, 0, sizeof(pcb->bc.vm_regs));
    }

    /* Reset agent tracking */
    if (pcb->is_agent) {
        s_agent_count--;
        pcb->is_agent = false;
    }

    /* Mark slot as unused */
    pcb->state = FLUX_PROC_UNUSED;
    pcb->pid = FLUX_PID_INVALID;
    pcb->name[0] = '\0';
    pcb->waiting_for = FLUX_PID_INVALID;

    s_active_count--;
}

/* ========================================================================
 * Process Subsystem Initialization
 * ======================================================================== */

/*
 * flux_proc_init — Initialize the process management subsystem.
 *
 * Sets up the kernel process (PID 1) and clears all other PCB slots.
 * The kernel process is always present and has supervisor capabilities.
 */
flux_status_t flux_proc_init(void)
{
    proc_lock();

    /* Clear entire PCB table */
    memset(s_pcb_table, 0, sizeof(s_pcb_table));

    for (int i = 0; i < FLUX_MAX_PROCESSES; i++) {
        s_pcb_table[i].state = FLUX_PROC_UNUSED;
        s_pcb_table[i].pid = FLUX_PID_INVALID;
        s_pcb_table[i].parent_pid = FLUX_PID_INVALID;
        s_pcb_table[i].waiting_for = FLUX_PID_INVALID;
    }

    s_active_count = 0;
    s_agent_count = 0;
    s_next_pid = FLUX_PID_FIRST_APP;

    /* Create the kernel process (PID 1) */
    flux_pcb_t *kernel_proc = &s_pcb_table[0]; /* Index 0 = PID 1 */
    kernel_proc->pid = FLUX_PID_KERNEL;
    kernel_proc->parent_pid = FLUX_PID_INVALID;
    kernel_proc->state = FLUX_PROC_RUNNING;
    kernel_proc->priority = 255;  /* Highest priority */
    kernel_proc->capabilities = FLUX_CAP_ALL;
    kernel_proc->mem_permissions = FLUX_PERM_RWX;
    kernel_proc->create_time = 0;

    /* Give it a name */
    const char *kname = "kernel";
    for (int i = 0; kname[i] && i < FLUX_PROC_NAME_LEN - 1; i++)
        kernel_proc->name[i] = kname[i];
    kernel_proc->name[sizeof("kernel") - 1] = '\0';

    /* The kernel process doesn't have a traditional stack/context */
    kernel_proc->stack_ptr = 0;
    kernel_proc->instr_ptr = 0;
    kernel_proc->page_table = 0;
    kernel_proc->is_agent = false;
    kernel_proc->is_compiled = false;
    kernel_proc->bc.active = false;
    kernel_proc->waiting_for = FLUX_PID_INVALID;

    s_active_count = 1;

    /* Update kernel state */
    flux_kernel_state_t *ks = flux_kernel_state();
    if (ks) {
        ks->num_processes = 1;
        ks->current_pid = FLUX_PID_KERNEL;
        ks->next_pid = FLUX_PID_FIRST_APP;
    }

    proc_unlock();

    return FLUX_OK;
}

/* ========================================================================
 * Public API
 * ======================================================================== */

/*
 * flux_spawn — Create a new process.
 *
 * Parameters:
 *   name        — Process name (truncated to 63 chars)
 *   entry       — Entry point address (0 for bytecode processes)
 *   stack_size  — Stack size in bytes (0 for default)
 *   is_agent    — Whether this process is an agent
 *
 * Returns:
 *   New PID on success, FLUX_PID_INVALID on failure.
 */
flux_pid_t flux_spawn(const char *name, flux_addr_t entry,
                      flux_size_t stack_size, bool is_agent)
{
    if (s_active_count >= FLUX_MAX_PROCESSES) {
        flux_log("spawn failed: process table full (%d/%d)",
                 s_active_count, FLUX_MAX_PROCESSES);
        return FLUX_PID_INVALID;
    }

    /* Default stack size */
    if (stack_size == 0)
        stack_size = FLUX_STACK_SIZE_DEFAULT;

    /* Enforce minimum */
    if (stack_size < FLUX_STACK_SIZE_MIN)
        stack_size = FLUX_STACK_SIZE_MIN;

    proc_lock();

    /* Allocate PID */
    flux_pid_t pid = proc_allocate_pid();
    if (pid == FLUX_PID_INVALID) {
        /* Try slot-based allocation as fallback */
        int slot = proc_find_unused_slot();
        if (slot < 0) {
            proc_unlock();
            return FLUX_PID_INVALID;
        }
        pid = (flux_pid_t)(slot + 1);
    }

    int idx = proc_pid_to_index(pid);
    if (idx < 0) {
        proc_unlock();
        return FLUX_PID_INVALID;
    }

    flux_pcb_t *pcb = &s_pcb_table[idx];

    /* Allocate stack */
    void *stack = flux_alloc(stack_size, is_agent ? FLUX_MEM_AGENT : FLUX_MEM_KERNEL);
    if (!stack) {
        proc_unlock();
        return FLUX_PID_INVALID;
    }

    /* Get current tick for create_time */
    flux_ticks_t create_time = 0;
    flux_kernel_state_t *ks = flux_kernel_state();
    if (ks)
        create_time = (flux_ticks_t)ks->tick_count;

    /* Get parent PID */
    flux_pid_t parent = ks ? ks->current_pid : FLUX_PID_KERNEL;

    /* Initialize the PCB */
    memset(pcb, 0, sizeof(flux_pcb_t));

    pcb->pid = pid;
    pcb->parent_pid = parent;
    pcb->state = FLUX_PROC_READY;
    pcb->priority = 128;  /* Default medium priority */
    pcb->cpu_time = 0;
    pcb->create_time = create_time;
    pcb->capabilities = FLUX_CAP_SPAWN | FLUX_CAP_COMMUNICATE;
    pcb->mem_permissions = is_agent ? FLUX_PERM_RW : FLUX_PERM_RWX;

    /* Set up the stack pointer (grows downward, so SP = top of stack) */
    pcb->stack_ptr = (flux_addr_t)((uint8_t *)stack + stack_size);
    pcb->instr_ptr = entry;
    pcb->page_table = 0;  /* Use parent's page table for now */

    /* Set initial register state */
    memset(pcb->regs, 0, sizeof(pcb->regs));
    /* SP register (R2 in VM) */
    pcb->regs[2] = pcb->stack_ptr;
    /* Entry point in PC register (R4) */
    pcb->regs[4] = entry;

    /* Name */
    if (name) {
        int i;
        for (i = 0; i < FLUX_PROC_NAME_LEN - 1 && name[i]; i++)
            pcb->name[i] = name[i];
        pcb->name[i] = '\0';
    } else {
        /* Auto-generate name */
        snprintf(pcb->name, FLUX_PROC_NAME_LEN, "proc-%u", pid);
    }

    /* Agent metadata */
    pcb->is_agent = is_agent;
    pcb->is_compiled = false;
    pcb->agent_id = is_agent ? (uint32_t)pid : 0;
    pcb->waiting_for = FLUX_PID_INVALID;

    if (is_agent) {
        /* Set default agent model */
        const char *model = "flux:bytecode";
        for (int i = 0; model[i] && i < 31; i++)
            pcb->agent_model[i] = model[i];
        pcb->agent_model[sizeof("flux:bytecode") - 1] = '\0';
        s_agent_count++;
    }

    /* Bytecode state — not active by default */
    pcb->bc.active = false;
    pcb->bc.bytecode_base = 0;
    pcb->bc.bytecode_len = 0;
    pcb->bc.pc = 0;
    memset(pcb->bc.vm_regs, 0, sizeof(pcb->bc.vm_regs));

    s_active_count++;

    /* Update kernel state */
    if (ks) {
        ks->num_processes = s_active_count;
        ks->next_pid = s_next_pid;
    }

    proc_unlock();

    flux_log("spawned process '%s' pid=%u parent=%u stack=%llu agent=%s",
             pcb->name, pid, parent,
             (unsigned long long)stack_size,
             is_agent ? "yes" : "no");

    /* Forward declaration — scheduler enqueue happens in sched.c */
    extern flux_status_t flux_sched_enqueue(flux_pid_t pid);
    flux_sched_enqueue(pid);

    return pid;
}

/*
 * flux_exit — Terminate the current process.
 *
 * Parameters:
 *   code — Exit code (0 = success, non-zero = error)
 *
 * Returns:
 *   Never returns (triggers context switch).
 */
flux_status_t flux_exit(int code)
{
    flux_kernel_state_t *ks = flux_kernel_state();
    if (!ks || ks->current_pid == FLUX_PID_INVALID)
        return FLUX_ERR_INVALID;

    flux_pid_t pid = ks->current_pid;

    /* Kernel process cannot exit */
    if (pid == FLUX_PID_KERNEL) {
        flux_log("FATAL: kernel process attempted to exit!");
        flux_panic("kernel process exit attempted");
    }

    proc_lock();

    int idx = proc_pid_to_index(pid);
    if (idx < 0 || s_pcb_table[idx].state == FLUX_PROC_UNUSED) {
        proc_unlock();
        return FLUX_ERR_INVALID;
    }

    flux_pcb_t *pcb = &s_pcb_table[idx];

    flux_log("process '%s' (pid=%u) exited with code=%d, cpu_time=%llu",
             pcb->name, pid, code, (unsigned long long)pcb->cpu_time);

    /* Transition to ZOMBIE state first (for any waiting parent) */
    pcb->state = FLUX_PROC_ZOMBIE;

    /* Wake parent if it's waiting */
    if (pcb->parent_pid != FLUX_PID_INVALID) {
        int pidx = proc_pid_to_index(pcb->parent_pid);
        if (pidx >= 0 &&
            (s_pcb_table[pidx].state == FLUX_PROC_BLOCKED ||
             s_pcb_table[pidx].state == FLUX_PROC_AGENT_IDLE)) {
            if (s_pcb_table[pidx].waiting_for == pid) {
                s_pcb_table[pidx].state = FLUX_PROC_READY;
                s_pcb_table[pidx].waiting_for = FLUX_PID_INVALID;
            }
        }
    }

    /* Full cleanup */
    proc_cleanup(pcb);

    if (ks)
        ks->num_processes = s_active_count;

    proc_unlock();

    /* Trigger scheduler to pick next process */
    /* In a real kernel, this would trigger an immediate context switch */
    extern void flux_sched_yield_current(void);
    flux_sched_yield_current();

    /* Should never reach here */
    return FLUX_OK;
}

/*
 * flux_kill — Kill a process by PID.
 *
 * Parameters:
 *   pid — PID of the process to kill
 *
 * Returns:
 *   FLUX_OK on success, FLUX_ERR_INVALID if PID is invalid.
 */
flux_status_t flux_kill(flux_pid_t pid)
{
    if (pid == FLUX_PID_INVALID || pid == FLUX_PID_KERNEL)
        return FLUX_ERR_DENIED;

    proc_lock();

    int idx = proc_pid_to_index(pid);
    if (idx < 0 || s_pcb_table[idx].state == FLUX_PROC_UNUSED) {
        proc_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    flux_pcb_t *pcb = &s_pcb_table[idx];

    flux_log("killing process '%s' (pid=%u) state=%s",
             pcb->name, pid, proc_state_name(pcb->state));

    /* Clean up */
    proc_cleanup(pcb);

    /* Update kernel state */
    flux_kernel_state_t *ks = flux_kernel_state();
    if (ks)
        ks->num_processes = s_active_count;

    proc_unlock();

    return FLUX_OK;
}

/*
 * flux_yield — Voluntary context switch.
 *
 * The current process gives up its remaining time slice and is placed
 * back on the ready queue. The scheduler picks the next process.
 *
 * Returns:
 *   FLUX_OK on success.
 */
flux_status_t flux_yield(void)
{
    flux_kernel_state_t *ks = flux_kernel_state();
    if (!ks || ks->current_pid == FLUX_PID_INVALID)
        return FLUX_ERR_INVALID;

    proc_lock();

    int idx = proc_pid_to_index(ks->current_pid);
    if (idx < 0) {
        proc_unlock();
        return FLUX_ERR_INVALID;
    }

    /* Move to READY state */
    if (s_pcb_table[idx].state == FLUX_PROC_RUNNING) {
        s_pcb_table[idx].state = FLUX_PROC_READY;
    }

    proc_unlock();

    /* Enqueue for rescheduling */
    extern flux_status_t flux_sched_enqueue(flux_pid_t pid);
    flux_sched_enqueue(ks->current_pid);

    /* Trigger context switch */
    extern void flux_sched_yield_current(void);
    flux_sched_yield_current();

    return FLUX_OK;
}

/*
 * flux_getpid — Get the current process ID.
 */
flux_pid_t flux_getpid(void)
{
    flux_kernel_state_t *ks = flux_kernel_state();
    return ks ? ks->current_pid : FLUX_PID_INVALID;
}

/*
 * flux_get_pcb — Get a pointer to the PCB for a given PID.
 *
 * Parameters:
 *   pid — Process ID to look up
 *
 * Returns:
 *   Pointer to the PCB, or NULL if PID is invalid or unused.
 */
flux_pcb_t *flux_get_pcb(flux_pid_t pid)
{
    if (pid == FLUX_PID_INVALID || pid == 0)
        return NULL;

    int idx = proc_pid_to_index(pid);
    if (idx < 0)
        return NULL;

    if (s_pcb_table[idx].state == FLUX_PROC_UNUSED)
        return NULL;

    return &s_pcb_table[idx];
}

/*
 * flux_proc_count — Get the number of active processes.
 */
uint32_t flux_proc_count(void)
{
    return s_active_count;
}

/*
 * flux_agent_count — Get the number of active agent processes.
 */
int flux_agent_count(void)
{
    return (int)s_agent_count;
}

/*
 * flux_proc_set_state — Change a process's state.
 * Used by the scheduler and IPC subsystems.
 */
flux_status_t flux_proc_set_state(flux_pid_t pid, flux_proc_state_t new_state)
{
    if (pid == FLUX_PID_INVALID)
        return FLUX_ERR_INVALID;

    int idx = proc_pid_to_index(pid);
    if (idx < 0 || s_pcb_table[idx].state == FLUX_PROC_UNUSED)
        return FLUX_ERR_NOTFOUND;

    s_pcb_table[idx].state = new_state;
    return FLUX_OK;
}

/*
 * flux_proc_set_priority — Change a process's priority.
 * Higher values = higher priority (range 0-255).
 */
flux_status_t flux_proc_set_priority(flux_pid_t pid, uint8_t priority)
{
    if (pid == FLUX_PID_INVALID)
        return FLUX_ERR_INVALID;

    int idx = proc_pid_to_index(pid);
    if (idx < 0 || s_pcb_table[idx].state == FLUX_PROC_UNUSED)
        return FLUX_ERR_NOTFOUND;

    s_pcb_table[idx].priority = priority;
    return FLUX_OK;
}

/*
 * flux_proc_dump — Dump all process information for debugging.
 */
void flux_proc_dump(void)
{
    char buf[128];
    const flux_hal_t *hal = flux_hal_get();

    const char *header =
        "  PID   PPID  STATE            PRI  CPU_TIME  NAME\r\n"
        "  ───── ───── ──────────────── ──── ──────── ────────\r\n";

    if (hal && hal->console_puts)
        hal->console_puts(header);
    else
        fputs(header, stderr);

    for (int i = 0; i < FLUX_MAX_PROCESSES; i++) {
        if (s_pcb_table[i].state == FLUX_PROC_UNUSED)
            continue;

        flux_pcb_t *p = &s_pcb_table[i];
        snprintf(buf, sizeof(buf),
                 "  %-5u %-5u %-16s %3u  %8llu  %s%s\r\n",
                 p->pid, p->parent_pid,
                 proc_state_name(p->state),
                 p->priority,
                 (unsigned long long)p->cpu_time,
                 p->is_agent ? "[AGENT] " : "",
                 p->name);

        if (hal && hal->console_puts)
            hal->console_puts(buf);
        else
            fputs(buf, stderr);
    }
}

/*
 * flux_proc_init_registers — Initialize register state for a new process.
 * Sets up a minimal register context for process entry.
 */
void flux_proc_init_registers(flux_pcb_t *pcb, flux_addr_t entry,
                              flux_addr_t stack_top)
{
    if (!pcb)
        return;

    memset(pcb->regs, 0, sizeof(pcb->regs));
    pcb->regs[FLUX_REG_SP] = stack_top;     /* Stack pointer */
    pcb->regs[FLUX_REG_BP] = stack_top;     /* Base pointer */
    pcb->regs[FLUX_REG_PC] = entry;         /* Program counter */
    pcb->regs[FLUX_REG_RA] = 0;             /* Return address (none) */
    pcb->stack_ptr = stack_top;
    pcb->instr_ptr = entry;
}
