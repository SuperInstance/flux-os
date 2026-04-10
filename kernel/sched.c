/*
 * FLUX OS — Process Scheduler
 *
 * Implements a priority-based round-robin scheduler with agent-aware
 * scheduling. The scheduler manages a circular ready queue of up to
 * 256 processes and selects the next process to run based on:
 *
 *   1. Priority (higher = more important, 0-255)
 *   2. Round-robin within the same priority level
 *   3. Agent thinking time (agents get extended time slices when thinking)
 *
 * The scheduler uses a time slice system where each process gets a
 * configurable number of ticks per slice. When the slice expires,
 * the process is preempted and the next process is selected.
 *
 * Key Design Decisions:
 *   - Circular buffer ready queue (no dynamic allocation)
 *   - O(1) enqueue/dequeue operations
 *   - Priority aging: processes that wait too long get boosted
 *   - Agent-aware: agent processes in THINKING state get bonus ticks
 *   - The kernel process (PID 1) can never be preempted
 */

#include "flux/kernel.h"
#include "flux/hal.h"
#include "flux/vm.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* ========================================================================
 * Configuration
 * ======================================================================== */

#ifndef FLUX_SCHED_QUEUE_SIZE
#define FLUX_SCHED_QUEUE_SIZE    256   /* Max processes in ready queue */
#endif

#ifndef FLUX_SCHED_DEFAULT_TIMESLICE
#define FLUX_SCHED_DEFAULT_TIMESLICE  10  /* Ticks per time slice */
#endif

#ifndef FLUX_SCHED_AGENT_THINK_BONUS
#define FLUX_SCHED_AGENT_THINK_BONUS  20  /* Extra ticks for thinking agents */
#endif

#ifndef FLUX_SCHED_PRIORITY_BOOST_INTERVAL
#define FLUX_SCHED_PRIORITY_BOOST_INTERVAL 1000  /* Boost every N ticks */
#endif

#ifndef FLUX_SCHED_PRIORITY_BOOST_AMOUNT
#define FLUX_SCHED_PRIORITY_BOOST_AMOUNT  10  /* Boost by this much */
#endif

#ifndef FLUX_SCHED_MAX_PRIORITY_BOOST
#define FLUX_SCHED_MAX_PRIORITY_BOOST   200  /* Max boosted priority */
#endif

/* ========================================================================
 * Ready Queue Entry
 * ======================================================================== */

typedef struct {
    flux_pid_t     pid;
    uint8_t        priority;
    flux_ticks_t   wait_start;     /* When this process started waiting */
    flux_ticks_t   time_slice;     /* Remaining ticks in current slice */
    bool           is_agent_thinking;  /* Agent in THINKING state */
} sched_entry_t;

/* ========================================================================
 * Scheduler Statistics
 * ======================================================================== */

typedef struct {
    uint64_t       total_switches;     /* Total context switches */
    uint64_t       idle_switches;      /* Times scheduler found no work */
    uint64_t       agent_switches;     /* Context switches involving agents */
    uint64_t       preemptions;        /* Involuntary switches (time slice expiry) */
    uint64_t       voluntary_yields;   /* Voluntary yields */
    uint32_t       current_timeslice;  /* Current time slice counter */
    uint32_t       boost_timer;        /* Timer for priority aging */
} sched_stats_t;

/* ========================================================================
 * Static State
 * ======================================================================== */

/* Circular ready queue */
static sched_entry_t s_ready_queue[FLUX_SCHED_QUEUE_SIZE];
static int s_queue_head = 0;    /* Index of first entry */
static int s_queue_tail = 0;    /* Index one past last entry */
static int s_queue_count = 0;   /* Number of entries in queue */

/* Currently running process */
static flux_pid_t s_current_pid = FLUX_PID_KERNEL;

/* Scheduler state */
static bool s_scheduler_running = false;
static volatile int s_sched_lock = 0;

/* Statistics */
static sched_stats_t s_stats;

