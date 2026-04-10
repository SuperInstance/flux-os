/*
 * FLUX OS — Agent-to-Agent (A2A) Protocol
 *
 * Implements the message passing system for inter-agent communication.
 * Supports direct messaging, broadcast, reply, and delegation patterns.
 *
 * Design:
 *   - Pre-allocated message pool for zero-allocation fast path
 *   - Per-agent circular inbox buffer (lock-free read/write indices)
 *   - Topic-based broadcast with subscriber tables
 *   - Unique message IDs for correlation and reply tracking
 *   - Delegation tracking for result correlation
 *
 * Message Flow:
 *   SEND:  sender → target.inbox  (direct message)
 *   BCAST: sender → all subscribers to topic
 *   REPLY: receiver → sender.inbox  (with reply_to reference)
 *   DELEGATE: delegator → worker.inbox  (with delegation tracking)
 */

#include "agent_internal.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ========================================================================
 * Message Pool
 * ======================================================================== */

/*
 * msg_pool_alloc — Allocate a message from the pool.
 *
 * Scans the message pool bitmap for a free slot. If all slots are
 * in use, falls back to heap allocation.
 *
 * Returns:
 *   Pointer to an allocated message (zeroed), or NULL on OOM.
 */
flux_a2a_msg_t *msg_pool_alloc(void)
{
    /* Try pool allocation first */
    for (int i = 0; i < FLUX_MSG_POOL_SIZE; i++) {
        int word = i / 64;
        int bit  = i % 64;
        if (!(g_agent_rt.msg_pool.allocated[word] & ((uint64_t)1 << bit))) {
            g_agent_rt.msg_pool.allocated[word] |= ((uint64_t)1 << bit);
            memset(&g_agent_rt.msg_pool.messages[i], 0, sizeof(flux_a2a_msg_t));
            return &g_agent_rt.msg_pool.messages[i];
        }
    }

    /* Pool exhausted — heap allocate */
    flux_a2a_msg_t *msg = (flux_a2a_msg_t *)calloc(1, sizeof(flux_a2a_msg_t));
    return msg;
}

/*
 * msg_pool_free — Return a message to the pool.
 *
 * If the message belongs to the pool (address check), clear it and
 * mark the slot as free. Otherwise, free() the heap allocation.
 */
void msg_pool_free(flux_a2a_msg_t *msg)
{
    if (!msg)
        return;

    /* Check if this message is in our pool */
    flux_addr_t msg_addr = (flux_addr_t)msg;
    flux_addr_t pool_start = (flux_addr_t)g_agent_rt.msg_pool.messages;
    flux_addr_t pool_end = pool_start + sizeof(g_agent_rt.msg_pool.messages);

    if (msg_addr >= pool_start && msg_addr < pool_end) {
        /* Belongs to pool — clear and free slot */
        int idx = (int)((msg_addr - pool_start) / sizeof(flux_a2a_msg_t));
        if (idx >= 0 && idx < FLUX_MSG_POOL_SIZE) {
            int word = idx / 64;
            int bit  = idx % 64;
            memset(msg, 0, sizeof(flux_a2a_msg_t));
            g_agent_rt.msg_pool.allocated[word] &= ~((uint64_t)1 << bit);
            g_agent_rt.msg_pool.freed++;
        }
    } else {
        /* Heap allocated */
        free(msg);
    }
}

/*
 * msg_generate_id — Generate a unique message ID.
 * Wraps around at UINT32_MAX; 0 is reserved as invalid.
 */
static uint32_t msg_generate_id(void)
{
    uint32_t id = __atomic_fetch_add(&g_agent_rt.next_msg_id, 1, __ATOMIC_RELAXED);
    if (id == 0)
        id = __atomic_fetch_add(&g_agent_rt.next_msg_id, 1, __ATOMIC_RELAXED);
    return id;
}

/* ========================================================================
 * Inbox Operations
 * ======================================================================== */

/*
 * inbox_enqueue — Add a message to an agent's inbox.
 *
 * Parameters:
 *   slot — Agent slot (must be locked externally)
 *   msg  — Message to enqueue (ownership transferred to inbox)
 *
 * Returns:
 *   FLUX_OK on success, FLUX_ERR_OVERFLOW if inbox is full.
 */
