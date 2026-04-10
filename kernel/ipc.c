/*
 * FLUX OS — Inter-Process Communication (IPC) Subsystem
 *
 * Implements the A2A (Agent-to-Agent) messaging system that allows
 * processes — and especially agents — to communicate with each other.
 *
 * Design:
 *   - Per-process message queues (circular buffers, 64 messages max)
 *   - Synchronous and asynchronous send modes
 *   - Broadcast capability (send to all agents)
 *   - Capability checking on send (sender must have FLUX_CAP_COMMUNICATE)
 *   - Timeout-based receive with blocking support
 *   - Message sequence numbers for tracking
 *   - Priority support in messages
 *
 * Message Flow:
 *   sender → flux_a2a_send(target, msg, len) → receiver's queue
 *   receiver ← flux_a2a_recv(sender, buf, len, timeout) ← queue
 *
 * The kernel.h declares three IPC functions:
 *   flux_a2a_send()  — send to a specific target
 *   flux_a2a_recv()  — receive from any or specific sender
 *   flux_a2a_broadcast() — send to all agents
 */

#include "flux/kernel.h"
#include "flux/hal.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* Capability constants (from agent.h — replicated here to avoid header conflict) */
#ifndef FLUX_CAP_COMMUNICATE
#define FLUX_CAP_COMMUNICATE    0x0000000000000002ULL
#endif

/* ========================================================================
 * Configuration
 * ======================================================================== */

#ifndef FLUX_IPC_QUEUE_DEPTH
#define FLUX_IPC_QUEUE_DEPTH    64      /* Messages per process queue */
#endif

#ifndef FLUX_IPC_MAX_PAYLOAD
#define FLUX_IPC_MAX_PAYLOAD    4096    /* Max message payload size */
#endif

#ifndef FLUX_IPC_MAX_BROADCAST_QUEUE
#define FLUX_IPC_MAX_BROADCAST_QUEUE  256  /* Max pending broadcasts */
#endif

#ifndef FLUX_IPC_TIMEOUT_INFINITE
#define FLUX_IPC_TIMEOUT_INFINITE  0xFFFFFFFF  /* No timeout */
#endif

/* ========================================================================
 * Internal Message Structure
 *
 * This is the internal representation stored in per-process queues.
 * It's more compact than the public flux_a2a_msg_t.
 * ======================================================================== */

typedef struct {
    uint32_t      msg_id;         /* Unique message sequence number */
    flux_pid_t    sender;         /* Sender PID */
    flux_pid_t    target;         /* Target PID (0 for broadcast) */
    flux_cap_t    sender_caps;    /* Sender's capabilities (for validation) */
    flux_ticks_t  send_tick;      /* Tick when message was sent */
    uint32_t      payload_len;    /* Payload length in bytes */
    uint8_t       payload[FLUX_IPC_MAX_PAYLOAD];
} ipc_message_t;

/* ========================================================================
 * Per-Process Message Queue
 *
 * Each process has a circular buffer of messages.
 * ======================================================================== */

typedef struct {
    ipc_message_t messages[FLUX_IPC_QUEUE_DEPTH];
    int head;             /* Index of oldest message */
    int tail;             /* Index where next message goes */
    int count;            /* Number of pending messages */
    uint32_t next_id;     /* Next message sequence number */
    uint32_t total_sent;  /* Total messages sent from this process */
    uint32_t total_recv;  /* Total messages received by this process */
    uint32_t dropped;     /* Messages dropped (queue full) */
} ipc_queue_t;

/* ========================================================================
 * Pending Broadcast Entry
 * ======================================================================== */

typedef struct {
    ipc_message_t msg;
    uint32_t      delivered_count;  /* How many agents received it */
    bool          active;
} broadcast_entry_t;

/* ========================================================================
 * Static State
 * ======================================================================== */

/* Per-process message queues — indexed by PID (pid - 1) */
static ipc_queue_t s_queues[256];

