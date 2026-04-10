/*
 * FLUX OS — Agent Lifecycle Management
 *
 * Implements the core agent table, agent ID allocation via bitmap,
 * and all agent lifecycle functions: init, spawn, terminate, pause,
 * resume, query, and listing.
 *
 * Design:
 *   - Fixed-size agent table (max 64 agents)
 *   - Bitmap-based ID allocation for O(1) free slot lookup
 *   - Each agent slot owns a VM instance and bytecode copy
 *   - Agent IDs 0-3 are reserved (INVALID, KERNEL, SYSTEM, COMPILER)
 *   - User agents start from ID 4
 *   - All mutations are protected by a global spinlock
 */

#include "agent_internal.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ========================================================================
 * Global Runtime State
 * ======================================================================== */

volatile int g_agent_lock = 0;
flux_agent_runtime_t g_agent_rt;

/* ========================================================================
 * Internal Helpers
 * ======================================================================== */

/*
 * agent_index_from_id — Convert an agent ID to a table index.
 * IDs 0-3 are reserved; user agents start at 4, mapping to index 0.
 * Returns -1 if the ID is out of range.
 */
int agent_index_from_id(uint32_t id)
{
    if (id < 4 || id >= FLUX_AGENT_MAX + 4)
        return -1;
    return (int)(id - 4);
}

/*
 * agent_slot_by_id — Get the agent slot pointer for a given agent ID.
 * Returns NULL if the ID is invalid or the slot is not active.
 */
flux_agent_slot_t *agent_slot_by_id(uint32_t id)
{
    int idx = agent_index_from_id(id);
    if (idx < 0)
        return NULL;
    if (!g_agent_rt.agents[idx].active)
        return NULL;
    return &g_agent_rt.agents[idx];
}

/*
 * agent_count_active — Count the number of active agent slots.
 */
static int agent_count_active(void)
{
    int count = 0;
    for (int i = 0; i < FLUX_AGENT_MAX; i++) {
        if (g_agent_rt.agents[i].active)
            count++;
    }
    return count;
}

/*
 * agent_update_peak — Update the peak agent count if current exceeds peak.
 */
static void agent_update_peak(void)
{
    int current = agent_count_active();
    if ((uint32_t)current > g_agent_rt.peak_agents)
        g_agent_rt.peak_agents = (uint32_t)current;
}

/*
 * agent_alloc_id — Allocate a new agent ID from the bitmap.
 * Returns the new agent ID, or FLUX_AGENT_ID_INVALID if table is full.
 */
static uint32_t agent_alloc_id(void)
{
    int bit = bitmap_find_zero(g_agent_rt.agent_bitmap, FLUX_AGENT_MAX);
    if (bit < 0)
        return FLUX_AGENT_ID_INVALID;

    bitmap_set(g_agent_rt.agent_bitmap, bit);
    return (uint32_t)(bit + 4); /* IDs start at 4 */
}

/*
 * agent_free_id — Release an agent ID back to the bitmap.
 */
static void agent_free_id(uint32_t id)
{
    int idx = agent_index_from_id(id);
    if (idx >= 0)
        bitmap_clear(g_agent_rt.agent_bitmap, idx);
}

/*
 * agent_slot_cleanup — Free all resources held by an agent slot.
 */
static void agent_slot_cleanup(flux_agent_slot_t *slot)
{
    if (!slot)
        return;

    /* Free bytecode */
    if (slot->bytecode) {
        free(slot->bytecode);
        slot->bytecode = NULL;
        slot->bytecode_len = 0;
    }

    /* Destroy VM if initialized */
    if (slot->vm_initialized) {
        flux_vm_destroy(&slot->vm);
        slot->vm_initialized = false;
    }

    /* Drain inbox — free any pooled messages */
    for (int i = 0; i < slot->inbox_count; i++) {
        int idx = (slot->inbox_head + i) % FLUX_INBOX_SIZE;
        if (slot->inbox[idx]) {
            msg_pool_free(slot->inbox[idx]);
            slot->inbox[idx] = NULL;
        }
    }
    slot->inbox_head = 0;
    slot->inbox_tail = 0;
    slot->inbox_count = 0;

    /* Clear delegation tracking */
    slot->num_delegations = 0;
    memset(slot->delegations, 0, sizeof(slot->delegations));

    /* Clear audit log */
    slot->audit_count = 0;
    memset(slot->audit_log, 0, sizeof(slot->audit_log));

    /* Mark inactive */
    slot->active = false;
    slot->paused = false;
    memset(&slot->desc, 0, sizeof(slot->desc));
    slot->desc.agent_id = FLUX_AGENT_ID_INVALID;
}

