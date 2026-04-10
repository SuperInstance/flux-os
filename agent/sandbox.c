/*
 * FLUX OS — Sandboxed Execution
 *
 * Implements the execution engine for running agents in sandboxed VM
 * environments. Each agent runs in its own VM instance with isolated
 * memory regions, cycle limits, and I/O restrictions.
 *
 * Design:
 *   - Each agent has its own flux_vm_t instance
 *   - Execution is cycle-limited to prevent runaway agents
 *   - A2A operations (TELL/ASK/DELEGATE) cause the VM to yield
 *   - Resource accounting tracks CPU ticks, memory, and I/O per agent
 *   - Sandbox violations (memory access out of bounds, exceeding limits)
 *     are detected and result in agent termination
 *   - Round-robin scheduler runs all ready agents in turn
 *
 * Execution States:
 *   CREATED → INITIALIZING → IDLE ↔ THINKING → WAITING
 *                                              ↓
 *                                        DELEGATING → WAITING
 *                                              ↓
 *                                        TERMINATED / FAILED
 */

#include "agent_internal.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ========================================================================
 * Sandbox Violation Codes
 * ======================================================================== */

#define SANDBOX_VIOLATION_NONE          0
#define SANDBOX_VIOLATION_CPU_LIMIT     1
#define SANDBOX_VIOLATION_MEMORY_LIMIT  2
#define SANDBOX_VIOLATION_IO_LIMIT      3
#define SANDBOX_VIOLATION_MEMORY_ACCESS 4
#define SANDBOX_VIOLATION_CAPABILITY    5
#define SANDBOX_VIOLATION_INVALID_STATE 6

/* ========================================================================
 * A2A Opcode Detection
 * ======================================================================== */

/*
 * is_a2a_opcode — Check if an opcode is an A2A operation that should
 * cause the VM to yield control.
 */
static bool is_a2a_opcode(uint8_t opcode)
{
    switch (opcode) {
        case OP_TELL:
        case OP_ASK:
        case OP_REPLY:
        case OP_DELEGATE:
        case OP_BARRIER:
        case OP_YIELD_AGENT:
            return true;
        default:
            return false;
    }
}

/*
 * is_io_opcode — Check if an opcode performs I/O.
 */
static bool is_io_opcode(uint8_t opcode)
{
    switch (opcode) {
        case OP_IO_READ:
        case OP_IO_WRITE:
        case OP_IO_READ8:
        case OP_IO_WRITE8:
        case OP_IO_READ16:
        case OP_IO_WRITE16:
        case OP_IO_READ32:
        case OP_IO_WRITE32:
            return true;
        default:
            return false;
    }
}

/* ========================================================================
 * Resource Accounting
 * ======================================================================== */

/*
 * sandbox_check_cpu — Check if an agent has exceeded its CPU tick limit.
 *
 * Returns:
 *   true if limit exceeded (violation), false if OK.
 */
static bool sandbox_check_cpu(flux_agent_slot_t *slot)
{
    if (slot->desc.limits.max_cpu_ticks == 0)
        return false; /* No limit */

    if (slot->desc.limits.used_cpu_ticks >= slot->desc.limits.max_cpu_ticks) {
        slot->desc.state = FLUX_AGENT_FAILED;
        return true;
    }
    return false;
}

/*
 * sandbox_check_memory — Check if an agent has exceeded its memory limit.
 *
 * This is a simplified check — in a real implementation, the VM would
 * track per-agent memory allocations. Here we check the VM's active
 * memory regions.
 *
 * Returns:
 *   true if limit exceeded (violation), false if OK.
 */
static bool sandbox_check_memory(flux_agent_slot_t *slot)
{
    if (slot->desc.limits.max_memory == 0)
        return false; /* No limit */

    if (!slot->vm_initialized)
        return false;

    /* Sum up all active region sizes */
    flux_size_t total = 0;
    for (int i = 0; i < FLUX_REGION_MAX; i++) {
        if (slot->vm.regions[i].active) {
            total += slot->vm.regions[i].size;
        }
    }

    slot->desc.limits.used_memory = total;

    if (total > slot->desc.limits.max_memory) {
        slot->desc.state = FLUX_AGENT_FAILED;
        return true;
    }
    return false;
}

/*
 * sandbox_check_io — Track and check I/O operations per agent.
 *
 * Returns:
 *   true if limit exceeded (violation), false if OK.
 */
