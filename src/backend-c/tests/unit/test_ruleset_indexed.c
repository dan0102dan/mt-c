/* Compile the production translation unit into this test, excluding its .o
 * in the Makefile. This exercises private reconciliation helpers without
 * exporting test-only APIs or needing a router's netfilter kernel modules. */
#include "greatest.h"
#include "fake_ipset_nl.h"
#include "magitrickle/match.h"
#include "magitrickle/dns_cache.h"

#include <stdlib.h>
#include <string.h>

/* Observe only calls from ruleset.c. The real matcher/cache implementations
 * are linked normally; this keeps failure injection out of production and
 * does not require linker-wrapper flags in the build. */
static struct {
    size_t creates, adds, matches, singles, reads, lists, grows;
    size_t live, max_live, current_rules, max_rules;
    size_t fail_create, fail_add, fail_single, fail_read, fail_grow;
    bool fail_list;
} observed;

static mt_matcher_t *observed_matcher_new(void) {
    if (++observed.creates == observed.fail_create) { return NULL; }
    mt_matcher_t *m = mt_matcher_new();
    if (m) {
        observed.live++;
        if (observed.live > observed.max_live) { observed.max_live = observed.live; }
        observed.current_rules = 0;
    }
    return m;
}
static void observed_matcher_free(mt_matcher_t *m) {
    if (m) { observed.live--; observed.current_rules = 0; }
    mt_matcher_free(m);
}
static mt_err_t observed_matcher_add(mt_matcher_t *m, const char *type, const char *rule) {
    if (++observed.adds == observed.fail_add) { return MT_ERR_NOMEM; }
    mt_err_t err = mt_matcher_add(m, type, rule);
    if (err == MT_OK && ++observed.current_rules > observed.max_rules) {
        observed.max_rules = observed.current_rules;
    }
    return err;
}
static bool observed_matcher_match(mt_matcher_t *m, const char *name) {
    observed.matches++;
    return mt_matcher_match(m, name);
}
static mt_rule_matcher_t *observed_single_new(const char *type, const char *rule) {
    if (++observed.singles == observed.fail_single) { return NULL; }
    return mt_rule_matcher_new(type, rule);
}
static mt_err_t observed_addresses(mt_cache_t *cache, const char *name, int64_t now,
                                    mt_cache_addr_t **out, size_t *n) {
    if (++observed.reads == observed.fail_read) { *out = NULL; *n = 0; return MT_ERR_NOMEM; }
    return mt_cache_get_addresses(cache, name, now, out, n);
}
static mt_err_t observed_known(mt_cache_t *cache, char ***out, size_t *n) {
    observed.lists++;
    if (observed.fail_list) { *out = NULL; *n = 0; return MT_ERR_NOMEM; }
    return mt_cache_list_known_domains(cache, out, n);
}
static void *observed_realloc(void *ptr, size_t size) {
    if (++observed.grows == observed.fail_grow) { return NULL; }
    return realloc(ptr, size);
}

#define mt_matcher_new observed_matcher_new
#define mt_matcher_free observed_matcher_free
#define mt_matcher_add observed_matcher_add
#define mt_matcher_match observed_matcher_match
#define mt_rule_matcher_new observed_single_new
#define mt_cache_get_addresses observed_addresses
#define mt_cache_list_known_domains observed_known
#define realloc observed_realloc
#include "../../src/netfilter/ruleset.c"
#undef realloc
#undef mt_cache_list_known_domains
#undef mt_cache_get_addresses
#undef mt_rule_matcher_new
#undef mt_matcher_match
#undef mt_matcher_add
#undef mt_matcher_free
#undef mt_matcher_new

/* Frozen rule-major builder from 6a94817a. Keep independent of the new
 * collection/index helpers: it is the compatibility oracle, including legacy
 * namespace matching, TTL selection and first-insertion order. */
static void reference_build_new_lists(mt_ruleset_t *rs, mt_cache_t *cache, int64_t now,
                                 new4_list_t *new4, new6_list_t *new6, mt_err_t *err) {
    char **known = NULL;
    size_t n_known = 0;
    *err = mt_cache_list_known_domains(cache, &known, &n_known);
    if (*err != MT_OK) { return; }

    const mt_group_t *g = rs->group;
    for (size_t i = 0; i < g->n_rules && *err == MT_OK; i++) {
        const mt_rule_t *rule = g->rules[i];
        if (!rule->enable) { continue; }

        if (strcmp(rule->type, MT_RULE_SUBNET) == 0) {
            mt_ipv4_subnet_t subnet;
            if (!parse_ipv4_rule(rule->rule, &subnet)) { continue; }
            if (subnet.cidr == 0) {
                /* TODO: matches Go's dirty hack for a full 0.0.0.0/0 rule,
                 * splitting into two /1 halves; see decisions.md. */
                mt_ipv4_subnet_t a = {.addr = {0x00, 0, 0, 0}, .cidr = 1};
                mt_ipv4_subnet_t b = {.addr = {0x80, 0, 0, 0}, .cidr = 1};
                *err = new4_upsert(new4, a, false, 0, false);
                if (*err == MT_OK) { *err = new4_upsert(new4, b, false, 0, false); }
            } else {
                *err = new4_upsert(new4, subnet, false, 0, false);
            }
            continue;
        }

        if (strcmp(rule->type, MT_RULE_SUBNET6) == 0) {
            mt_ipv6_subnet_t subnet;
            if (!parse_ipv6_rule(rule->rule, &subnet)) { continue; }
            if (subnet.cidr == 0) {
                mt_ipv6_subnet_t a = {0};
                a.cidr = 1;
                mt_ipv6_subnet_t b = {0};
                b.addr[0] = 0x80;
                b.cidr = 1;
                *err = new6_upsert(new6, a, false, 0, false);
                if (*err == MT_OK) { *err = new6_upsert(new6, b, false, 0, false); }
            } else {
                *err = new6_upsert(new6, subnet, false, 0, false);
            }
            continue;
        }

        mt_rule_matcher_t *rm = mt_rule_matcher_new(rule->type, rule->rule);
        if (!rm) {
            *err = MT_ERR_NOMEM;
            continue;
        }
        for (size_t k = 0; k < n_known && *err == MT_OK; k++) {
            if (!mt_rule_matcher_match(rm, known[k])) { continue; }

            mt_cache_addr_t *addrs = NULL;
            size_t n_addrs = 0;
            *err = mt_cache_get_addresses(cache, known[k], now, &addrs, &n_addrs);
            if (*err != MT_OK) { break; }

            for (size_t a = 0; a < n_addrs; a++) {
                int64_t remain = addrs[a].deadline - now;
                if (remain <= 0) { continue; }
                uint32_t ttl = (uint32_t)remain;
                if (addrs[a].addr_len == 4) {
                    mt_ipv4_subnet_t subnet = {.cidr = 0};
                    memcpy(subnet.addr, addrs[a].addr, 4);
                    *err = new4_upsert(new4, subnet, true, ttl, true);
                } else if (addrs[a].addr_len == 16) {
                    mt_ipv6_subnet_t subnet = {.cidr = 0};
                    memcpy(subnet.addr, addrs[a].addr, 16);
                    *err = new6_upsert(new6, subnet, true, ttl, true);
                }
                if (*err != MT_OK) { break; }
            }
            free(addrs);
        }
        mt_rule_matcher_free(rm);
    }

    mt_cache_free_strings(known, n_known);
}