/* ========================================================================
 * Default Configuration
 * ======================================================================== */

static const flux_agent_config_t s_default_config = {
    .max_agents            = FLUX_AGENT_MAX,
    .max_messages_queue    = FLUX_INBOX_SIZE,
    .default_cpu_ticks     = 1000000,
    .default_memory        = (256 * 1024),   /* 256 KB */
    .heartbeat_interval_ms = 5000,
    .auto_heartbeat        = true,
    .auto_terminate_idle   = false,
    .idle_timeout_ticks    = 0,
};

/* ========================================================================
 * Public API — Initialization & Shutdown
 * ======================================================================== */

/*
 * flux_agent_init — Initialize the agent runtime subsystem.
 *
 * Sets up the agent table, message pool, topic registry, and
 * capability registry. Must be called before any agent operations.
 *
 * Parameters:
 *   config — Runtime configuration (NULL for defaults)
 *
 * Returns:
 *   FLUX_OK on success, FLUX_ERR_GENERAL on re-initialization.
 */
flux_status_t flux_agent_init(const flux_agent_config_t *config)
{
    if (g_agent_rt.initialized) {
        /* Already initialized — shutdown first */
        flux_agent_shutdown();
    }

    /* Apply configuration */
    if (config) {
        memcpy(&g_agent_rt.config, config, sizeof(flux_agent_config_t));
    } else {
        memcpy(&g_agent_rt.config, &s_default_config, sizeof(flux_agent_config_t));
    }

    /* Clamp max_agents to our table size */
    if (g_agent_rt.config.max_agents > FLUX_AGENT_MAX)
        g_agent_rt.config.max_agents = FLUX_AGENT_MAX;

    /* Clear agent table */
    memset(g_agent_rt.agents, 0, sizeof(g_agent_rt.agents));
    for (int i = 0; i < FLUX_AGENT_MAX; i++) {
        g_agent_rt.agents[i].desc.agent_id = FLUX_AGENT_ID_INVALID;
        g_agent_rt.agents[i].active = false;
    }

    /* Clear ID allocation bitmap — IDs 0-3 are "reserved" (always set) */
    memset(g_agent_rt.agent_bitmap, 0, sizeof(g_agent_rt.agent_bitmap));
    bitmap_set(g_agent_rt.agent_bitmap, 0); /* ID 4 maps to bit 0, but
                                                we just start from bit 0 = ID 4 */

    /* Initialize message pool */
    memset(&g_agent_rt.msg_pool, 0, sizeof(flux_msg_pool_t));

    /* Initialize message ID counter — start from 1 so 0 is invalid */
    g_agent_rt.next_msg_id = 1;

    /* Clear topic registry */
    memset(g_agent_rt.topics, 0, sizeof(g_agent_rt.topics));
    g_agent_rt.num_topics = 0;

    /* Clear published capabilities */
    memset(g_agent_rt.pub_caps, 0, sizeof(g_agent_rt.pub_caps));
    g_agent_rt.num_pub_caps = 0;

    /* Reset statistics */
    g_agent_rt.total_messages_sent = 0;
    g_agent_rt.total_messages_recv = 0;
    g_agent_rt.total_broadcasts = 0;
    g_agent_rt.total_delegations = 0;
    g_agent_rt.peak_agents = 0;

    /* Reset scheduling */
    g_agent_rt.schedule_cursor = 0;

    /* Tick counter */
    g_agent_rt.tick_counter = 0;

    /* Lock */
    g_agent_lock = 0;

    g_agent_rt.initialized = true;

    return FLUX_OK;
}