static bool sandbox_check_io(flux_agent_slot_t *slot)
{
    if (slot->desc.limits.max_io_per_sec == 0)
        return false; /* No limit */

    if (slot->desc.limits.used_io_per_sec >= slot->desc.limits.max_io_per_sec) {
        return true;
    }
    return false;
}

/*
 * sandbox_account_io — Increment the I/O counter for an agent.
 */
static void sandbox_account_io(flux_agent_slot_t *slot)
{
    slot->desc.limits.used_io_per_sec++;
}

/* ========================================================================
 * Sandbox Violation Handler
 * ======================================================================== */

/*
 * sandbox_handle_violation — Handle a sandbox violation.
 *
 * Terminates the agent and logs the violation reason.
 */
static void sandbox_handle_violation(flux_agent_slot_t *slot, int violation)
{
    const char *reasons[] = {
        "none",
        "CPU tick limit exceeded",
        "memory limit exceeded",
        "I/O rate limit exceeded",
        "invalid memory access",
        "insufficient capability",
        "invalid agent state",
    };

    const char *reason = (violation >= 0 && violation <= 6)
                         ? reasons[violation]
                         : "unknown";

    slot->desc.state = FLUX_AGENT_FAILED;

    /* Halt the VM */
    if (slot->vm_initialized) {
        slot->vm.state = FLUX_VM_ERROR;
        slot->vm.error_code = (uint8_t)violation;
    }

    /* Log the violation */
    printf("[SANDBOX] Agent '%s' (id=%u) violated: %s "
           "(cpu=%llu/%llu, mem=%llu/%llu, io=%u/%u)\n",
           slot->desc.name,
           slot->desc.agent_id,
           reason,
           (unsigned long long)slot->desc.limits.used_cpu_ticks,
           (unsigned long long)slot->desc.limits.max_cpu_ticks,
           (unsigned long long)slot->desc.limits.used_memory,
           (unsigned long long)slot->desc.limits.max_memory,
           slot->desc.limits.used_io_per_sec,
           slot->desc.limits.max_io_per_sec);
}

/* ========================================================================
 * VM Step with Sandbox Monitoring
 * ======================================================================== */

/*
 * sandbox_step — Execute one VM step with sandbox monitoring.
 *
 * Checks resource limits before and after execution, detects A2A
 * operations that should cause yield, and handles violations.
 *
 * Parameters:
 *   slot — Agent slot (must be locked externally)
 *
 * Returns:
 *   FLUX_OK on successful step
 *   FLUX_ERR_GENERAL on VM halt/completion
 *   FLUX_ERR_INVALID on sandbox violation
 *   FLUX_ERR_BUSY on A2A yield
 */