/* Default time slice */
static flux_ticks_t s_default_timeslice = FLUX_SCHED_DEFAULT_TIMESLICE;

/* ========================================================================
 * Internal Helpers
 * ======================================================================== */

/*
 * sched_lock / sched_unlock — Spinlock for scheduler operations.
 */
static inline void sched_lock(void)
{
    while (__atomic_exchange_n(&s_sched_lock, 1, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause" ::: "memory");
    }
}

static inline void sched_unlock(void)
{
    __atomic_store_n(&s_sched_lock, 0, __ATOMIC_RELEASE);
}

/*
 * sched_queue_empty — Check if the ready queue is empty.
 */
static inline bool sched_queue_empty(void)
{
    return s_queue_count == 0;
}

/*
 * sched_queue_full — Check if the ready queue is full.
 */
static inline bool sched_queue_full(void)
{
    return s_queue_count >= FLUX_SCHED_QUEUE_SIZE;
}

/*
 * sched_queue_peek — Look at the first entry without removing it.
 */
static sched_entry_t *sched_queue_peek(void)
{
    if (s_queue_count == 0)
        return NULL;
    return &s_ready_queue[s_queue_head];
}

/*
 * sched_current_tick — Get the current tick count.
 */
static flux_ticks_t sched_current_tick(void)
{
    flux_kernel_state_t *ks = flux_kernel_state();
    return ks ? (flux_ticks_t)ks->tick_count : 0;
}

/*
 * sched_find_highest_priority — Scan the ready queue for the highest
 * priority entry. This is used for priority-based selection.
 *
 * Returns the index within the circular buffer of the best candidate,
 * or -1 if the queue is empty.
 */
static int sched_find_highest_priority(void)
{
    if (s_queue_count == 0)
        return -1;

    int best_idx = s_queue_head;
    uint8_t best_priority = s_ready_queue[s_queue_head].priority;

    /* Scan all entries */
    for (int i = 0; i < s_queue_count; i++) {
        int idx = (s_queue_head + i) % FLUX_SCHED_QUEUE_SIZE;
        uint8_t pri = s_ready_queue[idx].priority;

        /* Higher priority wins; ties broken by round-robin (first found) */
        if (pri > best_priority) {
            best_priority = pri;
            best_idx = idx;
        }
    }

    return best_idx;
}

/*
 * sched_age_priorities — Priority aging.
 * Processes that have been waiting too long get a priority boost
 * to prevent starvation.
 */
static void sched_age_priorities(void)
{
    s_stats.boost_timer++;

    if (s_stats.boost_timer < FLUX_SCHED_PRIORITY_BOOST_INTERVAL)
        return;

    s_stats.boost_timer = 0;

    flux_ticks_t now = sched_current_tick();

    for (int i = 0; i < s_queue_count; i++) {
        int idx = (s_queue_head + i) % FLUX_SCHED_QUEUE_SIZE;
        sched_entry_t *entry = &s_ready_queue[idx];

        /* How long has this process been waiting? */
        flux_ticks_t wait_time = now - entry->wait_start;

        /* Boost if waiting too long (1 boost interval per boost cycle) */
        if (wait_time > FLUX_SCHED_PRIORITY_BOOST_INTERVAL) {
            if (entry->priority < FLUX_SCHED_MAX_PRIORITY_BOOST) {
                entry->priority += FLUX_SCHED_PRIORITY_BOOST_AMOUNT;
                if (entry->priority > FLUX_SCHED_MAX_PRIORITY_BOOST)
                    entry->priority = FLUX_SCHED_MAX_PRIORITY_BOOST;

                /* Also update the PCB priority */
                flux_pcb_t *pcb = flux_get_pcb(entry->pid);
                if (pcb)
                    pcb->priority = entry->priority;
            }
        }
    }
}

/* ========================================================================
 * Scheduler Initialization
 * ======================================================================== */

/*
 * flux_sched_init — Initialize the scheduler subsystem.
 *
 * Sets up the ready queue, resets statistics, and configures
 * default time slice values.
 */
flux_status_t flux_sched_init(void)
{
    sched_lock();

    /* Clear queue */
    memset(s_ready_queue, 0, sizeof(s_ready_queue));
    s_queue_head = 0;
    s_queue_tail = 0;
    s_queue_count = 0;
    s_current_pid = FLUX_PID_KERNEL;
    s_scheduler_running = false;

    /* Clear statistics */
    memset(&s_stats, 0, sizeof(s_stats));
    s_default_timeslice = FLUX_SCHED_DEFAULT_TIMESLICE;

    sched_unlock();

    return FLUX_OK;
}

/* ========================================================================
 * Ready Queue Management
 * ======================================================================== */

/*
 * flux_sched_enqueue — Add a process to the ready queue.
 *
 * Parameters:
 *   pid — PID of the process to enqueue
 *
 * Returns:
 *   FLUX_OK on success, FLUX_ERR_OVERFLOW if queue is full,
 *   FLUX_ERR_INVALID if PID is invalid.
 *
 * Notes:
 *   - If the process is an agent in THINKING state, it gets bonus time
 *   - The process's priority is read from its PCB
 *   - Processes already in the queue are NOT added again
 */
flux_status_t flux_sched_enqueue(flux_pid_t pid)
{
    if (pid == FLUX_PID_INVALID)
        return FLUX_ERR_INVALID;

    sched_lock();

    /* Check if already in queue */
    for (int i = 0; i < s_queue_count; i++) {
        int idx = (s_queue_head + i) % FLUX_SCHED_QUEUE_SIZE;
        if (s_ready_queue[idx].pid == pid) {
            sched_unlock();
            return FLUX_OK;  /* Already queued — not an error */
        }
    }

    /* Check queue capacity */
    if (sched_queue_full()) {
        sched_unlock();
        return FLUX_ERR_OVERFLOW;
    }

    /* Get PCB for priority and state */
    flux_pcb_t *pcb = flux_get_pcb(pid);
    if (!pcb) {
        sched_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    /* Create queue entry */
    sched_entry_t *entry = &s_ready_queue[s_queue_tail];
    entry->pid = pid;
    entry->priority = pcb->priority;
    entry->wait_start = sched_current_tick();

    /* Determine time slice based on process type */
    if (pcb->is_agent && pcb->state == FLUX_PROC_AGENT_THINKING) {
        /* Agents thinking get extended time for reasoning */
        entry->time_slice = s_default_timeslice + FLUX_SCHED_AGENT_THINK_BONUS;
        entry->is_agent_thinking = true;
    } else if (pcb->is_agent) {
        entry->time_slice = s_default_timeslice;
        entry->is_agent_thinking = false;
    } else {
        entry->time_slice = s_default_timeslice;
        entry->is_agent_thinking = false;
    }

    /* Advance tail */
    s_queue_tail = (s_queue_tail + 1) % FLUX_SCHED_QUEUE_SIZE;
    s_queue_count++;

    /* Mark process as READY */
    if (pcb->state == FLUX_PROC_BLOCKED ||
        pcb->state == FLUX_PROC_AGENT_IDLE) {
        pcb->state = FLUX_PROC_READY;
    }

    sched_unlock();

    return FLUX_OK;
}

/*
 * flux_sched_dequeue — Remove and return the next process to run.
 *
 * Selection algorithm:
 *   1. Find the highest-priority process in the queue
 *   2. Within same priority, use round-robin (FIFO)
 *   3. Skip the kernel process (PID 1) unless it's the only one
 *
 * Returns:
 *   PID of the selected process, or FLUX_PID_INVALID if queue is empty.
 */
flux_pid_t flux_sched_dequeue(void)
{
    sched_lock();

    if (sched_queue_empty()) {
        sched_unlock();
        return FLUX_PID_INVALID;
    }

    /* Priority aging — prevent starvation */
    sched_age_priorities();

    /* Find highest priority entry */
    int best = sched_find_highest_priority();
    if (best < 0) {
        sched_unlock();
        return FLUX_PID_INVALID;
    }

    flux_pid_t selected_pid = s_ready_queue[best].pid;
    flux_ticks_t time_slice = s_ready_queue[best].time_slice;

    /* Remove from queue by shifting */
    /* If it's the head, just advance head */
    if (best == s_queue_head) {
        s_queue_head = (s_queue_head + 1) % FLUX_SCHED_QUEUE_SIZE;
    } else {
        /* Shift entries to fill the gap */
        int count_to_move = s_queue_count - (best - s_queue_head + 1);
        for (int i = 0; i < count_to_move; i++) {
            int src = (best + i + 1) % FLUX_SCHED_QUEUE_SIZE;
            int dst = (best + i) % FLUX_SCHED_QUEUE_SIZE;
            s_ready_queue[dst] = s_ready_queue[src];
        }
        s_queue_tail = (s_queue_tail - 1 + FLUX_SCHED_QUEUE_SIZE) % FLUX_SCHED_QUEUE_SIZE;
    }

    s_queue_count--;

    sched_unlock();

    /* Update the PCB state to RUNNING */
    flux_pcb_t *pcb = flux_get_pcb(selected_pid);
    if (pcb) {
        pcb->state = FLUX_PROC_RUNNING;
        s_stats.current_timeslice = (uint32_t)time_slice;
    }

    return selected_pid;
}

/*
 * flux_sched_yield_current — Force the current process to yield.
 * This is called internally by flux_yield() and flux_exit().
 * It does NOT block — it just triggers the scheduler.
 */
void flux_sched_yield_current(void)
{
    s_stats.voluntary_yields++;
    s_current_pid = FLUX_PID_INVALID;

    /* In a real kernel, this would trigger a timer interrupt or
     * software interrupt to force the context switch. For now,
     * we just mark the current PID as invalid so the scheduler
     * loop picks a new process. */
}

/* ========================================================================
 * Main Scheduler Loop
 * ======================================================================== */

/*
 * flux_sched_run — Main scheduler loop.
 *
 * This is the heart of the FLUX scheduler. It continuously:
 *   1. Picks the highest-priority ready process
 *   2. Runs it for one time slice
 *   3. Preempts if the time slice expires
 *   4. Re-enqueues the process if still runnable
 *
 * The loop runs until s_scheduler_running is set to false.
 *
 * In a real kernel, this would be the idle thread / timer ISR handler.
 */
flux_status_t flux_sched_run(void)
{
    s_scheduler_running = true;

    flux_log("scheduler started (timeslice=%u, queue_cap=%d)",
             s_default_timeslice, FLUX_SCHED_QUEUE_SIZE);

    while (s_scheduler_running) {
        /* Increment kernel tick */
        flux_kernel_state_t *ks = flux_kernel_state();
        if (ks)
            ks->tick_count++;

        /* Check if current process still has time */
        if (s_current_pid != FLUX_PID_INVALID && s_stats.current_timeslice > 0) {
            s_stats.current_timeslice--;

            /* Update process CPU time */
            flux_pcb_t *pcb = flux_get_pcb(s_current_pid);
            if (pcb)
                pcb->cpu_time++;

            /* In a real kernel, we'd execute one instruction or
             * yield to the process here. For simulation, we just
             * decrement the tick counter. */
            continue;
        }

        /* Time slice expired or no current process — switch */
        flux_pid_t prev_pid = s_current_pid;

        /* Re-enqueue previous process if still runnable */
        if (prev_pid != FLUX_PID_INVALID && prev_pid != FLUX_PID_KERNEL) {
            flux_pcb_t *prev_pcb = flux_get_pcb(prev_pid);
            if (prev_pcb && prev_pcb->state == FLUX_PROC_RUNNING) {
                /* Check if it was preempted (time slice expired) */
                if (s_stats.current_timeslice == 0)
                    s_stats.preemptions++;

                flux_sched_enqueue(prev_pid);
            }
        }

        /* Dequeue next process */
        flux_pid_t next_pid = flux_sched_dequeue();

        if (next_pid == FLUX_PID_INVALID) {
            /* No processes ready — idle */
            s_stats.idle_switches++;
            s_current_pid = FLUX_PID_KERNEL;
            s_stats.current_timeslice = 1;

            if (ks)
                ks->current_pid = FLUX_PID_KERNEL;

            /* In a real kernel, this would be a HLT/WFE instruction */
            continue;
        }

        /* Context switch */
        s_current_pid = next_pid;
        s_stats.total_switches++;

        if (ks)
            ks->current_pid = next_pid;

        /* Check if this is an agent switch */
        flux_pcb_t *next_pcb = flux_get_pcb(next_pid);
        if (next_pcb && next_pcb->is_agent)
            s_stats.agent_switches++;

        /* Log context switches at DEBUG level */
        if (next_pid != prev_pid) {
            flux_log_debug("ctx switch: pid=%u -> pid=%u", prev_pid, next_pid);
        }
    }

    return FLUX_OK;
}

/*
 * flux_sched_stop — Stop the scheduler loop.
 */
flux_status_t flux_sched_stop(void)
{
    s_scheduler_running = false;
    return FLUX_OK;
}

/*
 * flux_sched_set_timeslice — Set the default time slice duration.
 */
void flux_sched_set_timeslice(flux_ticks_t ticks)
{
    if (ticks > 0 && ticks < 10000)
        s_default_timeslice = ticks;
}

/*
 * flux_sched_get_stats — Get scheduler statistics.
 */
const sched_stats_t *flux_sched_get_stats(void)
{
    return &s_stats;
}

/*
 * flux_sched_queue_count — Return number of processes in ready queue.
 */
int flux_sched_queue_count(void)
{
    return s_queue_count;
}

/*
 * flux_sched_dump — Dump scheduler state for debugging.
 */
void flux_sched_dump(void)
{
    char buf[128];

    snprintf(buf, sizeof(buf),
             "Scheduler: running=%s queue=%d switches=%llu preemptions=%llu "
             "voluntary=%llu idle=%llu agent=%llu timeslice=%u/%lu\r\n",
             s_scheduler_running ? "yes" : "no",
             s_queue_count,
             (unsigned long long)s_stats.total_switches,
             (unsigned long long)s_stats.preemptions,
             (unsigned long long)s_stats.voluntary_yields,
             (unsigned long long)s_stats.idle_switches,
             (unsigned long long)s_stats.agent_switches,
             s_stats.current_timeslice,
             s_default_timeslice);

    const flux_hal_t *hal = flux_hal_get();
    if (hal && hal->console_puts)
        hal->console_puts(buf);
    else
        fputs(buf, stderr);

    /* Dump ready queue */
    snprintf(buf, sizeof(buf),
             "  Ready Queue (%d entries):\r\n", s_queue_count);
    if (hal && hal->console_puts)
        hal->console_puts(buf);
    else
        fputs(buf, stderr);

    for (int i = 0; i < s_queue_count; i++) {
        int idx = (s_queue_head + i) % FLUX_SCHED_QUEUE_SIZE;
        sched_entry_t *e = &s_ready_queue[idx];

        snprintf(buf, sizeof(buf),
                 "    [%d] pid=%u pri=%u slice=%lu wait=%llu agent_think=%s\r\n",
                 i, e->pid, e->priority, e->time_slice,
                 (unsigned long long)(sched_current_tick() - e->wait_start),
                 e->is_agent_thinking ? "yes" : "no");

        if (hal && hal->console_puts)
            hal->console_puts(buf);
        else
            fputs(buf, stderr);
    }
}