static void lists_clear(new4_list_t *a, new6_list_t *b) {
    free(a->items); free(b->items); mt_lookup_clear(&a->index); mt_lookup_clear(&b->index);
}

static bool add_rule(mt_group_t *g, const char *type, const char *pattern, bool enabled) {
    mt_rule_t *r = mt_rule_new();
    if (!r) { return false; }
    r->type = strdup(type); r->rule = strdup(pattern); r->enable = enabled;
    if (!r->type || !r->rule || mt_group_add_rule(g, r) != MT_OK) {
        mt_rule_free(r); return false;
    }
    return true;
}

static void cache4(mt_cache_t *cache, const char *name, uint8_t last, uint32_t ttl, int64_t now) {
    const uint8_t addr[] = {192, 0, 2, last};
    mt_cache_add_address(cache, name, addr, 4, ttl, now);
}
static void cache6(mt_cache_t *cache, const char *name, uint8_t last, uint32_t ttl, int64_t now) {
    uint8_t addr[16] = {0x20, 1, 0x0d, 0xb8}; addr[15] = last;
    mt_cache_add_address(cache, name, addr, 16, ttl, now);
}

static size_t position4(const new4_list_t *list, mt_ipv4_subnet_t key) {
    unsigned char bytes[5]; size_t pos = SIZE_MAX;
    memcpy(bytes, key.addr, 4); bytes[4] = key.cidr;
    (void)mt_lookup_get(&list->index, bytes, sizeof(bytes), &pos);
    return pos;
}
static size_t position6(const new6_list_t *list, mt_ipv6_subnet_t key) {
    unsigned char bytes[17]; size_t pos = SIZE_MAX;
    memcpy(bytes, key.addr, 16); bytes[16] = key.cidr;
    (void)mt_lookup_get(&list->index, bytes, sizeof(bytes), &pos);
    return pos;
}

/* Different addresses may be inserted in a different order. For one address,
 * preserve the ordered sequence of prefixes as well as the final values:
 * cidr=0 and explicit /32 or /128 are separate desired keys but the same
 * kernel host. An unordered set comparison would miss a TTL/permanent flip. */
static int equivalent_lists(const new4_list_t *want4, const new6_list_t *want6,
                            const new4_list_t *got4, const new6_list_t *got6) {
    ASSERT_EQ(want4->n, got4->n); ASSERT_EQ(want6->n, got6->n);
    ASSERT_EQ(got4->n, got4->index.len); ASSERT_EQ(got6->n, got6->index.len);
    for (size_t i = 0; i < want4->n; i++) {
        size_t pos = position4(got4, want4->items[i].subnet);
        ASSERT(pos < got4->n);
        ASSERT_EQ(want4->items[i].has_ttl, got4->items[pos].has_ttl);
        ASSERT_EQ(want4->items[i].ttl, got4->items[pos].ttl);
        for (size_t j = 0; j < i; j++) {
            if (!memcmp(want4->items[j].subnet.addr, want4->items[i].subnet.addr, 4)) {
                ASSERT(position4(got4, want4->items[j].subnet) < pos);
            }
        }
    }
    for (size_t i = 0; i < want6->n; i++) {
        size_t pos = position6(got6, want6->items[i].subnet);
        ASSERT(pos < got6->n);
        ASSERT_EQ(want6->items[i].has_ttl, got6->items[pos].has_ttl);
        ASSERT_EQ(want6->items[i].ttl, got6->items[pos].ttl);
        for (size_t j = 0; j < i; j++) {
            if (!memcmp(want6->items[j].subnet.addr, want6->items[i].subnet.addr, 16)) {
                ASSERT(position6(got6, want6->items[j].subnet) < pos);
            }
        }
    }
    PASS();
}

static int compare_builders(mt_group_t *group, mt_cache_t *cache, int64_t now) {
    new4_list_t want4 = {0}, got4 = {0}; new6_list_t want6 = {0}, got6 = {0};
    mt_ruleset_t rs = {.group = group}; mt_err_t err = MT_OK;
    reference_build_new_lists(&rs, cache, now, &want4, &want6, &err);
    ASSERT_EQ(MT_OK, err);
    memset(&observed, 0, sizeof(observed));
    sync_build_new_lists(&rs, cache, now, &got4, &got6, &err);
    ASSERT_EQ(MT_OK, err); ASSERT_EQ(0u, observed.live);
    ASSERT(observed.max_live <= 1); ASSERT(observed.max_rules <= 4096);
    CHECK_CALL(equivalent_lists(&want4, &want6, &got4, &got6));
    lists_clear(&want4, &want6); lists_clear(&got4, &got6);
    PASS();
}

