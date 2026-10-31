#include "magitrickle/dnspipeline.h"

#include <stdlib.h>
#include <string.h>

#include "magitrickle/log.h"

struct mt_dns_pipeline {
    mt_cache_t *cache; /* not owned */
    uint32_t additional_ttl;
    mt_match_sink_cb sink;
    void *sink_ud;
    mt_ruleset_snapshot_t *snapshot; /* owned; loop-thread only (D-17) */
};

/* Bound both stack use and quadratic duplicate comparisons independently of
 * DNS packet size or group count. Strings/addresses passed by record handlers
 * are borrowed, so buffered actions must own those bytes until flush. */
#define ACTION_BATCH_CAP 32
#define ACTION_NAME_CAP 4096
typedef struct action_batch {
    struct {
        mt_match_action_t action;
        uint8_t addr[16];
    } entries[ACTION_BATCH_CAP];
    char names[ACTION_NAME_CAP];
    size_t count, names_used;
} action_batch_t;

static bool same_address(const mt_match_action_t *a, const uint8_t *a_addr,
                         const mt_match_action_t *b, const uint8_t *b_addr)
{
    return mt_id_equal(a->group_id, b->group_id) &&
           a->addr_len == b->addr_len &&
           memcmp(a_addr, b_addr, a->addr_len) == 0;
}

static void flush_actions(mt_dns_pipeline_t *p, action_batch_t *batch)
{
    for (size_t i = 0; i < batch->count; i++) {
        mt_match_action_t *action = &batch->entries[i].action;
        bool superseded = false;
        for (size_t j = i + 1; j < batch->count; j++) {
            if (same_address(action, batch->entries[i].addr,
                             &batch->entries[j].action, batch->entries[j].addr)) {
                superseded = true;
                break;
            }
        }
        if (superseded) { continue; }
        action->addr = batch->entries[i].addr;
        action->attempts = 1;
        for (size_t j = 0; j < i; j++) {
            if (same_address(action, action->addr,
                             &batch->entries[j].action, batch->entries[j].addr)) {
                action->attempts++;
            }
        }
        p->sink(action, p->sink_ud);
    }
    batch->count = 0;
    batch->names_used = 0;
}

static void emit_action(mt_dns_pipeline_t *p, action_batch_t *batch,
                        const mt_match_action_t *action)
{
    if (p->sink == NULL) { return; }
    if (batch == NULL) { p->sink(action, p->sink_ud); return; }
    size_t domain_len = strlen(action->record_domain) + 1;
    size_t matched_len = strlen(action->matched_name) + 1;
    size_t needed = domain_len + matched_len;
    if (batch->count == ACTION_BATCH_CAP ||
        needed > sizeof(batch->names) - batch->names_used) {
        flush_actions(p, batch);
    }
    if (needed > sizeof(batch->names)) {
        /* No truncation/drop if presentation names ever exceed this arena. */
        p->sink(action, p->sink_ud);
        return;
    }
    size_t idx = batch->count++;
    batch->entries[idx].action = *action;
    memcpy(batch->entries[idx].addr, action->addr, action->addr_len);
    char *name = batch->names + batch->names_used;
    memcpy(name, action->record_domain, domain_len);
    batch->entries[idx].action.record_domain = name;
    memcpy(name + domain_len, action->matched_name, matched_len);
    batch->entries[idx].action.matched_name = name + domain_len;
    batch->names_used += needed;
}

mt_dns_pipeline_t *mt_dns_pipeline_create(mt_cache_t *cache,
                                          uint32_t additional_ttl_seconds,
                                          mt_match_sink_cb sink,
                                          void *sink_ud)
{
    mt_dns_pipeline_t *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return NULL;
    }
    p->cache = cache;
    p->additional_ttl = additional_ttl_seconds;
    p->sink = sink;
    p->sink_ud = sink_ud;
    return p;
}

void mt_dns_pipeline_destroy(mt_dns_pipeline_t *p)
{
    if (p == NULL) {
        return;
    }
    mt_ruleset_snapshot_free(p->snapshot);
    free(p);
}

void mt_dns_pipeline_set_snapshot(mt_dns_pipeline_t *p,
                                  mt_ruleset_snapshot_t *snap)
{
    mt_ruleset_snapshot_t *old = p->snapshot;
    p->snapshot = snap;
    mt_ruleset_snapshot_free(old);
}

void mt_dns_pipeline_set_additional_ttl(mt_dns_pipeline_t *p,
                                        uint32_t additional_ttl_seconds)
{
    p->additional_ttl = additional_ttl_seconds;
}

/* Drops exactly one trailing '.' — mt_dns_name_to_string always produces
 * a fully-qualified presentation name (root included), matching Go's
 * trimFQDN(name) on the wire-derived string. */
static void trim_fqdn(char *name)
{
    size_t len = strlen(name);
    if (len > 0 && name[len - 1] == '.') {
        name[len - 1] = '\0';
    }
}

/* Does any name in the list match the group's matcher? Returns the first
 * matching name (for the log-only "matched_name" field) or NULL. */