static flux_status_t sandbox_step(flux_agent_slot_t *slot)
{
    if (!slot->vm_initialized) {
        return FLUX_ERR_INVALID;
    }

    /* Pre-step resource checks */
    if (sandbox_check_cpu(slot)) {
        sandbox_handle_violation(slot, SANDBOX_VIOLATION_CPU_LIMIT);
        return FLUX_ERR_INVALID;
    }

    if (sandbox_check_memory(slot)) {
        sandbox_handle_violation(slot, SANDBOX_VIOLATION_MEMORY_LIMIT);
        return FLUX_ERR_INVALID;
    }

    /* Ensure VM is in a runnable state */
    if (slot->vm.state != FLUX_VM_IDLE && slot->vm.state != FLUX_VM_RUNNING) {
        if (slot->vm.state == FLUX_VM_HALTED) {
            slot->desc.state = FLUX_AGENT_TERMINATED;
            return FLUX_ERR_GENERAL;
        }
        return FLUX_ERR_INVALID;
    }

    /* Peek at next opcode for pre-checks */
    if (slot->vm.pc < slot->vm.bytecode_len) {
        uint8_t next_op = slot->vm.bytecode[slot->vm.pc];

        /* Check I/O capability */
        if (is_io_opcode(next_op)) {
            if (!(slot->desc.capabilities & FLUX_CAP_IO_READ) &&
                !(slot->desc.capabilities & FLUX_CAP_IO_WRITE)) {
                sandbox_handle_violation(slot, SANDBOX_VIOLATION_CAPABILITY);
                return FLUX_ERR_DENIED;
            }
            if (sandbox_check_io(slot)) {
                sandbox_handle_violation(slot, SANDBOX_VIOLATION_IO_LIMIT);
                return FLUX_ERR_DENIED;
            }
            sandbox_account_io(slot);
        }

        /* Yield on A2A operations */
        if (is_a2a_opcode(next_op)) {
            /* Execute the A2A opcode (one step) */
            flux_status_t step_status = flux_vm_step(&slot->vm);
            slot->desc.limits.used_cpu_ticks += slot->vm.last_op_cycles;

            if (step_status == FLUX_OK) {
                /* Yield — agent will process the A2A operation and
                 * wait for response if needed */
                slot->desc.state = FLUX_AGENT_WAITING;
                return FLUX_ERR_BUSY; /* Signal: yield for A2A */
            }
            return step_status;
        }
    }

    /* Execute one VM step */
    flux_status_t step_status = flux_vm_step(&slot->vm);

    /* Account CPU cycles */
    slot->desc.limits.used_cpu_ticks += slot->vm.last_op_cycles;
    if (slot->vm.last_op_cycles == 0)
        slot->desc.limits.used_cpu_ticks++; /* At least 1 tick per step */

    /* Update tick counter */
    slot->last_tick_update = agent_now();

    /* Handle step result */
    switch (step_status) {
        case FLUX_OK:
            /* Successful step — continue */
            slot->desc.state = FLUX_AGENT_THINKING;
            break;

        case FLUX_ERR_GENERAL:
            /* VM halted — agent completed */
            slot->desc.state = FLUX_AGENT_TERMINATED;
            return FLUX_ERR_GENERAL;

        default:
            /* VM error */
            slot->desc.state = FLUX_AGENT_FAILED;
            slot->vm.state = FLUX_VM_ERROR;
            return FLUX_ERR_GENERAL;
    }

    return FLUX_OK;
}

/* ========================================================================
 * Public API — Agent Execution
 * ======================================================================== */

/*
 * flux_agent_run — Run an agent in its sandbox for up to max_ticks.
 *
 * Executes the agent's bytecode in the VM with a cycle limit.
 * Monitors for A2A operations (yields on TELL/ASK/DELEGATE),
 * enforces resource limits, and handles violations.
 *
 * Parameters:
 *   agent_id  — Agent to run
 *   max_ticks — Maximum CPU ticks to execute (0 = use agent's limit)
 *
 * Returns:
 *   FLUX_OK on normal completion (agent halted)
 *   FLUX_ERR_BUSY on A2A yield (agent waiting for response)
 *   FLUX_ERR_INVALID on sandbox violation or invalid state
 */
