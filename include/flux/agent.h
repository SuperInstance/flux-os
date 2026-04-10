/*
 * FLUX OS — Agent Runtime
 *
 * The Agent Runtime provides the A2A (Agent-to-Agent) protocol layer for
 * the FLUX OS. Every process can optionally be an "agent" — a autonomous
 * entity that can communicate with other agents, delegate tasks, and
 * compose solutions together.
 *
 * Agent Lifecycle:
 *   1. SPAWN: Create agent with capability set and bytecode payload
 *   2. INIT: Agent runs initialization code, publishes capabilities
 *   3. THINK: Agent processes tasks, makes decisions
 *   4. TELL: Agent sends message to another agent (fire-and-forget)
 *   5. ASK: Agent requests data/action from another agent (request-reply)
 *   6. DELEGATE: Agent offloads subtask to specialized agent
 *   7. BARRIER: Synchronization point between agents
 *   8. EXIT: Agent terminates, resources freed
 *
 * Security Model:
 *   - Capability-based: agents hold capability tokens, not roles
 *   - Sandboxed execution: agents run in isolated VM regions
 *   - Message validation: all A2A messages pass through capability check
 *   - Resource limits: CPU time, memory, I/O bandwidth caps
 */

#ifndef FLUX_AGENT_H
#define FLUX_AGENT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "flux/kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Agent ID & Capability Types
 * ======================================================================== */

#define FLUX_AGENT_ID_INVALID   0
#define FLUX_AGENT_ID_KERNEL    1
#define FLUX_AGENT_ID_SYSTEM    2
#define FLUX_AGENT_ID_COMPILER  3

/* Capability flags */
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

/* ========================================================================
 * Agent States
 * ======================================================================== */

typedef enum {
    FLUX_AGENT_CREATED     = 0,
    FLUX_AGENT_INITIALIZING = 1,
    FLUX_AGENT_IDLE        = 2,
    FLUX_AGENT_THINKING    = 3,
    FLUX_AGENT_WAITING     = 4,
    FLUX_AGENT_DELEGATING  = 5,
    FLUX_AGENT_TERMINATED  = 6,
    FLUX_AGENT_FAILED      = 7,
} flux_agent_state_t;

/* ========================================================================
 * A2A Message Types
 * ======================================================================== */

typedef enum {
    FLUX_A2A_TELL       = 0,   /* Fire-and-forget notification */
    FLUX_A2A_ASK        = 1,   /* Request with expected reply */
    FLUX_A2A_REPLY      = 2,   /* Response to ASK */
    FLUX_A2A_DELEGATE   = 3,   /* Task delegation */
    FLUX_A2A_RESULT     = 4,   /* Delegation result */
    FLUX_A2A_BARRIER    = 5,   /* Synchronization point */
    FLUX_A2A_SUBSCRIBE  = 6,   /* Event subscription */
    FLUX_A2A_EVENT      = 7,   /* Event notification */
    FLUX_A2A_ERROR      = 8,   /* Error message */
    FLUX_A2A_CAPABILITY = 9,   /* Capability grant/transfer */
    FLUX_A2A_HEARTBEAT  = 10,  /* Keep-alive ping */
} flux_a2a_type_t;

/* ========================================================================
 * A2A Message
 * ======================================================================== */

#define FLUX_A2A_MAX_PAYLOAD  4096
#define FLUX_A2A_MAX_TOPIC    64

typedef struct {
    uint32_t          msg_id;
    uint32_t          reply_to;     /* Message ID this replies to */
    flux_a2a_type_t   type;
    uint32_t          sender;
    uint32_t          target;
    flux_cap_t        required_cap; /* Capability needed to receive */
    uint64_t          timestamp;
    uint64_t          deadline;     /* Deadline in ticks (0 = no deadline) */
    uint32_t          priority;
    char              topic[FLUX_A2A_MAX_TOPIC];
    uint8_t           payload[FLUX_A2A_MAX_PAYLOAD];
    uint32_t          payload_len;
} flux_a2a_msg_t;

/* ========================================================================
 * Agent Descriptor
 * ======================================================================== */

#define FLUX_AGENT_NAME_MAX     64
#define FLUX_AGENT_MODEL_MAX    32
#define FLUX_AGENT_MAX_CAPS     16

