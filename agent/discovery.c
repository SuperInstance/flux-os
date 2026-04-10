/*
 * FLUX OS — Agent Discovery
 *
 * Implements the capability publishing and discovery system that allows
 * agents to find each other based on what they can do. Also provides
 * topic-based pub/sub for event-driven communication patterns.
 *
 * Design:
 *   - Agents publish capabilities (name + description) to a global registry
 *   - Other agents can discover providers by capability name
 *   - Topic subscriptions allow agents to receive broadcast events
 *   - Published capabilities are stored in a flat table (max 256 entries)
 *   - Topics are stored in a flat table (max 32 topics)
 *   - Linear search for discovery (sufficient for agent counts ≤ 64)
 *
 * Discovery Flow:
 *   1. Agent A publishes capability "math.add"
 *   2. Agent B discovers "math.add" → gets Agent A's descriptor
 *   3. Agent B sends ASK message to Agent A
 *   4. Agent A replies with result
 *
 * Topic Flow:
 *   1. Agent A subscribes to topic "alerts"
 *   2. Agent B broadcasts to topic "alerts"
 *   3. Agent A receives the broadcast in its inbox
 */

#include "agent_internal.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ========================================================================
 * Internal Helpers — Topic Management
 * ======================================================================== */

/*
 * find_topic_by_name — Find a topic entry by name.
 *
 * Parameters:
 *   name — Topic name to search for
 *
 * Returns:
 *   Pointer to the topic entry, or NULL if not found.
 */
flux_topic_t *find_topic_by_name(const char *name)
{
    if (!name || name[0] == '\0')
        return NULL;

    for (int i = 0; i < g_agent_rt.num_topics; i++) {
        if (strcmp(g_agent_rt.topics[i].name, name) == 0)
            return &g_agent_rt.topics[i];
    }

    return NULL;
}

/*
 * find_or_create_topic — Find an existing topic or create a new one.
 *
 * Parameters:
 *   name — Topic name
 *
 * Returns:
 *   Pointer to the topic entry, or NULL if table is full.
 */
flux_topic_t *find_or_create_topic(const char *name)
{
    if (!name || name[0] == '\0')
        return NULL;

    /* Try to find existing */
    flux_topic_t *existing = find_topic_by_name(name);
    if (existing)
        return existing;

    /* Create new topic */
    if (g_agent_rt.num_topics >= FLUX_TOPIC_MAX)
        return NULL;

    flux_topic_t *tp = &g_agent_rt.topics[g_agent_rt.num_topics];
    memset(tp, 0, sizeof(flux_topic_t));

    /* Copy name */
    int i;
    for (i = 0; i < FLUX_TOPIC_MAX - 1 && name[i]; i++)
        tp->name[i] = name[i];
    tp->name[i] = '\0';

    tp->num_subscribers = 0;

    g_agent_rt.num_topics++;

    return tp;
}

/* ========================================================================
 * Internal Helpers — Published Capabilities
 * ======================================================================== */

/*
 * find_pub_cap — Find a published capability entry by name.
 *
 * Returns:
 *   Pointer to the entry, or NULL if not found.
 */
static flux_published_cap_t *find_pub_cap(const char *name)
{
    if (!name || name[0] == '\0')
        return NULL;

    for (int i = 0; i < g_agent_rt.num_pub_caps; i++) {
        if (g_agent_rt.pub_caps[i].active &&
            strcmp(g_agent_rt.pub_caps[i].name, name) == 0) {
            return &g_agent_rt.pub_caps[i];
        }
    }

    return NULL;
}

/*
 * find_pub_cap_by_agent — Find all published capabilities for an agent.
 *
 * Parameters:
 *   agent_id — Agent ID to search for
 *   results  — Output array of capability names
 *   max      — Maximum results
 *
 * Returns:
 *   Number of capabilities found.
 */
static int find_pub_caps_by_agent(uint32_t agent_id, char (*results)[32], int max)
{
    int count = 0;
    for (int i = 0; i < g_agent_rt.num_pub_caps && count < max; i++) {
        if (g_agent_rt.pub_caps[i].active &&
            g_agent_rt.pub_caps[i].agent_id == agent_id) {
            if (results) {
                memcpy(results[count], g_agent_rt.pub_caps[i].name, 32);
            }
            count++;
        }
    }
    return count;
}

/*
 * invalidate_pub_caps — Remove all published capabilities for an agent.
 *
 * Called during agent termination to clean up the registry.
 */
static void invalidate_pub_caps(uint32_t agent_id)
{
    for (int i = 0; i < g_agent_rt.num_pub_caps; i++) {
        if (g_agent_rt.pub_caps[i].agent_id == agent_id) {
            g_agent_rt.pub_caps[i].active = false;
        }
    }
}