flux_status_t flux_agent_run(uint32_t agent_id, uint32_t max_ticks)
{
    if (!g_agent_rt.initialized)
        return FLUX_ERR_INVALID;

    agent_lock_real();

    flux_agent_slot_t *slot = agent_slot_by_id(agent_id);
    if (!slot) {
        agent_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    /* Check valid state for execution */
    if (slot->desc.state == FLUX_AGENT_TERMINATED ||
        slot->desc.state == FLUX_AGENT_FAILED) {
        agent_unlock();
        return FLUX_ERR_INVALID;
    }

    /* Check if paused */
    if (slot->paused) {
        agent_unlock();
        return FLUX_ERR_INVALID;
    }

    /* Set effective tick limit */
    uint64_t tick_limit = max_ticks > 0
                          ? (uint64_t)max_ticks
                          : slot->desc.limits.max_cpu_ticks;
    uint64_t start_ticks = slot->desc.limits.used_cpu_ticks;

    /* Transition to THINKING state */
    if (slot->desc.state == FLUX_AGENT_CREATED ||
        slot->desc.state == FLUX_AGENT_IDLE) {
        slot->desc.state = FLUX_AGENT_INITIALIZING;
        /* One-step transition from INITIALIZING to THINKING */
    }

    slot->desc.state = FLUX_AGENT_THINKING;
    slot->desc.last_active = agent_now();

    if (slot->vm_initialized && slot->vm.state == FLUX_VM_IDLE) {
        slot->vm.state = FLUX_VM_RUNNING;
    }

    /* Main execution loop */
    flux_status_t result = FLUX_OK;
    uint64_t steps = 0;

    while (1) {
        /* Check tick limit */
        if (tick_limit > 0 &&
            (slot->desc.limits.used_cpu_ticks - start_ticks) >= tick_limit) {
            /* Tick budget exhausted — yield */
            slot->desc.state = FLUX_AGENT_IDLE;
            result = FLUX_ERR_BUSY;
            break;
        }

        /* Check hard CPU limit */
        if (sandbox_check_cpu(slot)) {
            sandbox_handle_violation(slot, SANDBOX_VIOLATION_CPU_LIMIT);
            result = FLUX_ERR_INVALID;
            break;
        }

        /* Check memory */
        if (sandbox_check_memory(slot)) {
            sandbox_handle_violation(slot, SANDBOX_VIOLATION_MEMORY_LIMIT);
            result = FLUX_ERR_INVALID;
            break;
        }

        /* Execute one step */
        result = sandbox_step(slot);
        steps++;

        if (result != FLUX_OK) {
            /* Step returned error, halt, or yield */
            break;
        }

        /* Prevent infinite single-run — if steps exceed a reasonable
         * number without yielding, force a yield */
        if (steps >= 10000000 && max_ticks == 0) {
            slot->desc.state = FLUX_AGENT_IDLE;
            result = FLUX_ERR_BUSY;
            break;
        }
    }

    /* Halt VM if we're done */
    if (result == FLUX_OK || slot->desc.state == FLUX_AGENT_TERMINATED) {
        if (slot->vm_initialized) {
            slot->vm.state = FLUX_VM_HALTED;
        }
    }

    agent_unlock();

    return result;
}

/*
 * flux_agent_step — Execute a single VM step for an agent.
 *
 * Parameters:
 *   agent_id — Agent to step
 *
 * Returns:
 *   FLUX_OK on successful step, error code on failure.
 */
flux_status_t flux_agent_step(uint32_t agent_id)
{
    if (!g_agent_rt.initialized)
        return FLUX_ERR_INVALID;

    agent_lock_real();

    flux_agent_slot_t *slot = agent_slot_by_id(agent_id);
    if (!slot) {
        agent_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    /* Check state */
    if (slot->desc.state == FLUX_AGENT_TERMINATED ||
        slot->desc.state == FLUX_AGENT_FAILED) {
        agent_unlock();
        return FLUX_ERR_INVALID;
    }

    if (slot->paused) {
        agent_unlock();
        return FLUX_ERR_INVALID;
    }

    /* Ensure VM is initialized */
    if (!slot->vm_initialized || !slot->vm.bytecode) {
        agent_unlock();
        return FLUX_ERR_INVALID;
    }

    /* Set VM to running if idle */
    if (slot->vm.state == FLUX_VM_IDLE) {
        slot->vm.state = FLUX_VM_RUNNING;
    }

    slot->desc.last_active = agent_now();

    /* Execute one sandboxed step */
    flux_status_t result = sandbox_step(slot);

    agent_unlock();

    return result;
}

/*
 * flux_agent_schedule — Run all ready agents in round-robin fashion.
 *
 * Iterates through all active agents, running each one that is in a
 * ready state (IDLE or THINKING) for a single time slice. Each agent
 * gets up to the configured default CPU ticks per round.
 *
 * Returns:
 *   FLUX_OK on success (always succeeds, even if no agents ran).
 */
flux_status_t flux_agent_schedule(void)
{
    if (!g_agent_rt.initialized)
        return FLUX_ERR_INVALID;

    int rounds = 0;
    int max_rounds = FLUX_AGENT_MAX * 2; /* Prevent infinite scheduling */
    int agents_run = 0;

    agent_lock_real();

    while (rounds < max_rounds) {
        bool found_ready = false;

        for (int i = 0; i < FLUX_AGENT_MAX; i++) {
            int idx = (g_agent_rt.schedule_cursor + i) % FLUX_AGENT_MAX;
            flux_agent_slot_t *slot = &g_agent_rt.agents[idx];

            if (!slot->active)
                continue;

            /* Skip non-runnable states */
            if (slot->desc.state == FLUX_AGENT_TERMINATED ||
                slot->desc.state == FLUX_AGENT_FAILED ||
                slot->desc.state == FLUX_AGENT_WAITING ||
                slot->desc.state == FLUX_AGENT_DELEGATING ||
                slot->paused) {
                continue;
            }

            /* Agent is ready to run */
            found_ready = true;
            agents_run++;

            /* Release lock during execution to allow A2A */
            agent_unlock();

            /* Give agent a time slice */
            uint32_t slice = g_agent_rt.config.default_cpu_ticks / 10;
            if (slice < 100) slice = 100;
            if (slice > 100000) slice = 100000;

            flux_agent_run(slot->desc.agent_id, slice);

            agent_lock_real();
        }

        /* Advance cursor for next round */
        g_agent_rt.schedule_cursor = (g_agent_rt.schedule_cursor + 1) % FLUX_AGENT_MAX;

        rounds++;

        if (!found_ready)
            break;
    }

    agent_unlock();

    /* Advance global tick counter */
    g_agent_rt.tick_counter++;

    return FLUX_OK;
}

/*
 * flux_agent_schedule_step — Run one step of each ready agent.
 *
 * Lighter-weight version of flux_agent_schedule() that only executes
 * a single VM step per agent. Useful for fine-grained control.
 *
 * Returns:
 *   Number of agents that were stepped.
 */
int flux_agent_schedule_step(void)
{
    if (!g_agent_rt.initialized)
        return 0;

    int stepped = 0;

    for (int i = 0; i < FLUX_AGENT_MAX; i++) {
        flux_agent_slot_t *slot = &g_agent_rt.agents[i];
        if (!slot->active)
            continue;

        if (slot->desc.state != FLUX_AGENT_IDLE &&
            slot->desc.state != FLUX_AGENT_THINKING &&
            slot->desc.state != FLUX_AGENT_CREATED &&
            slot->desc.state != FLUX_AGENT_INITIALIZING)
            continue;

        if (slot->paused)
            continue;

        flux_agent_step(slot->desc.agent_id);
        stepped++;
    }

    g_agent_rt.tick_counter++;

    return stepped;
}

/*
 * flux_agent_get_vm — Get the VM instance for an agent.
 *
 * This is a low-level API for debugging and inspection.
 *
 * Parameters:
 *   agent_id — Agent ID
 *
 * Returns:
 *   Pointer to the agent's VM instance, or NULL.
 */
flux_vm_t *flux_agent_get_vm(uint32_t agent_id)
{
    if (!g_agent_rt.initialized)
        return NULL;

    agent_lock_real();

    flux_agent_slot_t *slot = agent_slot_by_id(agent_id);
    if (!slot || !slot->vm_initialized) {
        agent_unlock();
        return NULL;
    }

    agent_unlock();

    return &slot->vm;
}

/*
 * flux_agent_resource_report — Get resource usage report for an agent.
 *
 * Parameters:
 *   agent_id — Agent ID
 *   cpu_pct  — Output: CPU usage percentage (0-100)
 *   mem_pct  — Output: Memory usage percentage (0-100)
 *   io_pct   — Output: I/O usage percentage (0-100)
 *
 * Returns:
 *   FLUX_OK on success, error code on failure.
 */
flux_status_t flux_agent_resource_report(uint32_t agent_id,
                                         int *cpu_pct, int *mem_pct,
                                         int *io_pct)
{
    if (!g_agent_rt.initialized)
        return FLUX_ERR_INVALID;

    agent_lock_real();

    flux_agent_slot_t *slot = agent_slot_by_id(agent_id);
    if (!slot) {
        agent_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    if (cpu_pct) {
        if (slot->desc.limits.max_cpu_ticks > 0) {
            *cpu_pct = (int)((slot->desc.limits.used_cpu_ticks * 100) /
                             slot->desc.limits.max_cpu_ticks);
            if (*cpu_pct > 100) *cpu_pct = 100;
        } else {
            *cpu_pct = 0;
        }
    }

    if (mem_pct) {
        if (slot->desc.limits.max_memory > 0) {
            *mem_pct = (int)((slot->desc.limits.used_memory * 100) /
                            slot->desc.limits.max_memory);
            if (*mem_pct > 100) *mem_pct = 100;
        } else {
            *mem_pct = 0;
        }
    }

    if (io_pct) {
        if (slot->desc.limits.max_io_per_sec > 0) {
            *io_pct = (int)((slot->desc.limits.used_io_per_sec * 100) /
                           slot->desc.limits.max_io_per_sec);
            if (*io_pct > 100) *io_pct = 100;
        } else {
            *io_pct = 0;
        }
    }

    agent_unlock();

    return FLUX_OK;
}
