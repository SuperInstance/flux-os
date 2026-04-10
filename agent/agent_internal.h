/*
 * FLUX OS — Agent Runtime Internal Header
 *
 * Shared state and internal helpers for the agent subsystem modules.
 * This header is NOT part of the public API.
 */

#ifndef FLUX_AGENT_INTERNAL_H
#define FLUX_AGENT_INTERNAL_H

#include "flux/agent.h"
#include "flux/vm.h"
#include "flux/opcodes.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ========================================================================
 * Configuration Constants
 * ======================================================================== */

#define FLUX_AGENT_MAX           64
#define FLUX_AGENT_BITMAP_WORDS  ((FLUX_AGENT_MAX + 63) / 64)

#define FLUX_MSG_POOL_SIZE       512
#define FLUX_INBOX_SIZE          64
#define FLUX_TOPIC_MAX           32
#define FLUX_TOPIC_SUBS_MAX      64
#define FLUX_PUB_CAPS_MAX        256
#define FLUX_AUDIT_LOG_SIZE      512

/* ========================================================================
 * Internal Agent Slot (extends the public descriptor with VM state)
 * ======================================================================== */

typedef struct {
    flux_agent_desc_t    desc;

    /* VM instance for sandboxed execution */
    flux_vm_t            vm;
    bool                 vm_initialized;

    /* Bytecode copy (owned by agent slot) */
    uint8_t             *bytecode;
    flux_size_t          bytecode_len;

    /* Slot state */
    bool                 active;       /* Slot is in use */
    bool                 paused;       /* Agent is paused */

    /* Per-agent inbox queue */
    flux_a2a_msg_t      *inbox[FLUX_INBOX_SIZE];
    int                  inbox_head;
    int                  inbox_tail;
    int                  inbox_count;
    int                  inbox_total;  /* Total messages ever received */

    /* Delegation tracking */
    struct {
        uint32_t         original_msg_id;
        uint32_t         target_agent;
        bool             active;
    } delegations[16];
    int                  num_delegations;

    /* Capabilities audit log entry */
    struct {
        uint64_t         timestamp;
        uint32_t         agent_id;
        uint32_t         target_id;
        flux_cap_t       cap;
        uint8_t          action;  /* 0=grant, 1=revoke, 2=transfer, 3=check */
    } audit_log[FLUX_AUDIT_LOG_SIZE];
    int                  audit_count;

    /* Resource tracking */
    uint64_t             last_tick_update;
} flux_agent_slot_t;

/* ========================================================================
 * Message Pool
 * ======================================================================== */

#define FLUX_MSG_POOL_WORDS  ((FLUX_MSG_POOL_SIZE + 63) / 64)

typedef struct {
    flux_a2a_msg_t       messages[FLUX_MSG_POOL_SIZE];
    uint64_t             allocated[FLUX_MSG_POOL_WORDS]; /* Bitmap */
    uint64_t             freed;       /* Count of total frees for reuse */
} flux_msg_pool_t;

/* ========================================================================
 * Topic Subscription
 * ======================================================================== */

typedef struct {
    char                 name[FLUX_TOPIC_MAX];
    uint32_t             subscribers[FLUX_TOPIC_SUBS_MAX];
    int                  num_subscribers;
} flux_topic_t;

/* ========================================================================
 * Published Capability
 * ======================================================================== */

typedef struct {
    char                 name[32];
    uint32_t             agent_id;
    char                 description[128];
    bool                 active;
} flux_published_cap_t;

/* ========================================================================
 * Runtime State (shared across all agent modules)
 * ======================================================================== */

typedef struct {
    bool                 initialized;

    /* Configuration */
    flux_agent_config_t  config;

    /* Agent table */
    flux_agent_slot_t    agents[FLUX_AGENT_MAX];
    uint64_t             agent_bitmap[FLUX_AGENT_BITMAP_WORDS]; /* ID allocation */

    /* Message pool */
    flux_msg_pool_t      msg_pool;

    /* Message ID counter */
    uint32_t             next_msg_id;

    /* Topic subscriptions */
    flux_topic_t         topics[FLUX_TOPIC_MAX];
    int                  num_topics;

    /* Published capabilities registry */
    flux_published_cap_t pub_caps[FLUX_PUB_CAPS_MAX];
    int                  num_pub_caps;

    /* Global statistics */
    uint64_t             total_messages_sent;
    uint64_t             total_messages_recv;
    uint64_t             total_broadcasts;
    uint64_t             total_delegations;
    uint32_t             peak_agents;

    /* Schedule round-robin index */
    int                  schedule_cursor;

    /* Tick counter for timestamps */
    uint64_t             tick_counter;
} flux_agent_runtime_t;