/*
 * flux_agent_shutdown — Shut down the agent runtime.
 *
 * Terminates all active agents, frees their resources, and resets
 * the runtime to an uninitialized state.
 */
void flux_agent_shutdown(void)
{
    if (!g_agent_rt.initialized)
        return;

    agent_lock_real();

    /* Terminate all active agents */
    for (int i = 0; i < FLUX_AGENT_MAX; i++) {
        if (g_agent_rt.agents[i].active) {
            agent_slot_cleanup(&g_agent_rt.agents[i]);
        }
    }

    /* Clear ID bitmap */
    memset(g_agent_rt.agent_bitmap, 0, sizeof(g_agent_rt.agent_bitmap));

    /* Clear topic subscriptions */
    for (int i = 0; i < g_agent_rt.num_topics; i++) {
        g_agent_rt.topics[i].num_subscribers = 0;
    }
    g_agent_rt.num_topics = 0;

    /* Clear published capabilities */
    g_agent_rt.num_pub_caps = 0;

    /* Reset all statistics */
    g_agent_rt.total_messages_sent = 0;
    g_agent_rt.total_messages_recv = 0;
    g_agent_rt.total_broadcasts = 0;
    g_agent_rt.total_delegations = 0;
    g_agent_rt.peak_agents = 0;
    g_agent_rt.schedule_cursor = 0;

    g_agent_rt.initialized = false;

    agent_unlock();
}

/* ========================================================================
 * Public API — Agent Lifecycle
 * ======================================================================== */

/*
 * flux_agent_spawn — Create a new agent.
 *
 * Allocates an agent descriptor, sets up a VM region for sandboxed
 * execution, loads bytecode, and sets the initial state to CREATED.
 *
 * Parameters:
 *   name         — Agent name (truncated to 63 chars, can be NULL)
 *   model        — Execution model string (e.g., "bytecode", can be NULL)
 *   caps         — Initial capability bitmask
 *   bytecode     — Pointer to bytecode payload
 *   len          — Bytecode length in bytes
 *
 * Returns:
 *   Agent ID on success, FLUX_AGENT_ID_INVALID on failure.
 */