static mt_err_t reference_sync(mt_ruleset_t *rs, mt_cache_t *cache, int64_t now) {
    if (!rs->enabled || !rs->group->enable) { return MT_OK; }
    new4_list_t a = {0}; new6_list_t b = {0}; mt_err_t err = MT_OK;
    reference_build_new_lists(rs, cache, now, &a, &b, &err);
    if (err == MT_OK) { sync_diff_v4(rs, &a, &err); }
    if (err == MT_OK) { sync_diff_v6(rs, &b, &err); }
    lists_clear(&a, &b);
    return err;
}

static mt_group_t *mixed_group(void) {
    mt_group_t *g = mt_group_new();
    if (!g) { return NULL; }
    g->enable = true;
    const struct { const char *type, *pattern; bool enabled; } rules[] = {
        {MT_RULE_NAMESPACE, "example", true},
        {MT_RULE_DOMAIN, "alias", true},
        {MT_RULE_DOMAIN, "expired-alias", true},
        {MT_RULE_DOMAIN, "cycle-a", true},
        {MT_RULE_SUBNET, "192.0.2.1/32", true},
        {MT_RULE_SUBNET6, "2001:db8::1/128", true},
        {MT_RULE_REGEX, "HIGH", true},
        {MT_RULE_NAMESPACE, ".example", true},
        {MT_RULE_WILDCARD, "a?b", true},
        {MT_RULE_WILDCARD, "@mtc:*.imported", true},
        {MT_RULE_DOMAIN, "disabled", false},
        {MT_RULE_SUBNET, "192.0.2.12", false},
        {MT_RULE_SUBNET, "203.0.113.42/24", true},
        {MT_RULE_SUBNET6, "2001:db8:1::ff/64", true},
        {MT_RULE_SUBNET, "0.0.0.0/0", true},
        {MT_RULE_SUBNET6, "::/0", true},
        {MT_RULE_SUBNET, "bad/55", true},
        {MT_RULE_SUBNET6, "192.0.2.99", true},
        {MT_RULE_REGEX, "[", true},
        {"unknown", "*", true},
    };
    for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
        if (!add_rule(g, rules[i].type, rules[i].pattern, rules[i].enabled)) {
            mt_group_free(g); return NULL;
        }
    }
    return g;
}