/*
 * unsubscribe_all — Remove an agent from all topic subscriptions.
 *
 * Called during agent termination to clean up topics.
 */
static void unsubscribe_all(uint32_t agent_id)
{
    for (int i = 0; i < g_agent_rt.num_topics; i++) {
        flux_topic_t *tp = &g_agent_rt.topics[i];
        for (int j = tp->num_subscribers - 1; j >= 0; j--) {
            if (tp->subscribers[j] == agent_id) {
                /* Shift remaining subscribers down */
                for (int k = j; k < tp->num_subscribers - 1; k++) {
                    tp->subscribers[k] = tp->subscribers[k + 1];
                }
                tp->num_subscribers--;
            }
        }
    }
}

/* ========================================================================
 * Public API — Capability Publishing
 * ======================================================================== */

/*
 * flux_agent_publish_cap — Publish a capability for an agent.
 *
 * Registers a named capability with a description in the global
 * registry. Other agents can then discover this agent by searching
 * for the capability name.
 *
 * Parameters:
 *   agent_id    — Agent publishing the capability
 *   name        — Capability name (e.g., "math.optimize", "file.parse")
 *   description — Human-readable description
 *
 * Returns:
 *   FLUX_OK on success, error code on failure.
 */
flux_status_t flux_agent_publish_cap(uint32_t agent_id, const char *name,
                                      const char *description)
{
    if (!g_agent_rt.initialized)
        return FLUX_ERR_INVALID;

    if (!name || name[0] == '\0')
        return FLUX_ERR_INVALID;

    agent_lock_real();

    /* Validate agent exists */
    flux_agent_slot_t *slot = agent_slot_by_id(agent_id);
    if (!slot) {
        agent_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    /* Check if already published */
    flux_published_cap_t *existing = find_pub_cap(name);
    if (existing && existing->agent_id == agent_id) {
        /* Update description */
        if (description) {
            int i;
            for (i = 0; i < 127 && description[i]; i++)
                existing->description[i] = description[i];
            existing->description[i] = '\0';
        }
        agent_unlock();
        return FLUX_OK;
    }

    /* Check if another agent already published this name */
    if (existing) {
        agent_unlock();
        return FLUX_ERR_EXISTS;
    }

    /* Check agent's published cap count */
    if (slot->desc.num_caps >= FLUX_AGENT_MAX_CAPS) {
        agent_unlock();
        return FLUX_ERR_OVERFLOW;
    }

    /* Find a free slot in the registry */
    int reg_idx = -1;
    for (int i = 0; i < FLUX_PUB_CAPS_MAX; i++) {
        if (!g_agent_rt.pub_caps[i].active) {
            reg_idx = i;
            break;
        }
    }

    if (reg_idx < 0) {
        /* Try to compact: find gaps */
        agent_unlock();
        return FLUX_ERR_OVERFLOW;
    }

    /* Add to registry */
    flux_published_cap_t *entry = &g_agent_rt.pub_caps[reg_idx];
    memset(entry, 0, sizeof(flux_published_cap_t));

    int i;
    for (i = 0; i < 31 && name[i]; i++)
        entry->name[i] = name[i];
    entry->name[i] = '\0';

    if (description) {
        for (i = 0; i < 127 && description[i]; i++)
            entry->description[i] = description[i];
        entry->description[i] = '\0';
    }

    entry->agent_id = agent_id;
    entry->active = true;

    /* Update num_pub_caps */
    if (reg_idx >= g_agent_rt.num_pub_caps)
        g_agent_rt.num_pub_caps = reg_idx + 1;

    /* Add to agent's own capability list */
    int cap_idx = slot->desc.num_caps;
    memcpy(slot->desc.caps[cap_idx].name, entry->name, 32);
    if (description) {
        int d;
        for (d = 0; d < 127 && description[d]; d++)
            slot->desc.caps[cap_idx].description[d] = description[d];
        slot->desc.caps[cap_idx].description[d] = '\0';
    }
    slot->desc.num_caps++;

    agent_unlock();

    return FLUX_OK;
}

/*
 * flux_agent_unpublish_cap — Remove a published capability.
 *
 * Parameters:
 *   agent_id — Agent removing the capability
 *   name     — Capability name to remove
 *
 * Returns:
 *   FLUX_OK on success, error code on failure.
 */
flux_status_t flux_agent_unpublish_cap(uint32_t agent_id, const char *name)
{
    if (!g_agent_rt.initialized)
        return FLUX_ERR_INVALID;

    if (!name || name[0] == '\0')
        return FLUX_ERR_INVALID;

    agent_lock_real();

    /* Find in registry */
    flux_published_cap_t *entry = find_pub_cap(name);
    if (!entry || entry->agent_id != agent_id) {
        agent_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    /* Remove from registry */
    entry->active = false;
    memset(entry->name, 0, sizeof(entry->name));
    memset(entry->description, 0, sizeof(entry->description));
    entry->agent_id = FLUX_AGENT_ID_INVALID;

    /* Remove from agent's capability list */
    flux_agent_slot_t *slot = agent_slot_by_id(agent_id);
    if (slot) {
        for (int i = 0; i < slot->desc.num_caps; i++) {
            if (strcmp(slot->desc.caps[i].name, name) == 0) {
                /* Shift remaining caps down */
                for (int j = i; j < slot->desc.num_caps - 1; j++) {
                    slot->desc.caps[j] = slot->desc.caps[j + 1];
                }
                slot->desc.num_caps--;
                memset(&slot->desc.caps[slot->desc.num_caps], 0,
                       sizeof(slot->desc.caps[0]));
                break;
            }
        }
    }

    agent_unlock();

    return FLUX_OK;
}

/* ========================================================================
 * Public API — Discovery
 * ======================================================================== */

/*
 * flux_agent_discover — Find agents that have published a specific capability.
 *
 * Searches the published capabilities registry for entries matching
 * the given capability name (or prefix match if name ends with '*').
 *
 * Parameters:
 *   capability — Capability name to search for (or prefix with '*')
 *   results    — Output buffer for matching agent descriptors
 *   max        — Maximum number of results
 *
 * Returns:
 *   Number of matching agents found.
 */
flux_status_t flux_agent_discover(const char *capability,
                                   flux_agent_desc_t *results, int max)
{
    if (!g_agent_rt.initialized)
        return FLUX_ERR_INVALID;

    if (!capability || capability[0] == '\0')
        return FLUX_ERR_INVALID;

    agent_lock_real();

    int count = 0;
    int name_len = (int)strlen(capability);
    bool prefix_match = (name_len > 0 && capability[name_len - 1] == '*');

    for (int i = 0; i < g_agent_rt.num_pub_caps && count < max; i++) {
        if (!g_agent_rt.pub_caps[i].active)
            continue;

        bool match;
        if (prefix_match) {
            /* Prefix match: "math.*" matches "math.add", "math.optimize" */
            match = (strncmp(g_agent_rt.pub_caps[i].name, capability,
                            name_len - 1) == 0);
        } else {
            /* Exact match */
            match = (strcmp(g_agent_rt.pub_caps[i].name, capability) == 0);
        }

        if (!match)
            continue;

        /* Get the agent descriptor */
        flux_agent_slot_t *slot = agent_slot_by_id(g_agent_rt.pub_caps[i].agent_id);
        if (!slot)
            continue;

        if (results) {
            memcpy(&results[count], &slot->desc, sizeof(flux_agent_desc_t));
        }
        count++;
    }

    agent_unlock();

    return count > 0 ? FLUX_OK : FLUX_ERR_NOTFOUND;
}

/*
 * flux_agent_discover_all — List all published capabilities in the registry.
 *
 * Parameters:
 *   names   — Output buffer for capability names (32-char strings)
 *   agents  — Output buffer for agent IDs (parallel to names)
 *   descs   — Output buffer for descriptions (128-char strings, can be NULL)
 *   max     — Maximum number of entries
 *
 * Returns:
 *   Number of published capabilities found.
 */
int flux_agent_discover_all(char (*names)[32], uint32_t *agents,
                            char (*descs)[128], int max)
{
    if (!g_agent_rt.initialized || max <= 0)
        return 0;

    agent_lock_real();

    int count = 0;
    for (int i = 0; i < g_agent_rt.num_pub_caps && count < max; i++) {
        if (!g_agent_rt.pub_caps[i].active)
            continue;

        if (names) {
            memcpy(names[count], g_agent_rt.pub_caps[i].name, 32);
        }
        if (agents) {
            agents[count] = g_agent_rt.pub_caps[i].agent_id;
        }
        if (descs) {
            memcpy(descs[count], g_agent_rt.pub_caps[i].description, 128);
        }
        count++;
    }

    agent_unlock();

    return count;
}

/* ========================================================================
 * Public API — Topic Subscription
 * ======================================================================== */

/*
 * flux_agent_subscribe — Subscribe an agent to a topic.
 *
 * The agent will receive all broadcast messages sent to this topic.
 * If the topic doesn't exist, it is created automatically.
 *
 * Parameters:
 *   agent_id — Agent subscribing
 *   topic    — Topic name
 *
 * Returns:
 *   FLUX_OK on success, error code on failure.
 */
flux_status_t flux_agent_subscribe(uint32_t agent_id, const char *topic)
{
    if (!g_agent_rt.initialized)
        return FLUX_ERR_INVALID;

    if (!topic || topic[0] == '\0')
        return FLUX_ERR_INVALID;

    agent_lock_real();

    /* Validate agent exists */
    flux_agent_slot_t *slot = agent_slot_by_id(agent_id);
    if (!slot) {
        agent_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    /* Find or create topic */
    flux_topic_t *tp = find_or_create_topic(topic);
    if (!tp) {
        agent_unlock();
        return FLUX_ERR_OVERFLOW;
    }

    /* Check if already subscribed */
    for (int i = 0; i < tp->num_subscribers; i++) {
        if (tp->subscribers[i] == agent_id) {
            agent_unlock();
            return FLUX_OK; /* Already subscribed */
        }
    }

    /* Check subscription limit */
    if (tp->num_subscribers >= FLUX_TOPIC_SUBS_MAX) {
        agent_unlock();
        return FLUX_ERR_OVERFLOW;
    }

    /* Add subscription */
    tp->subscribers[tp->num_subscribers] = agent_id;
    tp->num_subscribers++;

    agent_unlock();

    return FLUX_OK;
}

/*
 * flux_agent_unsubscribe — Unsubscribe an agent from a topic.
 *
 * Parameters:
 *   agent_id — Agent unsubscribing
 *   topic    — Topic name
 *
 * Returns:
 *   FLUX_OK on success, error code on failure.
 */
flux_status_t flux_agent_unsubscribe(uint32_t agent_id, const char *topic)
{
    if (!g_agent_rt.initialized)
        return FLUX_ERR_INVALID;

    if (!topic || topic[0] == '\0')
        return FLUX_ERR_INVALID;

    agent_lock_real();

    /* Find topic */
    flux_topic_t *tp = find_topic_by_name(topic);
    if (!tp) {
        agent_unlock();
        return FLUX_ERR_NOTFOUND;
    }

    /* Find and remove subscription */
    bool found = false;
    for (int i = 0; i < tp->num_subscribers; i++) {
        if (tp->subscribers[i] == agent_id) {
            /* Shift remaining subscribers down */
            for (int j = i; j < tp->num_subscribers - 1; j++) {
                tp->subscribers[j] = tp->subscribers[j + 1];
            }
            tp->num_subscribers--;
            found = true;
            break;
        }
    }

    agent_unlock();

    return found ? FLUX_OK : FLUX_ERR_NOTFOUND;
}

/*
 * flux_agent_get_subscribers — Get all subscribers to a topic.
 *
 * Parameters:
 *   topic   — Topic name
 *   ids     — Output buffer for subscriber agent IDs
 *   max     — Maximum number of IDs
 *
 * Returns:
 *   Number of subscribers.
 */
int flux_agent_get_subscribers(const char *topic, uint32_t *ids, int max)
{
    if (!g_agent_rt.initialized || !topic)
        return 0;

    agent_lock_real();

    flux_topic_t *tp = find_topic_by_name(topic);
    if (!tp) {
        agent_unlock();
        return 0;
    }

    int count = tp->num_subscribers;
    if (count > max)
        count = max;

    if (ids) {
        memcpy(ids, tp->subscribers, count * sizeof(uint32_t));
    }

    agent_unlock();

    return count;
}

/*
 * flux_agent_get_topics — Get all active topics.
 *
 * Parameters:
 *   names — Output buffer for topic names (FLUX_TOPIC_MAX-char strings)
 *   counts — Output buffer for subscriber counts (can be NULL)
 *   max   — Maximum number of topics
 *
 * Returns:
 *   Number of active topics.
 */
int flux_agent_get_topics(char (*names)[FLUX_TOPIC_MAX], int *counts, int max)
{
    if (!g_agent_rt.initialized || max <= 0)
        return 0;

    agent_lock_real();

    int count = 0;
    for (int i = 0; i < g_agent_rt.num_topics && count < max; i++) {
        if (g_agent_rt.topics[i].num_subscribers == 0)
            continue;

        if (names) {
            memcpy(names[count], g_agent_rt.topics[i].name, FLUX_TOPIC_MAX);
        }
        if (counts) {
            counts[count] = g_agent_rt.topics[i].num_subscribers;
        }
        count++;
    }

    agent_unlock();

    return count;
}
