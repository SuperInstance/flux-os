/*
 * FLUX OS — Capability Security
 *
 * Implements capability-based access control for the agent runtime.
 * Agents hold capability tokens (bitmask) that determine what operations
 * they are allowed to perform. The system enforces a deny-by-default
 * policy: agents start with only explicitly granted capabilities.
 *
 * Design:
 *   - Capabilities are a 64-bit bitmask (one bit per capability)
 *   - Grant adds bits, revoke removes bits
 *   - Transfer moves specific bits from one agent to another
 *   - All mutations are logged in a per-agent audit trail
 *   - Capability inheritance: child agents inherit parent caps
 *   - Deny-by-default: newly spawned agents have only explicit caps
 *
 * Capability Categories:
 *   SPAWN       — Create new agents
 *   COMMUNICATE — Send/receive A2A messages
 *   COMPILE     — Use the self-compiler
 *   IO_READ     — Read from I/O devices/files
 *   IO_WRITE    — Write to I/O devices/files
 *   MEMORY      — Allocate/manipulate memory regions
 *   HARDWARE    — Direct hardware access
 *   NETWORK     — Network communication
 *   FILESYSTEM  — Filesystem operations
 *   DEBUG       — Debug/profiling access
 *   SUPERVISOR  — Administrative/supervisor operations
 */

#include "agent_internal.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ========================================================================
 * Audit Log Actions
 * ======================================================================== */

#define AUDIT_ACTION_GRANT    0
#define AUDIT_ACTION_REVOKE   1
#define AUDIT_ACTION_TRANSFER 2
#define AUDIT_ACTION_CHECK    3

/* ========================================================================
 * Audit Logging
 * ======================================================================== */

/*
 * audit_log_record — Record a capability action in the audit log.
 *
 * Each agent slot has a circular audit log buffer. New entries
 * overwrite the oldest when the buffer is full.
 *
 * Parameters:
 *   agent_id — Agent performing the action
 *   target_id — Target agent (0 if N/A)
 *   cap       — Capability being acted upon
 *   action    — Action type (grant/revoke/transfer/check)
 */
void audit_log_record(uint32_t agent_id, uint32_t target_id,
                      flux_cap_t cap, uint8_t action)
{
    int idx = agent_index_from_id(agent_id);
    if (idx < 0 || !g_agent_rt.agents[idx].active)
        return;

    flux_agent_slot_t *slot = &g_agent_rt.agents[idx];

    /* Check if this cap is already the most recent entry — dedup */
    if (slot->audit_count > 0) {
        int last = (slot->audit_count - 1) % FLUX_AUDIT_LOG_SIZE;
        if (slot->audit_log[last].cap == cap &&
            slot->audit_log[last].action == action &&
            slot->audit_log[last].target_id == target_id) {
            /* Update timestamp only */
            slot->audit_log[last].timestamp = agent_now();
            return;
        }
    }

    int entry = slot->audit_count % FLUX_AUDIT_LOG_SIZE;
    slot->audit_log[entry].timestamp = agent_now();
    slot->audit_log[entry].agent_id = agent_id;
    slot->audit_log[entry].target_id = target_id;
    slot->audit_log[entry].cap = cap;
    slot->audit_log[entry].action = action;
    slot->audit_count++;
}

/* ========================================================================
 * Public API — Capability Management
 * ======================================================================== */

/*
 * flux_agent_grant_cap — Grant a capability to an agent.
 *
 * Adds the specified capability bits to the agent's bitmask.
 * Only the SUPERVISOR can grant capabilities.
 *
 * Parameters:
 *   agent_id — Agent receiving the capability
 *   cap      — Capability bits to grant
 *
 * Returns:
 *   FLUX_OK on success, error code on failure.
 */