static mt_cache_t *mixed_cache(void) {
    mt_cache_t *cache = mt_cache_create(0);
    if (!cache) { return NULL; }
    cache4(cache, "low.example", 1, 40, 1000);
    cache4(cache, "high.example", 1, 400, 1000);
    cache6(cache, "low.example", 1, 50, 1000);
    cache6(cache, "high.example", 1, 500, 1000);
    cache4(cache, "high.example", 2, 20, 1000); /* expired by the sync */
    cache4(cache, "aXb", 3, 90, 1000);
    cache4(cache, "WWW.IMPORTED", 4, 80, 1000);
    cache4(cache, "disabled", 12, 100, 1000);
    cache4(cache, "target", 5, 70, 1000);
    cache6(cache, "target", 5, 75, 1000);
    mt_cache_add_alias(cache, "alias", "middle", 100, 1000);
    mt_cache_add_alias(cache, "middle", "target", 100, 1000);
    mt_cache_add_alias(cache, "expired-alias", "disabled", 20, 1000);
    mt_cache_add_alias(cache, "cycle-a", "cycle-b", 100, 1000);
    mt_cache_add_alias(cache, "cycle-b", "cycle-a", 100, 1000);
    return cache;
}
TEST fifty_thousand_subnet_entries_are_indexed_and_ordered(void) {
    new4_list_t a = {0}; new6_list_t b = {0};
    for (uint32_t i = 0; i < 50000; i++) {
        mt_ipv4_subnet_t key = {.addr = {10, (uint8_t)(i >> 8), (uint8_t)i, 0}, .cidr = 24};
        ASSERT_EQ(MT_OK, new4_upsert(&a, key, true, 60, false));
        ASSERT_EQ(MT_OK, new4_upsert(&a, key, true, 30, true));
        ASSERT_EQ(MT_OK, new4_upsert(&a, key, true, 90, true));
    }
    ASSERT_EQ(50000u, a.n); ASSERT_EQ(50000u, a.index.len);
    for (size_t i = 0; i < a.n; i++) {
        ASSERT_EQ((uint8_t)(i >> 8), a.items[i].subnet.addr[1]);
        ASSERT_EQ(90u, a.items[i].ttl);
    }
    ASSERT_EQ(MT_OK, new4_upsert(&a, a.items[0].subnet, false, 0, false));
    ASSERT_EQ(MT_OK, new4_upsert(&a, a.items[0].subnet, true, 999, true));
    ASSERT(!a.items[0].has_ttl);
    mt_ipv6_subnet_t six; ASSERT(parse_ipv6_rule("2001:db8::ffff/64", &six));
    ASSERT_EQ(MT_OK, new6_upsert(&b, six, true, 60, false));
    ASSERT_EQ(MT_OK, new6_upsert(&b, six, true, 120, true)); ASSERT_EQ(1u, b.n);
    ASSERT_EQ(120u, b.items[0].ttl); ASSERT_EQ(0u, b.items[0].subnet.addr[15]);
    lists_clear(&a, &b); PASS();
}
TEST indexed_diff_preserves_permanent_entries_and_updates_ttls(void) {
    mt_fake_ipset_nl_t *fake = mt_fake_ipset_nl_new(); ASSERT(fake);
    mt_ipset_t *ipset = mt_ipset_new(mt_fake_ipset_nl_as_transport(fake), "test"); ASSERT(ipset);
    ASSERT_EQ(MT_OK, mt_ipset_enable(ipset));
    mt_group_t group = {.name = "test", .enable = true};
    mt_ruleset_t rs = {.group = &group, .ipset = ipset, .enabled = true};
    mt_ipv4_subnet_t permanent = {.addr = {10, 0, 0, 0}, .cidr = 24};
    mt_ipv4_subnet_t timed = {.addr = {10, 1, 0, 0}, .cidr = 24};
    mt_ipv4_subnet_t obsolete = {.addr = {10, 2, 0, 0}, .cidr = 24};
    uint32_t ttl = 60;
    /* Simulate the kernel LIST representation of a permanent entry:
     * no timeout attribute, rather than the fake's literal ADD timeout=0. */
    mt_ipset_nl_t *transport = mt_fake_ipset_nl_as_transport(fake);
    ASSERT_EQ(MT_OK, transport->ops->add(transport, mt_ipset_name4(ipset), permanent.addr,
                                        4, permanent.cidr, false, 0, true));
    ASSERT_EQ(MT_OK, mt_ipset_add4(ipset, timed, &ttl));
    ASSERT_EQ(MT_OK, mt_ipset_add4(ipset, obsolete, NULL));
    size_t before; (void)mt_fake_ipset_nl_calls(fake, &before);
    new4_list_t a = {0}; new6_list_t b = {0};
    ASSERT_EQ(MT_OK, new4_upsert(&a, permanent, true, 120, false));
    ASSERT_EQ(MT_OK, new4_upsert(&a, timed, true, 90, false));
    mt_err_t err = MT_OK; sync_diff_v4(&rs, &a, &err); ASSERT_EQ(MT_OK, err);
    size_t count; const fake_ipset_call_t *calls = mt_fake_ipset_nl_calls(fake, &count);
    unsigned adds = 0, deletes = 0;
    for (size_t i = before; i < count; i++) {
        if (calls[i].kind == FAKE_IPSET_CALL_ADD) {
            adds++; ASSERT_EQ(1u, calls[i].ip[1]); ASSERT_EQ(90u, calls[i].timeout);
        }
        if (calls[i].kind == FAKE_IPSET_CALL_DEL) { deletes++; ASSERT_EQ(2u, calls[i].ip[1]); }
    }
    ASSERT_EQ(1u, adds); ASSERT_EQ(1u, deletes);
    /* A lower requested TTL does not shorten an existing entry. */
    a.items[1].ttl = 10;
    (void)mt_fake_ipset_nl_calls(fake, &before);
    sync_diff_v4(&rs, &a, &err); ASSERT_EQ(MT_OK, err);
    calls = mt_fake_ipset_nl_calls(fake, &count);
    for (size_t i = before; i < count; i++) { ASSERT_EQ(FAKE_IPSET_CALL_LIST, calls[i].kind); }
    lists_clear(&a, &b); mt_ipset_free(ipset); PASS();
}
TEST mixed_rules_keep_alias_expiry_max_ttl_and_static_entries(void) {
    mt_group_t *g = mixed_group(); mt_cache_t *cache = mixed_cache();
    ASSERT(g && cache); CHECK_CALL(compare_builders(g, cache, 1020));
    ASSERT_EQ(1u, observed.singles); /* the leading-dot namespace fallback */

    mt_ruleset_t rs = {.group = g}; new4_list_t a = {0}; new6_list_t b = {0};
    mt_err_t err = MT_OK; sync_build_new_lists(&rs, cache, 1020, &a, &b, &err);
    ASSERT_EQ(MT_OK, err);
    mt_ipv4_subnet_t shared4 = {.addr = {192, 0, 2, 1}, .cidr = 0};
    mt_ipv6_subnet_t shared6 = {.addr = {0x20, 1, 0x0d, 0xb8}, .cidr = 0};
    shared6.addr[15] = 1;
    size_t p4 = position4(&a, shared4), p6 = position6(&b, shared6);
    ASSERT(p4 < a.n && p6 < b.n);
    ASSERT_EQ(380u, a.items[p4].ttl); ASSERT_EQ(480u, b.items[p6].ttl);
    shared4.cidr = 32; shared6.cidr = 128;
    ASSERT(position4(&a, shared4) > p4); ASSERT(position6(&b, shared6) > p6);
    ASSERT_FALSE(a.items[position4(&a, shared4)].has_ttl);
    ASSERT_FALSE(b.items[position6(&b, shared6)].has_ttl);
    shared4.cidr = 0; shared4.addr[3] = 5;
    p4 = position4(&a, shared4); ASSERT(p4 < a.n); ASSERT_EQ(50u, a.items[p4].ttl);
    shared4.addr[3] = 2; ASSERT_EQ(SIZE_MAX, position4(&a, shared4));
    shared4.addr[3] = 12; ASSERT_EQ(SIZE_MAX, position4(&a, shared4));
    ASSERT_EQ(8u, a.n); ASSERT_EQ(6u, b.n);
    lists_clear(&a, &b); mt_group_free(g); mt_cache_destroy(cache); PASS();
}