/* The global runtime state */
extern flux_agent_runtime_t g_agent_rt;

/* ========================================================================
 * Spinlock Helpers
 * ======================================================================== */

static inline void agent_lock(void)
{
    /* Simple spinlock placeholder — in a real kernel this would use
     * proper atomic operations or mutexes */
    while (__atomic_exchange_n(&(int){0}, 1, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause" ::: "memory");
    }
}

/*
 * NOTE: The above agent_lock is a placeholder that won't work correctly
 * in practice. For a real kernel we would use a proper spinlock variable.
 * For this implementation we use a single global lock variable.
 */
extern volatile int g_agent_lock;

static inline void agent_lock_real(void)
{
    while (__atomic_exchange_n(&g_agent_lock, 1, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause" ::: "memory");
    }
}

static inline void agent_unlock(void)
{
    __atomic_store_n(&g_agent_lock, 0, __ATOMIC_RELEASE);
}

/* ========================================================================
 * Utility Functions
 * ======================================================================== */

/* Get current tick count */
static inline uint64_t agent_now(void)
{
    return g_agent_rt.tick_counter;
}

/* Bitmap operations for agent ID allocation */
static inline void bitmap_set(uint64_t *bm, int bit)
{
    bm[bit / 64] |= (1ULL << (bit % 64));
}

static inline void bitmap_clear(uint64_t *bm, int bit)
{
    bm[bit / 64] &= ~(1ULL << (bit % 64));
}

static inline bool bitmap_test(const uint64_t *bm, int bit)
{
    return (bm[bit / 64] & (1ULL << (bit % 64))) != 0;
}

/* Find first zero bit in bitmap, returns -1 if full */
static inline int bitmap_find_zero(const uint64_t *bm, int nbits)
{
    for (int i = 0; i < nbits; i++) {
        if (!bitmap_test(bm, i))
            return i;
    }
    return -1;
}

/* Cap name lookup for debugging */
static inline const char *cap_name(flux_cap_t cap)
{
    if (cap & FLUX_CAP_SPAWN)        return "SPAWN";
    if (cap & FLUX_CAP_COMMUNICATE)  return "COMMUNICATE";
    if (cap & FLUX_CAP_COMPILE)      return "COMPILE";
    if (cap & FLUX_CAP_IO_READ)      return "IO_READ";
    if (cap & FLUX_CAP_IO_WRITE)     return "IO_WRITE";
    if (cap & FLUX_CAP_MEMORY)       return "MEMORY";
    if (cap & FLUX_CAP_HARDWARE)     return "HARDWARE";
    if (cap & FLUX_CAP_NETWORK)      return "NETWORK";
    if (cap & FLUX_CAP_FILESYSTEM)   return "FILESYSTEM";
    if (cap & FLUX_CAP_DEBUG)        return "DEBUG";
    if (cap & FLUX_CAP_SUPERVISOR)   return "SUPERVISOR";
    return "UNKNOWN";
}

/* A2A message type name */
static inline const char *a2a_type_name(flux_a2a_type_t type)
{
    switch (type) {
        case FLUX_A2A_TELL:       return "TELL";
        case FLUX_A2A_ASK:        return "ASK";
        case FLUX_A2A_REPLY:      return "REPLY";
        case FLUX_A2A_DELEGATE:   return "DELEGATE";
        case FLUX_A2A_RESULT:     return "RESULT";
        case FLUX_A2A_BARRIER:    return "BARRIER";
        case FLUX_A2A_SUBSCRIBE:  return "SUBSCRIBE";
        case FLUX_A2A_EVENT:      return "EVENT";
        case FLUX_A2A_ERROR:      return "ERROR";
        case FLUX_A2A_CAPABILITY: return "CAPABILITY";
        case FLUX_A2A_HEARTBEAT:  return "HEARTBEAT";
        default:                  return "UNKNOWN";
    }
}

/* ========================================================================
 * Internal Function Declarations (cross-module)
 * ======================================================================== */

/* From agent.c */
flux_agent_slot_t *agent_slot_by_id(uint32_t id);
int agent_index_from_id(uint32_t id);

/* From a2a.c */
flux_a2a_msg_t *msg_pool_alloc(void);
void msg_pool_free(flux_a2a_msg_t *msg);

/* From capability.c */
void audit_log_record(uint32_t agent_id, uint32_t target_id,
                      flux_cap_t cap, uint8_t action);

/* From discovery.c */
flux_topic_t *find_topic_by_name(const char *name);
flux_topic_t *find_or_create_topic(const char *name);

#endif /* FLUX_AGENT_INTERNAL_H */