/* Global message sequence counter */
static volatile uint32_t s_global_msg_id = 0;

/* Broadcast tracking */
static broadcast_entry_t s_broadcasts[FLUX_IPC_MAX_BROADCAST_QUEUE];
static int s_broadcast_count = 0;

/* IPC spinlock */
static volatile int s_ipc_lock = 0;

/* IPC statistics */
static struct {
    uint64_t  total_sent;
    uint64_t  total_recv;
    uint64_t  total_broadcasts;
    uint64_t  capability_denied;
    uint64_t  queue_full;
    uint64_t  timeout_expired;
} s_ipc_stats;

/* ========================================================================
 * Internal Helpers
 * ======================================================================== */

/*
 * ipc_lock / ipc_unlock — Spinlock for IPC operations.
 */
static inline void ipc_lock(void)
{
    while (__atomic_exchange_n(&s_ipc_lock, 1, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause" ::: "memory");
    }
}

static inline void ipc_unlock(void)
{
    __atomic_store_n(&s_ipc_lock, 0, __ATOMIC_RELEASE);
}

/*
 * ipc_get_queue — Get the message queue for a PID.
 * Returns NULL if PID is invalid.
 */
static ipc_queue_t *ipc_get_queue(flux_pid_t pid)
{
    if (pid == 0 || pid > 256)
        return NULL;

    return &s_queues[pid - 1];
}

/*
 * ipc_queue_is_full — Check if a process's message queue is full.
 */
static inline bool ipc_queue_is_full(const ipc_queue_t *q)
{
    return q->count >= FLUX_IPC_QUEUE_DEPTH;
}

/*
 * ipc_queue_is_empty — Check if a process's message queue is empty.
 */
static inline bool ipc_queue_is_empty(const ipc_queue_t *q)
{
    return q->count == 0;
}

/*
 * ipc_alloc_msg_id — Generate a unique message ID.
 */
static uint32_t ipc_alloc_msg_id(void)
{
    return __atomic_fetch_add(&s_global_msg_id, 1, __ATOMIC_RELAXED);
}

/*
 * ipc_current_tick — Get current kernel tick.
 */
static flux_ticks_t ipc_current_tick(void)
{
    flux_kernel_state_t *ks = flux_kernel_state();
    return ks ? (flux_ticks_t)ks->tick_count : 0;
}

/*
 * ipc_check_capability — Verify that a sender has the COMMUNICATE capability.
 */
static bool ipc_check_capability(flux_pid_t sender)
{
    flux_pcb_t *pcb = flux_get_pcb(sender);
    if (!pcb)
        return false;

    /* Check for FLUX_CAP_COMMUNICATE bit */
    return (pcb->capabilities & FLUX_CAP_COMMUNICATE) != 0;
}

/*
 * ipc_enqueue_message — Add a message to a process's queue.
 */
static flux_status_t ipc_enqueue_message(ipc_queue_t *q, const ipc_message_t *msg)
{
    if (ipc_queue_is_full(q)) {
        q->dropped++;
        return FLUX_ERR_OVERFLOW;
    }

    /* Copy message into queue */
    q->messages[q->tail] = *msg;
    q->tail = (q->tail + 1) % FLUX_IPC_QUEUE_DEPTH;
    q->count++;
    q->total_recv++;

    return FLUX_OK;
}

/*
 * ipc_dequeue_message — Remove the oldest message from a process's queue.
 */
static flux_status_t ipc_dequeue_message(ipc_queue_t *q, ipc_message_t *out)
{
    if (ipc_queue_is_empty(q))
        return FLUX_ERR_NOTFOUND;

    *out = q->messages[q->head];
    q->head = (q->head + 1) % FLUX_IPC_QUEUE_DEPTH;
    q->count--;

    return FLUX_OK;
}

/*
 * ipc_peek_message — Look at the oldest message without removing it.
 * Optionally filter by sender PID (0 = any sender).
 */
static const ipc_message_t *ipc_peek_message(const ipc_queue_t *q, flux_pid_t sender)
{
    if (ipc_queue_is_empty(q))
        return NULL;

    if (sender == FLUX_PID_INVALID) {
        /* No filter — return oldest */
        return &q->messages[q->head];
    }

    /* Search for message from specific sender */
    for (int i = 0; i < q->count; i++) {
        int idx = (q->head + i) % FLUX_IPC_QUEUE_DEPTH;
        if (q->messages[idx].sender == sender)
            return &q->messages[idx];
    }

    return NULL;  /* No message from specified sender */
}

/*
 * ipc_remove_message — Remove a specific message from the queue.
 * This is O(n) but necessary for filtered receives.
 */
static flux_status_t ipc_remove_message(ipc_queue_t *q, int msg_index)
{
    if (msg_index < 0 || msg_index >= q->count)
        return FLUX_ERR_INVALID;

    (void)0; /* idx used below in shift loop */

    /* Shift messages to fill the gap */
    for (int i = msg_index; i < q->count - 1; i++) {
        int curr = (q->head + i) % FLUX_IPC_QUEUE_DEPTH;
        int next = (q->head + i + 1) % FLUX_IPC_QUEUE_DEPTH;
        q->messages[curr] = q->messages[next];
    }

    q->count--;
    q->tail = (q->tail - 1 + FLUX_IPC_QUEUE_DEPTH) % FLUX_IPC_QUEUE_DEPTH;

    return FLUX_OK;
}

/* ========================================================================
 * IPC Initialization
 * ======================================================================== */

/*
 * flux_ipc_init — Initialize the IPC subsystem.
 * Clears all message queues and resets statistics.
 */
flux_status_t flux_ipc_init(void)
{
    ipc_lock();

    memset(s_queues, 0, sizeof(s_queues));
    memset(s_broadcasts, 0, sizeof(s_broadcasts));
    s_global_msg_id = 0;
    s_broadcast_count = 0;

    memset(&s_ipc_stats, 0, sizeof(s_ipc_stats));

    ipc_unlock();

    return FLUX_OK;
}

/* ========================================================================
 * Public API — kernel.h Interface
 * ======================================================================== */

/*
 * flux_a2a_send — Send a message to a specific process.
 *
 * This is the primary point-to-point messaging primitive.
 * The message is placed in the target's receive queue.
 *
 * Parameters:
 *   target — PID of the receiving process
 *   msg    — Pointer to the message payload
 *   len    — Length of the payload in bytes
 *
 * Returns:
 *   FLUX_OK on success
 *   FLUX_ERR_DENIED if sender lacks COMMUNICATE capability
 *   FLUX_ERR_NOTFOUND if target PID doesn't exist
 *   FLUX_ERR_OVERFLOW if target's queue is full
 */
flux_status_t flux_a2a_send(flux_pid_t target, const void *msg, flux_size_t len)
{
    if (!msg || len == 0 || len > FLUX_IPC_MAX_PAYLOAD)
        return FLUX_ERR_INVALID;

    if (target == FLUX_PID_INVALID || target == 0)
        return FLUX_ERR_INVALID;

    /* Get sender PID */
    flux_pid_t sender = flux_getpid();
    if (sender == FLUX_PID_INVALID)
        return FLUX_ERR_INVALID;

    /* Capability check */
    if (!ipc_check_capability(sender)) {
        s_ipc_stats.capability_denied++;
        return FLUX_ERR_DENIED;
    }

    ipc_lock();

    /* Verify target exists */
    flux_pcb_t *target_pcb = flux_get_pcb(target);
    if (!target_pcb) {
        ipc_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    /* Get target's queue */
    ipc_queue_t *tq = ipc_get_queue(target);
    if (!tq) {
        ipc_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    /* Check if target queue is full */
    if (ipc_queue_is_full(tq)) {
        s_ipc_stats.queue_full++;
        ipc_unlock();
        return FLUX_ERR_OVERFLOW;
    }

    /* Build the internal message */
    ipc_message_t imsg;
    memset(&imsg, 0, sizeof(imsg));
    imsg.msg_id = ipc_alloc_msg_id();
    imsg.sender = sender;
    imsg.target = target;
    imsg.sender_caps = flux_get_pcb(sender) ? flux_get_pcb(sender)->capabilities : 0;
    imsg.send_tick = ipc_current_tick();
    imsg.payload_len = (uint32_t)len;
    memcpy(imsg.payload, msg, len);

    /* Enqueue */
    flux_status_t status = ipc_enqueue_message(tq, &imsg);

    if (status == FLUX_OK) {
        s_ipc_stats.total_sent++;

        /* Update sender's queue stats */
        ipc_queue_t *sq = ipc_get_queue(sender);
        if (sq)
            sq->total_sent++;

        /* Wake target if it's blocked waiting for messages */
        if (target_pcb->state == FLUX_PROC_BLOCKED ||
            target_pcb->state == FLUX_PROC_AGENT_IDLE) {
            target_pcb->state = FLUX_PROC_READY;
            target_pcb->waiting_for = FLUX_PID_INVALID;

            /* Enqueue for scheduling */
            extern flux_status_t flux_sched_enqueue(flux_pid_t pid);
            flux_sched_enqueue(target);
        }
    }

    ipc_unlock();

    if (status == FLUX_OK) {
        flux_log_debug("IPC send: pid=%u -> pid=%u msg_id=%u len=%u",
                       sender, target, imsg.msg_id, len);
    }

    return status;
}

/*
 * flux_a2a_recv — Receive a message.
 *
 * Blocks the caller until a message is available or timeout expires.
 * If sender is FLUX_PID_INVALID, receives from any sender.
 *
 * Parameters:
 *   sender  — PID to receive from (FLUX_PID_INVALID = any)
 *   buf     — Buffer to receive the payload
 *   len     — [in/out] buffer size on input, actual payload size on output
 *   timeout — Maximum ticks to wait (0 = non-blocking, UINT32_MAX = infinite)
 *
 * Returns:
 *   FLUX_OK on success
 *   FLUX_ERR_TIMEOUT if no message arrived within timeout
 *   FLUX_ERR_INVALID if parameters are bad
 */
flux_status_t flux_a2a_recv(flux_pid_t *sender, void *buf, flux_size_t *len,
                            flux_ticks_t timeout)
{
    if (!buf || !len)
        return FLUX_ERR_INVALID;

    flux_pid_t my_pid = flux_getpid();
    if (my_pid == FLUX_PID_INVALID)
        return FLUX_ERR_INVALID;

    ipc_queue_t *q = ipc_get_queue(my_pid);
    if (!q)
        return FLUX_ERR_INVALID;

    flux_pid_t filter_sender = (sender && *sender != FLUX_PID_INVALID)
                                ? *sender : FLUX_PID_INVALID;

    ipc_lock();

    /* Try to find a matching message */
    const ipc_message_t *found = NULL;
    int found_index = -1;

    if (filter_sender != FLUX_PID_INVALID) {
        /* Search for message from specific sender */
        for (int i = 0; i < q->count; i++) {
            int idx = (q->head + i) % FLUX_IPC_QUEUE_DEPTH;
            if (q->messages[idx].sender == filter_sender) {
                found = &q->messages[idx];
                found_index = i;
                break;
            }
        }
    } else {
        /* Any sender — take oldest */
        if (q->count > 0) {
            found = &q->messages[q->head];
            found_index = 0;
        }
    }

    if (found) {
        /* Message found — copy to caller */
        flux_size_t copy_len = found->payload_len;
        if (copy_len > *len)
            copy_len = *len;

        memcpy(buf, found->payload, copy_len);
        *len = found->payload_len;

        /* Return sender PID */
        if (sender)
            *sender = found->sender;

        /* Remove from queue */
        ipc_remove_message(q, found_index);

        s_ipc_stats.total_recv++;

        ipc_unlock();

        flux_log_debug("IPC recv: pid=%u from=%u msg_id=%u len=%u",
                       my_pid, found->sender, found->msg_id, found->payload_len);

        return FLUX_OK;
    }

    /* No message available — handle timeout / blocking */
    if (timeout == 0) {
        /* Non-blocking: return immediately */
        ipc_unlock();
        return FLUX_ERR_TIMEOUT;
    }

    /* Block the current process if timeout > 0 */
    flux_pcb_t *my_pcb = flux_get_pcb(my_pid);
    if (my_pcb) {
        my_pcb->state = FLUX_PROC_BLOCKED;
        my_pcb->waiting_for = filter_sender;
    }

    ipc_unlock();

    /*
     * In a real kernel, we'd wait on a wait queue with timeout.
     * For simulation, we simulate waiting by counting ticks.
     * The scheduler will re-awaken this process when a message arrives
     * (see the wakeup code in flux_a2a_send above).
     */

    /* For blocking with timeout, we do a simple spin-wait.
     * This is acceptable for simulation but would be replaced with
     * proper sleep/wakeup in a real kernel. */
    flux_ticks_t start = ipc_current_tick();

    while (true) {
        /* Check if timeout expired */
        if (timeout != FLUX_IPC_TIMEOUT_INFINITE) {
            flux_ticks_t elapsed = ipc_current_tick() - start;
            if (elapsed >= timeout) {
                s_ipc_stats.timeout_expired++;

                /* Restore process state */
                if (my_pcb && (my_pcb->state == FLUX_PROC_BLOCKED)) {
                    my_pcb->state = FLUX_PROC_READY;
                    my_pcb->waiting_for = FLUX_PID_INVALID;
                    extern flux_status_t flux_sched_enqueue(flux_pid_t pid);
                    flux_sched_enqueue(my_pid);
                }
                return FLUX_ERR_TIMEOUT;
            }
        }

        /* Check queue again */
        ipc_lock();

        const ipc_message_t *msg = ipc_peek_message(q, filter_sender);
        if (msg) {
            /* Found a message */
            int idx = -1;
            for (int i = 0; i < q->count; i++) {
                int qi = (q->head + i) % FLUX_IPC_QUEUE_DEPTH;
                if (&q->messages[qi] == msg) {
                    idx = i;
                    break;
                }
            }

            if (idx >= 0) {
                flux_size_t copy_len = msg->payload_len;
                if (copy_len > *len)
                    copy_len = *len;

                memcpy(buf, msg->payload, copy_len);
                *len = msg->payload_len;

                if (sender)
                    *sender = msg->sender;

                ipc_remove_message(q, idx);
                s_ipc_stats.total_recv++;

                /* Restore state */
                if (my_pcb) {
                    my_pcb->state = FLUX_PROC_RUNNING;
                    my_pcb->waiting_for = FLUX_PID_INVALID;
                }

                ipc_unlock();

                flux_log_debug("IPC recv (waited): pid=%u from=%u msg_id=%u",
                               my_pid, msg->sender, msg->msg_id);
                return FLUX_OK;
            }
        }

        ipc_unlock();
    }
}

/*
 * flux_a2a_broadcast — Send a message to all agents.
 *
 * Iterates over all processes and delivers the message to every
 * process that is an agent (is_agent == true).
 *
 * Parameters:
 *   msg — Pointer to the message payload
 *   len — Length of the payload in bytes
 *
 * Returns:
 *   FLUX_OK on success (even if some deliveries failed)
 *   Number of successful deliveries is tracked internally.
 */
flux_status_t flux_a2a_broadcast(const void *msg, flux_size_t len)
{
    if (!msg || len == 0 || len > FLUX_IPC_MAX_PAYLOAD)
        return FLUX_ERR_INVALID;

    flux_pid_t sender = flux_getpid();
    if (sender == FLUX_PID_INVALID)
        return FLUX_ERR_INVALID;

    /* Capability check */
    if (!ipc_check_capability(sender)) {
        s_ipc_stats.capability_denied++;
        return FLUX_ERR_DENIED;
    }

    ipc_lock();

    /* Build the broadcast message */
    ipc_message_t bmsg;
    memset(&bmsg, 0, sizeof(bmsg));
    bmsg.msg_id = ipc_alloc_msg_id();
    bmsg.sender = sender;
    bmsg.target = 0;  /* Broadcast — no specific target */
    bmsg.sender_caps = flux_get_pcb(sender) ? flux_get_pcb(sender)->capabilities : 0;
    bmsg.send_tick = ipc_current_tick();
    bmsg.payload_len = (uint32_t)len;
    memcpy(bmsg.payload, msg, len);

    /* Deliver to all agents */
    int delivered = 0;
    int failed = 0;

    for (uint32_t pid = FLUX_PID_FIRST_APP; pid <= 256; pid++) {
        flux_pcb_t *pcb = flux_get_pcb((flux_pid_t)pid);
        if (!pcb)
            continue;

        /* Only deliver to agents */
        if (!pcb->is_agent)
            continue;

        /* Don't send to self */
        if ((flux_pid_t)pid == sender)
            continue;

        ipc_queue_t *q = ipc_get_queue((flux_pid_t)pid);
        if (!q)
            continue;

        if (ipc_queue_is_full(q)) {
            q->dropped++;
            failed++;
            continue;
        }

        ipc_enqueue_message(q, &bmsg);
        delivered++;

        /* Wake agent if idle */
        if (pcb->state == FLUX_PROC_AGENT_IDLE) {
            pcb->state = FLUX_PROC_READY;
            extern flux_status_t flux_sched_enqueue(flux_pid_t epid);
            flux_sched_enqueue((flux_pid_t)pid);
        }
    }

    s_ipc_stats.total_broadcasts++;

    /* Track broadcast */
    if (s_broadcast_count < FLUX_IPC_MAX_BROADCAST_QUEUE) {
        broadcast_entry_t *be = &s_broadcasts[s_broadcast_count];
        be->msg = bmsg;
        be->delivered_count = (uint32_t)delivered;
        be->active = true;
        s_broadcast_count++;
    }

    ipc_unlock();

    flux_log("IPC broadcast: from=%u msg_id=%u len=%u delivered=%d failed=%d",
             sender, bmsg.msg_id, len, delivered, failed);

    return FLUX_OK;
}

/* ========================================================================
 * Extended IPC API
 * ======================================================================== */

/*
 * flux_ipc_stats — Get IPC statistics.
 */
const char *flux_ipc_stats(void)
{
    static char buf[256];

    snprintf(buf, sizeof(buf),
             "IPC: sent=%llu recv=%llu broadcasts=%llu denied=%llu "
             "queue_full=%llu timeouts=%llu",
             (unsigned long long)s_ipc_stats.total_sent,
             (unsigned long long)s_ipc_stats.total_recv,
             (unsigned long long)s_ipc_stats.total_broadcasts,
             (unsigned long long)s_ipc_stats.capability_denied,
             (unsigned long long)s_ipc_stats.queue_full,
             (unsigned long long)s_ipc_stats.timeout_expired);

    return buf;
}

/*
 * flux_ipc_queue_status — Get status of a process's message queue.
 */
flux_status_t flux_ipc_queue_status(flux_pid_t pid, uint32_t *pending,
                                    uint32_t *total_sent, uint32_t *total_recv,
                                    uint32_t *dropped)
{
    ipc_queue_t *q = ipc_get_queue(pid);
    if (!q)
        return FLUX_ERR_NOTFOUND;

    if (pending)    *pending = (uint32_t)q->count;
    if (total_sent) *total_sent = q->total_sent;
    if (total_recv) *total_recv = q->total_recv;
    if (dropped)    *dropped = q->dropped;

    return FLUX_OK;
}