TEST namespace_edge_cases_keep_single_rule_semantics(void) {
    const char *patterns[] = {"", ".", "..", "...", "example", ".example", "..example",
                              "example.", "example..", "a..example", ".a..example.",
                              "EXAMPLE", "a", "a.", ".a", "a.."};
    const char *names[] = {"", ".", "..", "...", "example", ".example", "..example",
                           "x.example", "x..example", "x...example", "example.",
                           "example..", "x.example.", "a..example", "x.a..example",
                           ".a..example.", "x..a..example.", "EXAMPLE", "x.EXAMPLE",
                           "a", ".a", "a.", "a..", "x.a..", "x.a", "other"};
    mt_cache_t *cache = mt_cache_create(0); ASSERT(cache);
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        cache4(cache, names[i], (uint8_t)(i + 1), 100, 1000);
        cache6(cache, names[i], (uint8_t)(i + 1), 200, 1000);
    }
    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        mt_group_t *g = mt_group_new(); ASSERT(g);
        ASSERT(add_rule(g, MT_RULE_NAMESPACE, patterns[i], true));
        CHECK_CALL(compare_builders(g, cache, 1001));
        ASSERT_EQ(patterns[i][0] == '.' ? 1u : 0u, observed.singles);
        mt_group_free(g);
    }
    /* Explicit guard against using the aggregate trie's broader match. */
    mt_group_t *g = mt_group_new(); ASSERT(g);
    ASSERT(add_rule(g, MT_RULE_NAMESPACE, ".example", true));
    mt_ruleset_t rs = {.group = g}; new4_list_t a = {0}; new6_list_t b = {0};
    mt_err_t err = MT_OK; sync_build_new_lists(&rs, cache, 1001, &a, &b, &err);
    ASSERT_EQ(MT_OK, err); ASSERT_EQ(6u, a.n); ASSERT_EQ(6u, b.n);
    mt_ipv4_subnet_t key = {.addr = {192, 0, 2, 5}, .cidr = 0};
    ASSERT_EQ(SIZE_MAX, position4(&a, key)); /* "example" */
    key.addr[3] = 8; ASSERT_EQ(SIZE_MAX, position4(&a, key)); /* "x.example" */
    key.addr[3] = 6; ASSERT(position4(&a, key) < a.n); /* ".example" */
    key.addr[3] = 9; ASSERT(position4(&a, key) < a.n); /* "x..example" */
    lists_clear(&a, &b); mt_group_free(g); mt_cache_destroy(cache); PASS();
}

TEST static_host_keys_keep_first_insertion_and_last_kernel_write_order(void) {
    mt_cache_t *cache = mt_cache_create(0); ASSERT(cache);
    cache4(cache, "a.example", 7, 60, 1000); cache6(cache, "a.example", 7, 60, 1000);
    cache4(cache, "b.example", 7, 600, 1000); cache6(cache, "b.example", 7, 600, 1000);
    cache4(cache, "x..example", 7, 900, 1000); cache6(cache, "x..example", 7, 900, 1000);
    const struct { const char *type, *text; } choices[] = {
        {MT_RULE_DOMAIN, "a.example"}, {MT_RULE_DOMAIN, "b.example"},
        {MT_RULE_SUBNET, "192.0.2.7"}, {MT_RULE_SUBNET6, "2001:db8::7"},
        {MT_RULE_NAMESPACE, ".example"},
    };
    /* Every placement of the two static rules relative to the three dynamic
     * rules, including static-before-dynamic and dynamic-before-static. */
    for (size_t shift = 0; shift < 5; shift++) {
        for (size_t reverse = 0; reverse < 2; reverse++) {
            mt_group_t *g = mt_group_new(); ASSERT(g);
            for (size_t i = 0; i < 5; i++) {
                size_t pos = (shift + (reverse ? 4 - i : i)) % 5;
                ASSERT(add_rule(g, choices[pos].type, choices[pos].text, true));
            }
            /* Repeating a key must not move its original insertion position. */
            ASSERT(add_rule(g, MT_RULE_SUBNET, "192.0.2.7/32", true));
            ASSERT(add_rule(g, MT_RULE_SUBNET6, "2001:db8::7/128", true));
            CHECK_CALL(compare_builders(g, cache, 1010));
            mt_group_free(g);
        }
    }
    mt_cache_destroy(cache); PASS();
}

static mt_group_t *large_group(size_t count) {
    mt_group_t *g = mt_group_new();
    if (!g) { return NULL; }
    g->enable = true;
    for (size_t i = 0; i < count; i++) {
        char name[64];
        snprintf(name, sizeof(name), "rule%zu.example", i);
        if (!add_rule(g, MT_RULE_NAMESPACE, name, true)) { mt_group_free(g); return NULL; }
    }
    return g;
}

TEST aggregate_blocks_are_bounded_and_merge_across_both_thresholds(void) {
    mt_group_t *g = large_group(8197); mt_cache_t *cache = mt_cache_create(0);
    ASSERT(g && cache);
    const size_t hits[] = {0, 4095, 4096, 8191, 8192, 8196};
    for (size_t i = 0; i < sizeof(hits) / sizeof(hits[0]); i++) {
        char name[64]; snprintf(name, sizeof(name), "www.rule%zu.example", hits[i]);
        cache4(cache, name, 1, (uint32_t)(600 - i * 40), 1000);
        cache6(cache, name, 1, (uint32_t)(100 + i * 80), 1000);
    }
    CHECK_CALL(compare_builders(g, cache, 1000));
    ASSERT_EQ(3u, observed.creates); ASSERT_EQ(8197u, observed.adds);
    ASSERT_EQ(4096u, observed.max_rules); ASSERT_EQ(1u, observed.max_live);
    ASSERT_EQ(18u, observed.matches); ASSERT_EQ(6u, observed.reads);
    /* A later block may add a different matching name for the same IP; retain
     * max TTL across blocks and the static key's position between them. */
    ASSERT(add_rule(g, MT_RULE_SUBNET, "192.0.2.1/32", true));
    ASSERT(add_rule(g, MT_RULE_SUBNET6, "2001:db8::1/128", true));
    ASSERT(add_rule(g, MT_RULE_NAMESPACE, ".rule8196.example", true));
    cache4(cache, "x..rule8196.example", 1, 1200, 1000);
    cache6(cache, "x..rule8196.example", 1, 1300, 1000);
    ASSERT(add_rule(g, MT_RULE_DOMAIN, "later", true));
    cache4(cache, "later", 1, 1400, 1000); cache6(cache, "later", 1, 1500, 1000);
    CHECK_CALL(compare_builders(g, cache, 1010));
    ASSERT_EQ(4u, observed.creates); ASSERT_EQ(4096u, observed.max_rules);
    ASSERT_EQ(1u, observed.singles);
    mt_group_free(g); mt_cache_destroy(cache); PASS();
}