uint32_t flux_agent_spawn(const char *name, const char *model,
                           flux_cap_t caps, flux_addr_t bytecode,
                           flux_size_t len)
{
    if (!g_agent_rt.initialized)
        return FLUX_AGENT_ID_INVALID;

    if (agent_count_active() >= (int)g_agent_rt.config.max_agents) {
        return FLUX_AGENT_ID_INVALID;
    }

    agent_lock_real();

    /* Allocate agent ID */
    uint32_t id = agent_alloc_id();
    if (id == FLUX_AGENT_ID_INVALID) {
        agent_unlock();
        return FLUX_AGENT_ID_INVALID;
    }

    int idx = agent_index_from_id(id);
    flux_agent_slot_t *slot = &g_agent_rt.agents[idx];

    /* Clear the slot */
    memset(slot, 0, sizeof(flux_agent_slot_t));

    /* Copy bytecode */
    if (bytecode && len > 0) {
        slot->bytecode = (uint8_t *)malloc(len);
        if (!slot->bytecode) {
            agent_free_id(id);
            agent_unlock();
            return FLUX_AGENT_ID_INVALID;
        }
        memcpy(slot->bytecode, (const void *)bytecode, len);
        slot->bytecode_len = len;
    } else {
        slot->bytecode = NULL;
        slot->bytecode_len = 0;
    }

    /* Initialize VM for sandboxed execution */
    flux_vm_init(&slot->vm);
    slot->vm.sandboxed = true;
    slot->vm.owner_pid = (flux_pid_t)id;
    slot->vm_initialized = true;

    /* Load bytecode into VM if present */
    if (slot->bytecode && slot->bytecode_len > 0) {
        flux_vm_load(&slot->vm, slot->bytecode, slot->bytecode_len);
    }

    /* Set up memory region for agent sandbox */
    flux_vm_region_create(&slot->vm, "agent_workspace",
                          g_agent_rt.config.default_memory, false);

    /* Fill in descriptor */
    slot->desc.agent_id = id;
    slot->desc.state = FLUX_AGENT_CREATED;
    slot->desc.capabilities = caps;

    /* Name */
    if (name) {
        int i;
        for (i = 0; i < FLUX_AGENT_NAME_MAX - 1 && name[i]; i++)
            slot->desc.name[i] = name[i];
        slot->desc.name[i] = '\0';
    } else {
        snprintf(slot->desc.name, FLUX_AGENT_NAME_MAX, "agent-%u", id);
    }

    /* Model */
    if (model) {
        int i;
        for (i = 0; i < FLUX_AGENT_MODEL_MAX - 1 && model[i]; i++)
            slot->desc.model[i] = model[i];
        slot->desc.model[i] = '\0';
    } else {
        snprintf(slot->desc.model, FLUX_AGENT_MODEL_MAX, "bytecode");
    }

    /* PID */
    slot->desc.pid = (flux_pid_t)id;

    /* Resource limits from config */
    slot->desc.limits.max_cpu_ticks = g_agent_rt.config.default_cpu_ticks;
    slot->desc.limits.used_cpu_ticks = 0;
    slot->desc.limits.max_memory = g_agent_rt.config.default_memory;
    slot->desc.limits.used_memory = 0;
    slot->desc.limits.max_io_per_sec = 1024;
    slot->desc.limits.used_io_per_sec = 0;

    /* Communication counters */
    slot->desc.msg_count_sent = 0;
    slot->desc.msg_count_recv = 0;
    slot->desc.delegate_count = 0;

    /* Published capabilities */
    slot->desc.num_caps = 0;

    /* Lifecycle timestamps */
    slot->desc.create_time = agent_now();
    slot->desc.last_active = slot->desc.create_time;

    /* Slot state */
    slot->active = true;
    slot->paused = false;

    /* Inbox */
    slot->inbox_head = 0;
    slot->inbox_tail = 0;
    slot->inbox_count = 0;
    slot->inbox_total = 0;

    /* Delegations */
    slot->num_delegations = 0;
    memset(slot->delegations, 0, sizeof(slot->delegations));

    /* Audit */
    slot->audit_count = 0;

    /* Update peak */
    agent_update_peak();

    agent_unlock();

    return id;
}

/*
 * flux_agent_terminate — Terminate an agent and free its resources.
 *
 * Parameters:
 *   agent_id — Agent to terminate
 *
 * Returns:
 *   FLUX_OK on success, FLUX_ERR_NOTFOUND if agent doesn't exist.
 */