static flux_status_t inbox_enqueue(flux_agent_slot_t *slot, flux_a2a_msg_t *msg)
{
    if (slot->inbox_count >= FLUX_INBOX_SIZE)
        return FLUX_ERR_OVERFLOW;

    slot->inbox[slot->inbox_tail] = msg;
    slot->inbox_tail = (slot->inbox_tail + 1) % FLUX_INBOX_SIZE;
    slot->inbox_count++;
    slot->inbox_total++;

    return FLUX_OK;
}

/*
 * inbox_dequeue — Remove a message from an agent's inbox.
 *
 * Parameters:
 *   slot — Agent slot (must be locked externally)
 *   out  — Output: pointer to dequeued message
 *
 * Returns:
 *   FLUX_OK on success, FLUX_ERR_TIMEOUT if inbox is empty.
 */
static flux_status_t inbox_dequeue(flux_agent_slot_t *slot, flux_a2a_msg_t **out)
{
    if (slot->inbox_count <= 0)
        return FLUX_ERR_TIMEOUT;

    *out = slot->inbox[slot->inbox_head];
    slot->inbox[slot->inbox_head] = NULL;
    slot->inbox_head = (slot->inbox_head + 1) % FLUX_INBOX_SIZE;
    slot->inbox_count--;

    return FLUX_OK;
}

/*
 * inbox_peek — Check if a message is available without dequeuing.
 */
static bool inbox_peek(flux_agent_slot_t *slot)
{
    return slot->inbox_count > 0;
}

/* ========================================================================
 * Public API — Direct Messaging
 * ======================================================================== */

/*
 * flux_a2a_send — Send a message from one agent to another.
 *
 * Validates sender and target existence, checks COMMUNICATE capability,
 * creates a message with unique ID, and enqueues in target's inbox.
 *
 * Parameters:
 *   sender  — Agent ID sending the message
 *   target  — Agent ID receiving the message
 *   type    — Message type (TELL, ASK, etc.)
 *   payload — Message payload data
 *   len     — Payload length in bytes (max FLUX_A2A_MAX_PAYLOAD)
 *   msg_id  — Output: generated message ID (can be NULL)
 *
 * Returns:
 *   FLUX_OK on success, error code on failure.
 */