flux_status_t flux_agent_grant_cap(uint32_t agent_id, flux_cap_t cap)
{
    if (!g_agent_rt.initialized)
        return FLUX_ERR_INVALID;

    if (cap == FLUX_CAP_NONE)
        return FLUX_OK;

    if (cap == FLUX_CAP_ALL) {
        /* Granting ALL is a supervisor-only operation — checked by caller */
    }

    agent_lock_real();

    flux_agent_slot_t *slot = agent_slot_by_id(agent_id);
    if (!slot) {
        agent_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    /* Deny-by-default policy: only explicit grants are allowed */
    /* We don't check who is granting here — the kernel/trusted caller
     * is responsible for authorization. In a full OS, the caller would
     * need SUPERVISOR cap. */

    /* Add capability bits */
    flux_cap_t prev = slot->desc.capabilities;
    slot->desc.capabilities |= cap;

    /* Log the grant */
    audit_log_record(agent_id, 0, cap, AUDIT_ACTION_GRANT);

    agent_unlock();

    return FLUX_OK;
}

/*
 * flux_agent_revoke_cap — Revoke a capability from an agent.
 *
 * Removes the specified capability bits from the agent's bitmask.
 *
 * Parameters:
 *   agent_id — Agent losing the capability
 *   cap      — Capability bits to revoke
 *
 * Returns:
 *   FLUX_OK on success, error code on failure.
 */
flux_status_t flux_agent_revoke_cap(uint32_t agent_id, flux_cap_t cap)
{
    if (!g_agent_rt.initialized)
        return FLUX_ERR_INVALID;

    if (cap == FLUX_CAP_NONE)
        return FLUX_OK;

    agent_lock_real();

    flux_agent_slot_t *slot = agent_slot_by_id(agent_id);
    if (!slot) {
        agent_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    /* Cannot revoke from reserved system agents */
    if (agent_id <= FLUX_AGENT_ID_COMPILER) {
        agent_unlock();
        return FLUX_ERR_DENIED;
    }

    /* Remove capability bits */
    slot->desc.capabilities &= ~cap;

    /* Log the revocation */
    audit_log_record(agent_id, 0, cap, AUDIT_ACTION_REVOKE);

    agent_unlock();

    return FLUX_OK;
}

/*
 * flux_agent_has_cap — Check if an agent has a specific capability.
 *
 * Parameters:
 *   agent_id — Agent to check
 *   cap      — Capability bits to check
 *
 * Returns:
 *   true if ALL requested capability bits are present, false otherwise.
 */
bool flux_agent_has_cap(uint32_t agent_id, flux_cap_t cap)
{
    if (!g_agent_rt.initialized)
        return false;

    if (cap == FLUX_CAP_NONE)
        return true;

    agent_lock_real();

    flux_agent_slot_t *slot = agent_slot_by_id(agent_id);
    if (!slot) {
        agent_unlock();
        return false;
    }

    bool result = (slot->desc.capabilities & cap) == cap;

    /* Log the check (only for non-trivial checks) */
    if (cap != FLUX_CAP_NONE) {
        audit_log_record(agent_id, 0, cap, AUDIT_ACTION_CHECK);
    }

    agent_unlock();

    return result;
}

/*
 * flux_agent_transfer_cap — Transfer a capability between agents.
 *
 * Moves the specified capability bits from one agent to another.
 * The sender must possess the capability before transfer.
 * The SUPERVISOR capability cannot be transferred.
 *
 * Parameters:
 *   from — Source agent ID
 *   to   — Destination agent ID
 *   cap  — Capability bits to transfer
 *
 * Returns:
 *   FLUX_OK on success, error code on failure.
 */
flux_status_t flux_agent_transfer_cap(uint32_t from, uint32_t to, flux_cap_t cap)
{
    if (!g_agent_rt.initialized)
        return FLUX_ERR_INVALID;

    if (cap == FLUX_CAP_NONE)
        return FLUX_OK;

    /* SUPERVISOR capability cannot be transferred */
    if (cap & FLUX_CAP_SUPERVISOR) {
        return FLUX_ERR_DENIED;
    }

    agent_lock_real();

    /* Validate source */
    flux_agent_slot_t *from_slot = agent_slot_by_id(from);
    if (!from_slot) {
        agent_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    /* Validate destination */
    flux_agent_slot_t *to_slot = agent_slot_by_id(to);
    if (!to_slot) {
        agent_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    /* Can't transfer to self */
    if (from == to) {
        agent_unlock();
        return FLUX_ERR_INVALID;
    }

    /* Source must have the capability */
    if ((from_slot->desc.capabilities & cap) != cap) {
        agent_unlock();
        return FLUX_ERR_DENIED;
    }

    /* Cannot transfer from reserved agents */
    if (from <= FLUX_AGENT_ID_COMPILER) {
        agent_unlock();
        return FLUX_ERR_DENIED;
    }

    /* Perform the transfer atomically */
    from_slot->desc.capabilities &= ~cap;
    to_slot->desc.capabilities |= cap;

    /* Log on both sides */
    audit_log_record(from, to, cap, AUDIT_ACTION_TRANSFER);
    audit_log_record(to, from, cap, AUDIT_ACTION_TRANSFER);

    agent_unlock();

    return FLUX_OK;
}

/* ========================================================================
 * Capability Inheritance
 * ======================================================================== */

/*
 * flux_agent_inherit_caps — Have a child agent inherit capabilities from parent.
 *
 * The child receives a COPY of the parent's capabilities (the parent
 * retains its own). This is called during spawn-time setup.
 *
 * Parameters:
 *   parent_id — Parent agent ID
 *   child_id  — Child agent ID
 *
 * Returns:
 *   FLUX_OK on success, error code on failure.
 */
flux_status_t flux_agent_inherit_caps(uint32_t parent_id, uint32_t child_id)
{
    if (!g_agent_rt.initialized)
        return FLUX_ERR_INVALID;

    agent_lock_real();

    flux_agent_slot_t *parent = agent_slot_by_id(parent_id);
    if (!parent) {
        agent_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    flux_agent_slot_t *child = agent_slot_by_id(child_id);
    if (!child) {
        agent_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    /* Child inherits all of parent's capabilities */
    child->desc.capabilities = parent->desc.capabilities;

    /* Log inheritance on both sides */
    audit_log_record(parent_id, child_id, parent->desc.capabilities,
                     AUDIT_ACTION_GRANT);
    audit_log_record(child_id, parent_id, child->desc.capabilities,
                     AUDIT_ACTION_CHECK);

    agent_unlock();

    return FLUX_OK;
}

/* ========================================================================
 * Capability Inspection
 * ======================================================================== */

/*
 * flux_agent_caps_describe — Generate a string describing an agent's capabilities.
 *
 * Parameters:
 *   agent_id — Agent to inspect
 *   buf      — Output buffer
 *   buflen   — Buffer size
 *
 * Returns:
 *   Number of characters written (excluding null terminator).
 */
int flux_agent_caps_describe(uint32_t agent_id, char *buf, int buflen)
{
    if (!g_agent_rt.initialized || !buf || buflen <= 0)
        return 0;

    agent_lock_real();

    flux_agent_slot_t *slot = agent_slot_by_id(agent_id);
    if (!slot) {
        agent_unlock();
        if (buflen > 0) {
            buf[0] = '\0';
        }
        return 0;
    }

    flux_cap_t caps = slot->desc.capabilities;
    agent_unlock();

    int pos = 0;

    if (caps == FLUX_CAP_NONE) {
        pos = snprintf(buf, buflen, "NONE");
        return pos;
    }

    if (caps == FLUX_CAP_ALL) {
        pos = snprintf(buf, buflen, "ALL");
        return pos;
    }

    /* List individual capabilities */
    bool first = true;
    struct {
        flux_cap_t flag;
        const char *name;
    } cap_list[] = {
        { FLUX_CAP_SPAWN,        "SPAWN" },
        { FLUX_CAP_COMMUNICATE,  "COMMUNICATE" },
        { FLUX_CAP_COMPILE,      "COMPILE" },
        { FLUX_CAP_IO_READ,      "IO_READ" },
        { FLUX_CAP_IO_WRITE,     "IO_WRITE" },
        { FLUX_CAP_MEMORY,       "MEMORY" },
        { FLUX_CAP_HARDWARE,     "HARDWARE" },
        { FLUX_CAP_NETWORK,      "NETWORK" },
        { FLUX_CAP_FILESYSTEM,   "FILESYSTEM" },
        { FLUX_CAP_DEBUG,        "DEBUG" },
        { FLUX_CAP_SUPERVISOR,   "SUPERVISOR" },
    };

    for (int i = 0; i < 11; i++) {
        if (caps & cap_list[i].flag) {
            if (!first && pos < buflen - 2) {
                pos += snprintf(buf + pos, buflen - pos, "|");
            }
            if (pos < buflen - 1) {
                pos += snprintf(buf + pos, buflen - pos, "%s", cap_list[i].name);
                first = false;
            }
        }
    }

    return pos;
}

/*
 * flux_agent_audit_dump — Dump the audit log for an agent.
 *
 * Parameters:
 *   agent_id — Agent to inspect
 *   buf      — Output buffer
 *   buflen   — Buffer size
 *
 * Returns:
 *   Number of characters written.
 */
int flux_agent_audit_dump(uint32_t agent_id, char *buf, int buflen)
{
    if (!g_agent_rt.initialized || !buf || buflen <= 0)
        return 0;

    agent_lock_real();

    flux_agent_slot_t *slot = agent_slot_by_id(agent_id);
    if (!slot) {
        agent_unlock();
        if (buflen > 0) buf[0] = '\0';
        return 0;
    }

    int pos = 0;
    int count = slot->audit_count;
    int start = (count > FLUX_AUDIT_LOG_SIZE) ? (count - FLUX_AUDIT_LOG_SIZE) : 0;
    int entries = count - start;
    if (entries > FLUX_AUDIT_LOG_SIZE)
        entries = FLUX_AUDIT_LOG_SIZE;

    const char *action_names[] = { "GRANT", "REVOKE", "TRANSFER", "CHECK" };

    for (int i = 0; i < entries && pos < buflen - 1; i++) {
        int idx = (start + i) % FLUX_AUDIT_LOG_SIZE;
        const char *aname = (slot->audit_log[idx].action < 4)
                            ? action_names[slot->audit_log[idx].action]
                            : "UNKNOWN";

        pos += snprintf(buf + pos, buflen - pos,
                        "[%llu] %s cap=0x%llx target=%u\n",
                        (unsigned long long)slot->audit_log[idx].timestamp,
                        aname,
                        (unsigned long long)slot->audit_log[idx].cap,
                        slot->audit_log[idx].target_id);
    }

    agent_unlock();

    return pos;
}