flux_status_t flux_agent_terminate(uint32_t agent_id)
{
    if (!g_agent_rt.initialized)
        return FLUX_ERR_INVALID;

    agent_lock_real();

    flux_agent_slot_t *slot = agent_slot_by_id(agent_id);
    if (!slot) {
        agent_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    /* Don't terminate reserved agents */
    if (agent_id <= FLUX_AGENT_ID_COMPILER) {
        agent_unlock();
        return FLUX_ERR_DENIED;
    }

    /* Mark as terminated before cleanup */
    slot->desc.state = FLUX_AGENT_TERMINATED;

    /* Clean up all resources */
    agent_slot_cleanup(slot);

    /* Release the ID */
    agent_free_id(agent_id);

    agent_unlock();

    return FLUX_OK;
}

/*
 * flux_agent_pause — Pause a running agent.
 *
 * The agent stops executing but retains all state. It can be
 * resumed with flux_agent_resume().
 *
 * Parameters:
 *   agent_id — Agent to pause
 *
 * Returns:
 *   FLUX_OK on success, error code on failure.
 */
flux_status_t flux_agent_pause(uint32_t agent_id)
{
    if (!g_agent_rt.initialized)
        return FLUX_ERR_INVALID;

    agent_lock_real();

    flux_agent_slot_t *slot = agent_slot_by_id(agent_id);
    if (!slot) {
        agent_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    /* Can only pause agents in certain states */
    if (slot->desc.state == FLUX_AGENT_TERMINATED ||
        slot->desc.state == FLUX_AGENT_FAILED) {
        agent_unlock();
        return FLUX_ERR_INVALID;
    }

    /* Already paused */
    if (slot->paused) {
        agent_unlock();
        return FLUX_OK;
    }

    slot->paused = true;

    /* If VM is running, halt it */
    if (slot->vm_initialized && slot->vm.state == FLUX_VM_RUNNING) {
        flux_vm_halt(&slot->vm);
    }

    /* Save PC from VM into descriptor for later resume */
    slot->desc.last_active = agent_now();

    agent_unlock();

    return FLUX_OK;
}

/*
 * flux_agent_resume — Resume a paused agent.
 *
 * Parameters:
 *   agent_id — Agent to resume
 *
 * Returns:
 *   FLUX_OK on success, error code on failure.
 */
flux_status_t flux_agent_resume(uint32_t agent_id)
{
    if (!g_agent_rt.initialized)
        return FLUX_ERR_INVALID;

    agent_lock_real();

    flux_agent_slot_t *slot = agent_slot_by_id(agent_id);
    if (!slot) {
        agent_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    /* Not paused */
    if (!slot->paused) {
        agent_unlock();
        return FLUX_OK;
    }

    slot->paused = false;
    slot->desc.last_active = agent_now();

    /* If the agent was thinking, restore VM to running state */
    if (slot->desc.state == FLUX_AGENT_THINKING && slot->vm_initialized) {
        slot->vm.state = FLUX_VM_IDLE;
    }

    agent_unlock();

    return FLUX_OK;
}

/* ========================================================================
 * Public API — Query Functions
 * ======================================================================== */

/*
 * flux_agent_get — Get a pointer to the agent descriptor.
 *
 * Parameters:
 *   agent_id — Agent to look up
 *
 * Returns:
 *   Pointer to agent descriptor, or NULL if agent doesn't exist.
 */
flux_agent_desc_t *flux_agent_get(uint32_t agent_id)
{
    if (!g_agent_rt.initialized)
        return NULL;

    agent_lock_real();

    flux_agent_slot_t *slot = agent_slot_by_id(agent_id);
    agent_unlock();

    if (!slot)
        return NULL;

    return &slot->desc;
}

/*
 * flux_agent_list — List all active agents into a buffer.
 *
 * Parameters:
 *   buf — Output buffer for agent descriptors
 *   max — Maximum number of descriptors to copy
 *
 * Returns:
 *   Number of agents copied, or total active count if buf is NULL.
 */
int flux_agent_list(flux_agent_desc_t *buf, int max)
{
    if (!g_agent_rt.initialized)
        return 0;

    agent_lock_real();

    int count = 0;
    for (int i = 0; i < FLUX_AGENT_MAX; i++) {
        if (!g_agent_rt.agents[i].active)
            continue;
        if (buf && count < max) {
            memcpy(&buf[count], &g_agent_rt.agents[i].desc,
                   sizeof(flux_agent_desc_t));
        }
        count++;
    }

    agent_unlock();

    return count;
}

/*
 * flux_agent_count — Return the number of active agents.
 */
int flux_agent_count(void)
{
    if (!g_agent_rt.initialized)
        return 0;

    agent_lock_real();
    int count = agent_count_active();
    agent_unlock();

    return count;
}

/*
 * flux_agent_state_name — Convert agent state enum to human-readable string.
 */
const char *flux_agent_state_name(flux_agent_state_t state)
{
    switch (state) {
        case FLUX_AGENT_CREATED:      return "CREATED";
        case FLUX_AGENT_INITIALIZING: return "INITIALIZING";
        case FLUX_AGENT_IDLE:         return "IDLE";
        case FLUX_AGENT_THINKING:     return "THINKING";
        case FLUX_AGENT_WAITING:      return "WAITING";
        case FLUX_AGENT_DELEGATING:   return "DELEGATING";
        case FLUX_AGENT_TERMINATED:   return "TERMINATED";
        case FLUX_AGENT_FAILED:       return "FAILED";
        default:                      return "UNKNOWN";
    }
}