flux_status_t flux_a2a_send(uint32_t sender, uint32_t target,
                             flux_a2a_type_t type, const void *payload,
                             uint32_t len, uint32_t *msg_id)
{
    if (!g_agent_rt.initialized)
        return FLUX_ERR_INVALID;

    /* Validate payload size */
    if (len > FLUX_A2A_MAX_PAYLOAD)
        return FLUX_ERR_OVERFLOW;

    agent_lock_real();

    /* Validate sender exists */
    flux_agent_slot_t *sender_slot = agent_slot_by_id(sender);
    if (!sender_slot) {
        agent_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    /* Validate target exists */
    flux_agent_slot_t *target_slot = agent_slot_by_id(target);
    if (!target_slot) {
        agent_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    /* Check sender has COMMUNICATE capability */
    if (!(sender_slot->desc.capabilities & FLUX_CAP_COMMUNICATE)) {
        agent_unlock();
        return FLUX_ERR_DENIED;
    }

    /* Can't send to self */
    if (sender == target) {
        agent_unlock();
        return FLUX_ERR_INVALID;
    }

    /* Allocate message */
    flux_a2a_msg_t *msg = msg_pool_alloc();
    if (!msg) {
        agent_unlock();
        return FLUX_ERR_NOMEM;
    }

    /* Fill in message fields */
    msg->msg_id = msg_generate_id();
    msg->reply_to = 0;
    msg->type = type;
    msg->sender = sender;
    msg->target = target;
    msg->required_cap = FLUX_CAP_NONE;
    msg->timestamp = agent_now();
    msg->deadline = 0;
    msg->priority = 0;
    msg->topic[0] = '\0';
    msg->payload_len = len;

    if (payload && len > 0) {
        memcpy(msg->payload, payload, len);
    }

    /* Enqueue in target's inbox */
    flux_status_t status = inbox_enqueue(target_slot, msg);
    if (status != FLUX_OK) {
        msg_pool_free(msg);
        agent_unlock();
        return status;
    }

    /* Update counters */
    sender_slot->desc.msg_count_sent++;
    target_slot->desc.msg_count_recv++;
    target_slot->desc.last_active = agent_now();
    sender_slot->desc.last_active = agent_now();
    g_agent_rt.total_messages_sent++;
    g_agent_rt.total_messages_recv++;

    /* Wake target if it was waiting */
    if (target_slot->desc.state == FLUX_AGENT_WAITING) {
        target_slot->desc.state = FLUX_AGENT_IDLE;
    }

    /* Output message ID */
    if (msg_id)
        *msg_id = msg->msg_id;

    agent_unlock();

    return FLUX_OK;
}

/*
 * flux_a2a_recv — Receive a message from an agent's inbox.
 *
 * Dequeues the next message. If the inbox is empty and timeout_ms > 0,
 * this function will spin-wait for the specified duration.
 *
 * Parameters:
 *   agent_id   — Agent receiving the message
 *   msg        — Output: received message (copied from inbox)
 *   timeout_ms — Max wait time in milliseconds (0 = non-blocking)
 *
 * Returns:
 *   FLUX_OK on success, FLUX_ERR_TIMEOUT if no message available.
 */
flux_status_t flux_a2a_recv(uint32_t agent_id, flux_a2a_msg_t *msg,
                             uint32_t timeout_ms)
{
    if (!g_agent_rt.initialized || !msg)
        return FLUX_ERR_INVALID;

    agent_lock_real();

    flux_agent_slot_t *slot = agent_slot_by_id(agent_id);
    if (!slot) {
        agent_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    /* Try to dequeue immediately */
    flux_a2a_msg_t *received = NULL;
    flux_status_t status = inbox_dequeue(slot, &received);

    if (status == FLUX_OK && received) {
        /* Copy message to output buffer */
        memcpy(msg, received, sizeof(flux_a2a_msg_t));
        msg_pool_free(received);

        slot->desc.last_active = agent_now();
        agent_unlock();
        return FLUX_OK;
    }

    /* No message available — check timeout */
    if (timeout_ms == 0) {
        agent_unlock();
        return FLUX_ERR_TIMEOUT;
    }

    /* Spin-wait with timeout (simple implementation) */
    uint64_t start = agent_now();
    uint64_t deadline_ticks = (uint64_t)timeout_ms; /* Approx: 1 tick = 1 ms */

    agent_unlock();

    while (1) {
        /* Small busy-wait delay */
        for (volatile int d = 0; d < 1000; d++) {}

        agent_lock_real();

        slot = agent_slot_by_id(agent_id);
        if (!slot) {
            agent_unlock();
            return FLUX_ERR_NOTFOUND;
        }

        status = inbox_dequeue(slot, &received);
        if (status == FLUX_OK && received) {
            memcpy(msg, received, sizeof(flux_a2a_msg_t));
            msg_pool_free(received);
            slot->desc.last_active = agent_now();
            agent_unlock();
            return FLUX_OK;
        }

        uint64_t elapsed = agent_now() - start;
        agent_unlock();

        if (timeout_ms > 0 && elapsed >= deadline_ticks) {
            return FLUX_ERR_TIMEOUT;
        }

        /* Mark agent as waiting */
        agent_lock_real();
        slot = agent_slot_by_id(agent_id);
        if (slot && slot->desc.state != FLUX_AGENT_TERMINATED &&
            slot->desc.state != FLUX_AGENT_FAILED) {
            slot->desc.state = FLUX_AGENT_WAITING;
        }
        agent_unlock();
    }
}

/* ========================================================================
 * Public API — Broadcast
 * ======================================================================== */

/*
 * flux_a2a_broadcast — Broadcast a message to all subscribers of a topic.
 *
 * Sends a copy of the message to each subscriber's inbox. The sender
 * must have COMMUNICATE capability.
 *
 * Parameters:
 *   sender  — Broadcasting agent ID
 *   topic   — Topic string (max FLUX_A2A_MAX_TOPIC - 1 chars)
 *   payload — Broadcast payload data
 *   len     — Payload length
 *
 * Returns:
 *   FLUX_OK on success, error code on failure.
 */
flux_status_t flux_a2a_broadcast(uint32_t sender, const char *topic,
                                  const void *payload, uint32_t len)
{
    if (!g_agent_rt.initialized)
        return FLUX_ERR_INVALID;

    if (!topic || topic[0] == '\0')
        return FLUX_ERR_INVALID;

    if (len > FLUX_A2A_MAX_PAYLOAD)
        return FLUX_ERR_OVERFLOW;

    agent_lock_real();

    /* Validate sender */
    flux_agent_slot_t *sender_slot = agent_slot_by_id(sender);
    if (!sender_slot) {
        agent_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    /* Check COMMUNICATE capability */
    if (!(sender_slot->desc.capabilities & FLUX_CAP_COMMUNICATE)) {
        agent_unlock();
        return FLUX_ERR_DENIED;
    }

    /* Find the topic */
    flux_topic_t *tp = NULL;
    for (int i = 0; i < g_agent_rt.num_topics; i++) {
        if (strcmp(g_agent_rt.topics[i].name, topic) == 0) {
            tp = &g_agent_rt.topics[i];
            break;
        }
    }

    /* No subscribers — not an error, just no-op */
    if (!tp || tp->num_subscribers == 0) {
        agent_unlock();
        return FLUX_OK;
    }

    /* Send to each subscriber */
    int sent_count = 0;
    for (int i = 0; i < tp->num_subscribers; i++) {
        uint32_t sub_id = tp->subscribers[i];

        /* Don't send back to sender */
        if (sub_id == sender)
            continue;

        flux_agent_slot_t *target = agent_slot_by_id(sub_id);
        if (!target)
            continue;

        /* Allocate message */
        flux_a2a_msg_t *msg = msg_pool_alloc();
        if (!msg)
            continue; /* Best effort */

        /* Fill message */
        msg->msg_id = msg_generate_id();
        msg->reply_to = 0;
        msg->type = FLUX_A2A_EVENT;
        msg->sender = sender;
        msg->target = sub_id;
        msg->required_cap = FLUX_CAP_NONE;
        msg->timestamp = agent_now();
        msg->deadline = 0;
        msg->priority = 1; /* Broadcasts get slightly higher priority */
        msg->payload_len = len;

        /* Copy topic */
        int t;
        for (t = 0; t < FLUX_A2A_MAX_TOPIC - 1 && topic[t]; t++)
            msg->topic[t] = topic[t];
        msg->topic[t] = '\0';

        /* Copy payload */
        if (payload && len > 0)
            memcpy(msg->payload, payload, len);

        /* Enqueue */
        if (inbox_enqueue(target, msg) == FLUX_OK) {
            target->desc.msg_count_recv++;
            target->desc.last_active = agent_now();
            g_agent_rt.total_messages_recv++;

            /* Wake if waiting */
            if (target->desc.state == FLUX_AGENT_WAITING)
                target->desc.state = FLUX_AGENT_IDLE;

            sent_count++;
        } else {
            msg_pool_free(msg);
        }
    }

    /* Update sender stats */
    sender_slot->desc.msg_count_sent += sent_count;
    sender_slot->desc.last_active = agent_now();
    g_agent_rt.total_messages_sent += sent_count;
    g_agent_rt.total_broadcasts++;

    agent_unlock();

    return FLUX_OK;
}

/* ========================================================================
 * Public API — Reply
 * ======================================================================== */

/*
 * flux_a2a_reply — Send a reply to a previously received message.
 *
 * Creates a REPLY message referencing the original's msg_id.
 * The reply is sent to the original sender.
 *
 * Parameters:
 *   original — The original message being replied to
 *   payload  — Reply payload data
 *   len      — Payload length
 *
 * Returns:
 *   FLUX_OK on success, error code on failure.
 */
flux_status_t flux_a2a_reply(const flux_a2a_msg_t *original,
                              const void *payload, uint32_t len)
{
    if (!g_agent_rt.initialized || !original)
        return FLUX_ERR_INVALID;

    if (len > FLUX_A2A_MAX_PAYLOAD)
        return FLUX_ERR_OVERFLOW;

    agent_lock_real();

    /* The replier is the "target" of the original message */
    uint32_t sender = original->target;
    uint32_t target = original->sender;

    /* Validate sender (the agent replying) */
    flux_agent_slot_t *sender_slot = agent_slot_by_id(sender);
    if (!sender_slot) {
        agent_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    /* Check COMMUNICATE capability */
    if (!(sender_slot->desc.capabilities & FLUX_CAP_COMMUNICATE)) {
        agent_unlock();
        return FLUX_ERR_DENIED;
    }

    /* Validate target (original sender) */
    flux_agent_slot_t *target_slot = agent_slot_by_id(target);
    if (!target_slot) {
        agent_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    /* Allocate reply message */
    flux_a2a_msg_t *msg = msg_pool_alloc();
    if (!msg) {
        agent_unlock();
        return FLUX_ERR_NOMEM;
    }

    /* Fill reply */
    msg->msg_id = msg_generate_id();
    msg->reply_to = original->msg_id;
    msg->type = FLUX_A2A_REPLY;
    msg->sender = sender;
    msg->target = target;
    msg->required_cap = FLUX_CAP_NONE;
    msg->timestamp = agent_now();
    msg->deadline = original->deadline;
    msg->priority = original->priority;
    msg->payload_len = len;

    /* Preserve topic from original */
    memcpy(msg->topic, original->topic, FLUX_A2A_MAX_TOPIC);

    /* Copy payload */
    if (payload && len > 0)
        memcpy(msg->payload, payload, len);

    /* Enqueue */
    flux_status_t status = inbox_enqueue(target_slot, msg);
    if (status != FLUX_OK) {
        msg_pool_free(msg);
        agent_unlock();
        return status;
    }

    sender_slot->desc.msg_count_sent++;
    target_slot->desc.msg_count_recv++;
    target_slot->desc.last_active = agent_now();
    sender_slot->desc.last_active = agent_now();
    g_agent_rt.total_messages_sent++;
    g_agent_rt.total_messages_recv++;

    /* Wake target if waiting for reply */
    if (target_slot->desc.state == FLUX_AGENT_WAITING) {
        target_slot->desc.state = FLUX_AGENT_IDLE;
    }

    agent_unlock();

    return FLUX_OK;
}

/* ========================================================================
 * Public API — Delegation
 * ======================================================================== */

/*
 * flux_a2a_delegate — Delegate a task from one agent to another.
 *
 * Creates a DELEGATE message with the task payload and tracks
 * the delegation for later result correlation.
 *
 * Parameters:
 *   from         — Delegating agent ID
 *   to           — Worker agent ID
 *   task         — Task payload data
 *   task_len     — Task payload length
 *   delegate_id  — Output: delegation tracking ID
 *
 * Returns:
 *   FLUX_OK on success, error code on failure.
 */
flux_status_t flux_a2a_delegate(uint32_t from, uint32_t to,
                                 const void *task, uint32_t task_len,
                                 uint32_t *delegate_id)
{
    if (!g_agent_rt.initialized)
        return FLUX_ERR_INVALID;

    if (task_len > FLUX_A2A_MAX_PAYLOAD)
        return FLUX_ERR_OVERFLOW;

    agent_lock_real();

    /* Validate delegator */
    flux_agent_slot_t *from_slot = agent_slot_by_id(from);
    if (!from_slot) {
        agent_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    /* Check COMMUNICATE capability */
    if (!(from_slot->desc.capabilities & FLUX_CAP_COMMUNICATE)) {
        agent_unlock();
        return FLUX_ERR_DENIED;
    }

    /* Validate worker */
    flux_agent_slot_t *to_slot = agent_slot_by_id(to);
    if (!to_slot) {
        agent_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    /* Can't delegate to self */
    if (from == to) {
        agent_unlock();
        return FLUX_ERR_INVALID;
    }

    /* Check delegation limit */
    if (from_slot->num_delegations >= 16) {
        agent_unlock();
        return FLUX_ERR_OVERFLOW;
    }

    /* Allocate delegate message */
    flux_a2a_msg_t *msg = msg_pool_alloc();
    if (!msg) {
        agent_unlock();
        return FLUX_ERR_NOMEM;
    }

    /* Fill delegate message */
    msg->msg_id = msg_generate_id();
    msg->reply_to = 0;
    msg->type = FLUX_A2A_DELEGATE;
    msg->sender = from;
    msg->target = to;
    msg->required_cap = FLUX_CAP_NONE;
    msg->timestamp = agent_now();
    msg->deadline = 0;
    msg->priority = 2; /* Delegations get higher priority */
    msg->payload_len = task_len;

    if (task && task_len > 0)
        memcpy(msg->payload, task, task_len);

    /* Enqueue in worker's inbox */
    flux_status_t status = inbox_enqueue(to_slot, msg);
    if (status != FLUX_OK) {
        msg_pool_free(msg);
        agent_unlock();
        return status;
    }

    /* Track delegation in sender */
    int del_idx = from_slot->num_delegations;
    from_slot->delegations[del_idx].original_msg_id = msg->msg_id;
    from_slot->delegations[del_idx].target_agent = to;
    from_slot->delegations[del_idx].active = true;
    from_slot->num_delegations++;

    /* Update counters */
    from_slot->desc.msg_count_sent++;
    from_slot->desc.delegate_count++;
    to_slot->desc.msg_count_recv++;
    to_slot->desc.last_active = agent_now();
    from_slot->desc.last_active = agent_now();
    from_slot->desc.state = FLUX_AGENT_DELEGATING;
    g_agent_rt.total_messages_sent++;
    g_agent_rt.total_messages_recv++;
    g_agent_rt.total_delegations++;

    /* Wake worker */
    if (to_slot->desc.state == FLUX_AGENT_WAITING) {
        to_slot->desc.state = FLUX_AGENT_IDLE;
    }

    /* Output delegate ID (same as message ID for correlation) */
    if (delegate_id)
        *delegate_id = msg->msg_id;

    agent_unlock();

    return FLUX_OK;
}

/*
 * flux_a2a_delegate_result — Send a delegation result back to the delegator.
 *
 * This is an internal helper used by workers to return results.
 * Creates a RESULT message referencing the original delegate message.
 */
flux_status_t flux_a2a_delegate_result(uint32_t worker, uint32_t delegate_msg_id,
                                        const void *result, uint32_t result_len)
{
    if (!g_agent_rt.initialized)
        return FLUX_ERR_INVALID;

    if (result_len > FLUX_A2A_MAX_PAYLOAD)
        return FLUX_ERR_OVERFLOW;

    agent_lock_real();

    flux_agent_slot_t *worker_slot = agent_slot_by_id(worker);
    if (!worker_slot) {
        agent_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    if (!(worker_slot->desc.capabilities & FLUX_CAP_COMMUNICATE)) {
        agent_unlock();
        return FLUX_ERR_DENIED;
    }

    /* Find the delegation in the target agent's tracking — we need to know
     * who originally delegated to us. Search all agents for this msg_id. */
    uint32_t delegator = FLUX_AGENT_ID_INVALID;
    for (int i = 0; i < FLUX_AGENT_MAX; i++) {
        if (!g_agent_rt.agents[i].active)
            continue;
        for (int j = 0; j < g_agent_rt.agents[i].num_delegations; j++) {
            if (g_agent_rt.agents[i].delegations[j].original_msg_id == delegate_msg_id) {
                delegator = g_agent_rt.agents[i].desc.agent_id;
                /* Mark delegation as complete */
                g_agent_rt.agents[i].delegations[j].active = false;
                break;
            }
        }
        if (delegator != FLUX_AGENT_ID_INVALID)
            break;
    }

    if (delegator == FLUX_AGENT_ID_INVALID) {
        agent_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    flux_agent_slot_t *delegator_slot = agent_slot_by_id(delegator);
    if (!delegator_slot) {
        agent_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    /* Allocate result message */
    flux_a2a_msg_t *msg = msg_pool_alloc();
    if (!msg) {
        agent_unlock();
        return FLUX_ERR_NOMEM;
    }

    msg->msg_id = msg_generate_id();
    msg->reply_to = delegate_msg_id;
    msg->type = FLUX_A2A_RESULT;
    msg->sender = worker;
    msg->target = delegator;
    msg->required_cap = FLUX_CAP_NONE;
    msg->timestamp = agent_now();
    msg->deadline = 0;
    msg->priority = 2;
    msg->payload_len = result_len;

    if (result && result_len > 0)
        memcpy(msg->payload, result, result_len);

    flux_status_t status = inbox_enqueue(delegator_slot, msg);
    if (status != FLUX_OK) {
        msg_pool_free(msg);
        agent_unlock();
        return status;
    }

    worker_slot->desc.msg_count_sent++;
    delegator_slot->desc.msg_count_recv++;
    delegator_slot->desc.last_active = agent_now();
    worker_slot->desc.last_active = agent_now();
    g_agent_rt.total_messages_sent++;
    g_agent_rt.total_messages_recv++;

    /* Wake delegator */
    if (delegator_slot->desc.state == FLUX_AGENT_WAITING ||
        delegator_slot->desc.state == FLUX_AGENT_DELEGATING) {
        delegator_slot->desc.state = FLUX_AGENT_IDLE;
    }

    agent_unlock();

    return FLUX_OK;
}