TEST empty_cache_skips_dynamic_compilation_but_keeps_static_rules(void) {
    mt_group_t *g = large_group(8197); mt_cache_t *cache = mt_cache_create(0);
    ASSERT(g && cache);
    ASSERT(add_rule(g, MT_RULE_REGEX, "[", true));
    ASSERT(add_rule(g, MT_RULE_NAMESPACE, ".example", true));
    ASSERT(add_rule(g, MT_RULE_SUBNET, "192.0.2.1", true));
    ASSERT(add_rule(g, MT_RULE_SUBNET6, "::/0", true));
    CHECK_CALL(compare_builders(g, cache, 1000));
    ASSERT_EQ(0u, observed.creates); ASSERT_EQ(0u, observed.adds);
    ASSERT_EQ(0u, observed.singles); ASSERT_EQ(0u, observed.matches);
    ASSERT_EQ(0u, observed.reads);
    mt_group_free(g); mt_cache_destroy(cache); PASS();
}

TEST disabled_groups_and_runtime_disabled_rulesets_are_noops(void) {
    mt_group_t *g = mixed_group(); mt_cache_t *cache = mixed_cache();
    ASSERT(g && cache);
    mt_fake_ipset_nl_t *fake = mt_fake_ipset_nl_new(); ASSERT(fake);
    mt_ipset_t *ipset = mt_ipset_new(mt_fake_ipset_nl_as_transport(fake), "disabled"); ASSERT(ipset);
    ASSERT_EQ(MT_OK, mt_ipset_enable(ipset));
    mt_ruleset_t rs = {.group = g, .ipset = ipset};
    size_t before, after; (void)mt_fake_ipset_nl_calls(fake, &before);
    memset(&observed, 0, sizeof(observed));
    ASSERT_EQ(MT_OK, mt_ruleset_sync(&rs, cache, 1020));
    rs.enabled = true; g->enable = false;
    ASSERT_EQ(MT_OK, mt_ruleset_sync(&rs, cache, 1020));
    (void)mt_fake_ipset_nl_calls(fake, &after);
    ASSERT_EQ(before, after); ASSERT_EQ(0u, observed.lists);
    /* Priority/source affect route ordering, not population of this set. */
    g->enable = true; g->priority = MT_PRIORITY_MIN; g->from_subscription = true;
    CHECK_CALL(compare_builders(g, cache, 1020));
    g->priority = MT_PRIORITY_MAX; g->from_subscription = false;
    CHECK_CALL(compare_builders(g, cache, 1020));
    mt_ipset_free(ipset); mt_group_free(g); mt_cache_destroy(cache); PASS();
}

static uint32_t random_next(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *state = x;
    return x;
}

TEST seeded_mixed_configs_match_the_rule_major_reference(void) {
    const char *names[] = {"", ".", "..", "example", ".example", "x.example",
        "x..example", "x...example", "example.", "a..example", "x.a..example",
        "x.a..example.", "EXAMPLE", "X.Example", "aXb", "a.b", "www.IMPORTED",
        "literal*imported", "high", "low", "other", "target", "alias", "cycle-a",
        "cycle-b", "x.example..", "...example.", "a", "a.", "x.a.."};
    const struct { const char *type, *pattern; } specs[] = {
        {MT_RULE_NAMESPACE, "example"}, {MT_RULE_NAMESPACE, ".example"},
        {MT_RULE_NAMESPACE, ""}, {MT_RULE_NAMESPACE, "example."},
        {MT_RULE_NAMESPACE, "a..example"}, {MT_RULE_NAMESPACE, "a..example."},
        {MT_RULE_NAMESPACE, ".."}, {MT_RULE_NAMESPACE, "a.."},
        {MT_RULE_DOMAIN, "x.example"}, {MT_RULE_DOMAIN, "EXAMPLE"},
        {MT_RULE_DOMAIN, "alias"}, {MT_RULE_DOMAIN, "cycle-a"},
        {MT_RULE_DOMAIN, ""}, {MT_RULE_DOMAIN, "target"},
        {MT_RULE_WILDCARD, "a.b"}, {MT_RULE_WILDCARD, "*.example"},
        {MT_RULE_WILDCARD, "?*"}, {MT_RULE_WILDCARD, "@mtc:*.imported"},
        {MT_RULE_WILDCARD, "@mtc:literal\\*imported"}, {MT_RULE_WILDCARD, "@mtc:"},
        {MT_RULE_REGEX, "HIGH|EXAMPLE"}, {MT_RULE_REGEX, "^a.*b$"},
        {MT_RULE_REGEX, "["}, {MT_RULE_REGEX, "^$"},
        {MT_RULE_SUBNET, "192.0.2.1"}, {MT_RULE_SUBNET, "192.0.2.1/32"},
        {MT_RULE_SUBNET, "192.0.2.7/24"}, {MT_RULE_SUBNET, "0.0.0.0/0"},
        {MT_RULE_SUBNET6, "2001:db8::1"}, {MT_RULE_SUBNET6, "2001:db8::1/128"},
        {MT_RULE_SUBNET6, "2001:db8::7/64"}, {MT_RULE_SUBNET6, "::/0"},
        {MT_RULE_SUBNET, "::1"}, {MT_RULE_SUBNET6, "bad/900"},
        {"unknown", "*"},
    };
    const size_t n_names = sizeof(names) / sizeof(names[0]);
    const size_t n_specs = sizeof(specs) / sizeof(specs[0]);
    uint32_t seed = UINT32_C(0x719fac03);
    for (size_t round = 0; round < 300; round++) {
        mt_cache_t *cache = mt_cache_create(0); mt_group_t *g = mt_group_new();
        ASSERT(cache && g);
        for (size_t i = 0; i < n_names; i++) {
            uint32_t choice = random_next(&seed);
            uint32_t ttl = random_next(&seed) % 240;
            uint8_t key = (uint8_t)(1 + random_next(&seed) % 8);
            if (choice & 1u) { cache4(cache, names[i], key, ttl, 1000); }
            if (choice & 2u) { cache6(cache, names[i], key, ttl / 2, 1000); }
            if (choice & 4u) {
                const char *target = names[random_next(&seed) % n_names];
                mt_cache_add_alias(cache, names[i], target, ttl, 1000);
            }
            if (choice & 8u) {
                const char *target = names[random_next(&seed) % n_names];
                mt_cache_add_alias(cache, names[i], target, ttl / 2, 1000);
            }
        }
        ASSERT_EQ(0u, mt_cache_dropped(cache));
        for (size_t i = 0; i < 48; i++) {
            size_t pos = random_next(&seed) % n_specs;
            ASSERT(add_rule(g, specs[pos].type, specs[pos].pattern,
                            random_next(&seed) % 5 != 0));
        }
        CHECK_CALL(compare_builders(g, cache, 1000 + (int64_t)(random_next(&seed) % 80)));
        mt_group_free(g); mt_cache_destroy(cache);
    }
    PASS();
}