static const char *first_match(mt_matcher_t *m, char **names, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (mt_matcher_match(m, names[i])) {
            return names[i];
        }
    }
    return NULL;
}

static void handle_address_record(mt_dns_pipeline_t *p, const mt_dns_rr_t *rr,
                                  uint8_t addr_len, const char *kind,
                                  int64_t now, action_batch_t *batch)
{
    if (rr->rdata_len != addr_len) {
        MT_WARN("unprocessable %s response (rdata len %zu)", kind,
                rr->rdata_len);
        return;
    }
    char domain[1100];
    if (mt_dns_name_to_string(rr->name, rr->name_len, domain,
                              sizeof(domain)) != MT_OK) {
        return;
    }
    trim_fqdn(domain);

    uint32_t ttl = rr->ttl + p->additional_ttl;
    mt_cache_add_address(p->cache, domain, rr->rdata, addr_len, ttl, now);

    char **names = NULL;
    size_t n_names = 0;
    if (mt_cache_get_aliases(p->cache, domain, &names, &n_names) != MT_OK) {
        return;
    }

    for (size_t i = 0; p->snapshot != NULL && i < p->snapshot->n_groups;
         i++) {
        mt_group_snapshot_t *g = p->snapshot->groups[i];
        const char *matched = first_match(g->matcher, names, n_names);
        if (matched == NULL) {
            continue;
        }
        mt_match_action_t action = {
            .group_id = g->id,
            .group_name = g->name,
            .addr = rr->rdata,
            .addr_len = addr_len,
            .ttl_seconds = ttl,
            .attempts = 1,
            .record_domain = domain,
            .matched_name = matched,
            .record_kind = kind,
        };
        emit_action(p, batch, &action);
    }
    mt_cache_free_strings(names, n_names);
}

static void handle_cname_record(mt_dns_pipeline_t *p, const mt_dns_rr_t *rr,
                                int64_t now, action_batch_t *batch)
{
    char domain[1100], target[1100];
    if (mt_dns_name_to_string(rr->name, rr->name_len, domain,
                              sizeof(domain)) != MT_OK) {
        return;
    }
    trim_fqdn(domain);
    if (mt_dns_name_to_string(rr->rdata, rr->rdata_len, target,
                              sizeof(target)) != MT_OK) {
        return;
    }
    trim_fqdn(target);

    uint32_t ttl = rr->ttl + p->additional_ttl;
    mt_cache_add_alias(p->cache, domain, target, ttl, now);

    mt_cache_addr_t *addrs = NULL;
    size_t n_addrs = 0;
    if (mt_cache_get_addresses(p->cache, domain, now, &addrs, &n_addrs) !=
        MT_OK) {
        return;
    }
    char **aliases = NULL;
    size_t n_aliases = 0;
    if (mt_cache_get_aliases(p->cache, domain, &aliases, &n_aliases) !=
        MT_OK) {
        free(addrs);
        return;
    }

    for (size_t i = 0; p->snapshot != NULL && i < p->snapshot->n_groups;
         i++) {
        mt_group_snapshot_t *g = p->snapshot->groups[i];
        const char *matched = first_match(g->matcher, aliases, n_aliases);
        if (matched == NULL) {
            continue;
        }
        for (size_t j = 0; j < n_addrs; j++) {
            int64_t remaining = addrs[j].deadline - now;
            if (remaining <= 0) {
                continue;
            }
            mt_match_action_t action = {
                .group_id = g->id,
                .group_name = g->name,
                .addr = addrs[j].addr,
                .addr_len = addrs[j].addr_len,
                .ttl_seconds = (uint32_t)remaining,
                .attempts = 1,
                .record_domain = domain,
                .matched_name = matched,
                .record_kind = "CNAME",
            };
            emit_action(p, batch, &action);
        }
    }
    mt_cache_free_strings(aliases, n_aliases);
    free(addrs);
}

void mt_dns_pipeline_handle_message(mt_dns_pipeline_t *p,
                                    const mt_dns_msg_t *msg, int64_t now)
{
    if ((msg->flags & 0x000f) != 0) {
        MT_WARN("unprocessable response (rcode=%u)", msg->flags & 0x000fu);
        return;
    }
    action_batch_t batch;
    batch.count = 0;
    batch.names_used = 0;
    /* Single-answer responses have no repeated record writes to coalesce. */
    action_batch_t *pending = msg->n_answers > 1 && p->sink != NULL ? &batch : NULL;
    for (size_t i = 0; i < msg->n_answers; i++) {
        const mt_dns_rr_t *rr = &msg->answers[i];
        switch (rr->rtype) {
        case MT_DNS_TYPE_A:
            handle_address_record(p, rr, 4, "A", now, pending);
            break;
        case MT_DNS_TYPE_AAAA:
            handle_address_record(p, rr, 16, "AAAA", now, pending);
            break;
        case MT_DNS_TYPE_CNAME:
            handle_cname_record(p, rr, now, pending);
            break;
        default:
            break;
        }
    }
    if (pending != NULL) { flush_actions(p, pending); }
}