typedef struct {
    uint32_t            agent_id;
    char                name[FLUX_AGENT_NAME_MAX];
    char                model[FLUX_AGENT_MODEL_MAX];  /* Execution model */
    flux_agent_state_t  state;
    flux_cap_t          capabilities;
    flux_pid_t          pid;           /* Underlying process */

    /* Published capabilities (what this agent can do) */
    struct {
        char            name[32];
        char            description[128];
    } caps[FLUX_AGENT_MAX_CAPS];
    int                num_caps;

    /* Resource limits */
    struct {
        uint64_t        max_cpu_ticks;
        uint64_t        used_cpu_ticks;
        flux_size_t     max_memory;
        flux_size_t     used_memory;
        uint32_t        max_io_per_sec;
        uint32_t        used_io_per_sec;
    } limits;

    /* Communication */
    uint32_t            msg_count_sent;
    uint32_t            msg_count_recv;
    uint32_t            delegate_count;

    /* Lifecycle */
    uint64_t            create_time;
    uint64_t            last_active;
} flux_agent_desc_t;

/* ========================================================================
 * Agent Runtime Configuration
 * ======================================================================== */

typedef struct {
    uint32_t    max_agents;
    uint32_t    max_messages_queue;
    uint32_t    default_cpu_ticks;
    flux_size_t default_memory;
    uint32_t    heartbeat_interval_ms;
    bool        auto_heartbeat;
    bool        auto_terminate_idle;
    uint32_t    idle_timeout_ticks;
} flux_agent_config_t;

/* ========================================================================
 * Agent Runtime API
 * ======================================================================== */

/* Initialize the agent runtime */
flux_status_t  flux_agent_init(const flux_agent_config_t *config);
void           flux_agent_shutdown(void);

/* Agent lifecycle */
uint32_t       flux_agent_spawn(const char *name, const char *model,
                                flux_cap_t caps, flux_addr_t bytecode,
                                flux_size_t bytecode_len);
flux_status_t  flux_agent_terminate(uint32_t agent_id);
flux_status_t  flux_agent_pause(uint32_t agent_id);
flux_status_t  flux_agent_resume(uint32_t agent_id);

/* Query */
flux_agent_desc_t *flux_agent_get(uint32_t agent_id);
int            flux_agent_list(flux_agent_desc_t *buf, int max);
int            flux_agent_count(void);
const char    *flux_agent_state_name(flux_agent_state_t state);

/* Capability management */
flux_status_t  flux_agent_grant_cap(uint32_t agent_id, flux_cap_t cap);
flux_status_t  flux_agent_revoke_cap(uint32_t agent_id, flux_cap_t cap);
bool           flux_agent_has_cap(uint32_t agent_id, flux_cap_t cap);
flux_status_t  flux_agent_transfer_cap(uint32_t from, uint32_t to, flux_cap_t cap);

/* A2A Messaging */
flux_status_t  flux_a2a_send(uint32_t sender, uint32_t target,
                             flux_a2a_type_t type, const void *payload,
                             uint32_t len, uint32_t *msg_id);
flux_status_t  flux_a2a_recv(uint32_t agent_id, flux_a2a_msg_t *msg,
                             uint32_t timeout_ms);
flux_status_t  flux_a2a_broadcast(uint32_t sender, const char *topic,
                                  const void *payload, uint32_t len);
flux_status_t  flux_a2a_reply(const flux_a2a_msg_t *original,
                              const void *payload, uint32_t len);
flux_status_t  flux_a2a_delegate(uint32_t from, uint32_t to,
                                 const void *task, uint32_t task_len,
                                 uint32_t *delegate_id);

/* Agent execution */
flux_status_t  flux_agent_run(uint32_t agent_id, uint32_t max_ticks);
flux_status_t  flux_agent_step(uint32_t agent_id);
flux_status_t  flux_agent_schedule(void);  /* Run all ready agents */

/* Publishing / Discovery */
flux_status_t  flux_agent_publish_cap(uint32_t agent_id, const char *name,
                                      const char *description);
flux_status_t  flux_agent_discover(const char *capability,
                                   flux_agent_desc_t *results, int max);
flux_status_t  flux_agent_subscribe(uint32_t agent_id, const char *topic);
flux_status_t  flux_agent_unsubscribe(uint32_t agent_id, const char *topic);

/* Runtime info */
void           flux_agent_info(char *buf, flux_size_t len);

#ifdef __cplusplus
}
#endif

#endif /* FLUX_AGENT_H */