static bool same_call(const fake_ipset_call_t *a, const fake_ipset_call_t *b) {
    return a->kind == b->kind && !strcmp(a->name, b->name) && a->iplen == b->iplen &&
           a->cidr == b->cidr && !memcmp(a->ip, b->ip, a->iplen) &&
           a->has_timeout == b->has_timeout && a->timeout == b->timeout && a->replace == b->replace;
}

static int equivalent_calls(const fake_ipset_call_t *want, size_t n_want,
                            const fake_ipset_call_t *got, size_t n_got) {
    ASSERT_EQ(n_want, n_got);
    size_t *positions = calloc(n_want ? n_want : 1, sizeof(*positions)); ASSERT(positions);
    for (size_t i = 0; i < n_want; i++) {
        positions[i] = SIZE_MAX;
        for (size_t j = 0; j < n_got; j++) {
            if (!same_call(&want[i], &got[j])) { continue; }
            bool used = false;
            for (size_t k = 0; k < i; k++) { if (positions[k] == j) { used = true; } }
            if (!used) { positions[i] = j; break; }
        }
        ASSERT(positions[i] < n_got);
        if (want[i].kind != FAKE_IPSET_CALL_ADD && want[i].kind != FAKE_IPSET_CALL_DEL) { continue; }
        for (size_t k = 0; k < i; k++) {
            if (want[k].kind == FAKE_IPSET_CALL_LIST || want[k].iplen != want[i].iplen) { continue; }
            if (!memcmp(want[k].ip, want[i].ip, want[i].iplen)) {
                ASSERT(positions[k] < positions[i]);
            }
        }
    }
    free(positions); PASS();
}

TEST public_sync_keeps_kernel_diff_and_host_write_order(void) {
    mt_group_t *g = mixed_group(); mt_cache_t *cache = mixed_cache(); ASSERT(g && cache);
    mt_fake_ipset_nl_t *fakes[2] = {mt_fake_ipset_nl_new(), mt_fake_ipset_nl_new()};
    mt_ipset_t *sets[2]; mt_ruleset_t rs[2]; size_t before[2];
    for (size_t i = 0; i < 2; i++) {
        ASSERT(fakes[i]);
        sets[i] = mt_ipset_new(mt_fake_ipset_nl_as_transport(fakes[i]), "sync"); ASSERT(sets[i]);
        ASSERT_EQ(MT_OK, mt_ipset_enable(sets[i]));
        rs[i] = (mt_ruleset_t){.group = g, .ipset = sets[i], .enabled = true};
        mt_ipv4_subnet_t old4 = {.addr = {192, 0, 2, 1}, .cidr = 0};
        mt_ipv6_subnet_t old6 = {.addr = {0x20, 1, 0x0d, 0xb8}, .cidr = 0}; old6.addr[15] = 1;
        uint32_t short_ttl = 10, long_ttl = 2000;
        ASSERT_EQ(MT_OK, mt_ipset_add4(sets[i], old4, &short_ttl));
        ASSERT_EQ(MT_OK, mt_ipset_add6(sets[i], old6, &long_ttl));
        old4.addr[3] = 250; old6.addr[15] = 250;
        ASSERT_EQ(MT_OK, mt_ipset_add4(sets[i], old4, NULL));
        ASSERT_EQ(MT_OK, mt_ipset_add6(sets[i], old6, NULL));
        (void)mt_fake_ipset_nl_calls(fakes[i], &before[i]);
    }
    memset(&observed, 0, sizeof(observed));
    ASSERT_EQ(MT_OK, reference_sync(&rs[0], cache, 1020));
    ASSERT_EQ(MT_OK, mt_ruleset_sync(&rs[1], cache, 1020));
    size_t count[2]; const fake_ipset_call_t *calls[2];
    for (size_t i = 0; i < 2; i++) { calls[i] = mt_fake_ipset_nl_calls(fakes[i], &count[i]); }
    CHECK_CALL(equivalent_calls(calls[0] + before[0], count[0] - before[0],
                                calls[1] + before[1], count[1] - before[1]));
    mt_ipset_entry4_t *a[2]; mt_ipset_entry6_t *b[2]; size_t na[2], nb[2];
    for (size_t i = 0; i < 2; i++) {
        ASSERT_EQ(MT_OK, mt_ipset_list4(sets[i], &a[i], &na[i]));
        ASSERT_EQ(MT_OK, mt_ipset_list6(sets[i], &b[i], &nb[i]));
    }
    ASSERT_EQ(na[0], na[1]); ASSERT_EQ(nb[0], nb[1]);
    for (size_t i = 0; i < na[0]; i++) {
        size_t pos = 0;
        while (pos < na[1] && (a[0][i].subnet.cidr != a[1][pos].subnet.cidr ||
               memcmp(a[0][i].subnet.addr, a[1][pos].subnet.addr, 4))) { pos++; }
        ASSERT(pos < na[1]); ASSERT_EQ(a[0][i].has_timeout, a[1][pos].has_timeout);
        ASSERT_EQ(a[0][i].timeout, a[1][pos].timeout);
    }
    for (size_t i = 0; i < nb[0]; i++) {
        size_t pos = 0;
        while (pos < nb[1] && (b[0][i].subnet.cidr != b[1][pos].subnet.cidr ||
               memcmp(b[0][i].subnet.addr, b[1][pos].subnet.addr, 16))) { pos++; }
        ASSERT(pos < nb[1]); ASSERT_EQ(b[0][i].has_timeout, b[1][pos].has_timeout);
        ASSERT_EQ(b[0][i].timeout, b[1][pos].timeout);
    }
    for (size_t i = 0; i < 2; i++) { free(a[i]); free(b[i]); mt_ipset_free(sets[i]); }
    mt_group_free(g); mt_cache_destroy(cache); PASS();
}

TEST allocation_failure_after_a_complete_block_does_not_touch_ipsets(void) {
    mt_group_t *g = large_group(4097); mt_cache_t *cache = mt_cache_create(0); ASSERT(g && cache);
    ASSERT(add_rule(g, MT_RULE_NAMESPACE, ".other", true));
    cache4(cache, "rule0.example", 1, 100, 1000); cache6(cache, "rule0.example", 1, 100, 1000);
    cache4(cache, "rule4096.example", 2, 200, 1000); cache6(cache, "rule4096.example", 2, 200, 1000);
    for (size_t failure = 0; failure < 10; failure++) {
        mt_fake_ipset_nl_t *fake = mt_fake_ipset_nl_new(); ASSERT(fake);
        mt_ipset_t *ipset = mt_ipset_new(mt_fake_ipset_nl_as_transport(fake), "atomic"); ASSERT(ipset);
        ASSERT_EQ(MT_OK, mt_ipset_enable(ipset));
        mt_ipv4_subnet_t existing = {.addr = {203, 0, 113, 0}, .cidr = 24};
        ASSERT_EQ(MT_OK, mt_ipset_add4(ipset, existing, NULL));
        mt_ruleset_t rs = {.group = g, .ipset = ipset, .enabled = true};
        size_t before, after; (void)mt_fake_ipset_nl_calls(fake, &before);
        memset(&observed, 0, sizeof(observed));
        switch (failure) {
        case 0: observed.fail_list = true; break;
        case 1: observed.fail_create = 1; break;
        case 2: observed.fail_add = 1; break;
        case 3: observed.fail_grow = 1; break;
        case 4: observed.fail_grow = 2; break;
        case 5: observed.fail_read = 1; break;
        case 6: observed.fail_create = 2; break;
        case 7: observed.fail_add = 4097; break;
        case 8: observed.fail_read = 2; break;
        case 9: observed.fail_single = 1; break;
        }
        ASSERT_EQ(MT_ERR_NOMEM, mt_ruleset_sync(&rs, cache, 1001));
        (void)mt_fake_ipset_nl_calls(fake, &after);
        ASSERT_EQ(before, after); /* not even LIST, let alone ADD/DEL */
        ASSERT_EQ(0u, observed.live); ASSERT(observed.max_live <= 1);
        ASSERT(observed.max_rules <= 4096);
        ASSERT_EQ(1u, mt_fake_ipset_nl_entry_count(fake, mt_ipset_name4(ipset)));
        ASSERT_EQ(0u, mt_fake_ipset_nl_entry_count(fake, mt_ipset_name6(ipset)));
        /* Recovery must not retain a partial matcher/list from the failed run. */
        memset(&observed, 0, sizeof(observed));
        ASSERT_EQ(MT_OK, mt_ruleset_sync(&rs, cache, 1001));
        ASSERT_EQ(0u, observed.live);
        ASSERT_EQ(2u, mt_fake_ipset_nl_entry_count(fake, mt_ipset_name4(ipset)));
        ASSERT_EQ(2u, mt_fake_ipset_nl_entry_count(fake, mt_ipset_name6(ipset)));
        mt_ipset_free(ipset);
    }
    mt_group_free(g); mt_cache_destroy(cache); PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    mt_log_set_level(MT_LOG_DISABLED);
    RUN_TEST(fifty_thousand_subnet_entries_are_indexed_and_ordered);
    RUN_TEST(indexed_diff_preserves_permanent_entries_and_updates_ttls);
    RUN_TEST(mixed_rules_keep_alias_expiry_max_ttl_and_static_entries);
    RUN_TEST(namespace_edge_cases_keep_single_rule_semantics);
    RUN_TEST(static_host_keys_keep_first_insertion_and_last_kernel_write_order);
    RUN_TEST(aggregate_blocks_are_bounded_and_merge_across_both_thresholds);
    RUN_TEST(empty_cache_skips_dynamic_compilation_but_keeps_static_rules);
    RUN_TEST(disabled_groups_and_runtime_disabled_rulesets_are_noops);
    RUN_TEST(seeded_mixed_configs_match_the_rule_major_reference);
    RUN_TEST(public_sync_keeps_kernel_diff_and_host_write_order);
    RUN_TEST(allocation_failure_after_a_complete_block_does_not_touch_ipsets);
    GREATEST_MAIN_END();
}
