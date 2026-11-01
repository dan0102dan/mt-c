#define PCRE2_CODE_UNIT_WIDTH 8

#include "magitrickle/match.h"
#include "magitrickle/lookup.h"

#include <pcre2.h>
#include <stdlib.h>
#include <string.h>

#include "magitrickle/log.h"

/* PCRE2 hardening limits (decisions.md D-07). */
#define MT_REGEX_MATCH_LIMIT 1000000
#define MT_REGEX_DEPTH_LIMIT 10000

typedef enum rule_kind {
    RK_DOMAIN,
    RK_NAMESPACE,
    RK_WILDCARD,
    RK_REGEX,
    RK_NEVER, /* subnet/subnet6/unknown: IsMatch == false */
} rule_kind_t;

struct mt_rule_matcher {
    rule_kind_t kind;
    char *rule;
    size_t rule_len;
    pcre2_code *re;               /* RK_REGEX only, NULL if compile failed */
    pcre2_match_data *md;
    pcre2_match_context *mctx;
    bool compile_ok;
};

static rule_kind_t kind_of(const char *type)
{
    if (strcmp(type, MT_RULE_DOMAIN) == 0) {
        return RK_DOMAIN;
    }
    if (strcmp(type, MT_RULE_NAMESPACE) == 0) {
        return RK_NAMESPACE;
    }
    if (strcmp(type, MT_RULE_WILDCARD) == 0) {
        return RK_WILDCARD;
    }
    if (strcmp(type, MT_RULE_REGEX) == 0) {
        return RK_REGEX;
    }
    return RK_NEVER;
}

/* DNS names are ASCII (IDNs use A-labels). Do not use locale-dependent
 * tolower(), allocate in the query path, or route literal keywords via PCRE2. */
static unsigned char fold_ascii(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

/* Imported wildcard dialect: literal dots, ASCII case folding and '*'/'?'
 * operators; backslash escapes literal metacharacters in source keywords.
 * The internal @mtc: marker selects this path. Legacy go-wildcard remains
 * unchanged and no new configured rule type is introduced. */
static bool domain_wildcard_match(const char *pattern, const char *domain)
{
    const char *star = NULL, *retry = NULL;
    while (*domain) {
        if (*pattern == '*') {
            star = pattern++;
            retry = domain;
        } else if (*pattern == '?' ||
                   (unsigned char)*pattern == fold_ascii((unsigned char)*domain)) {
            pattern++;
            domain++;
        } else if (*pattern == '\\' && pattern[1] != '\0' &&
                   (unsigned char)pattern[1] == fold_ascii((unsigned char)*domain)) {
            pattern += 2;
            domain++;
        } else if (star) {
            pattern = star + 1;
            domain = ++retry;
        } else {
            return false;
        }
    }
    while (*pattern == '*') { pattern++; }
    return *pattern == '\0';
}

mt_rule_matcher_t *mt_rule_matcher_new(const char *type, const char *rule)
{
    mt_rule_matcher_t *m = calloc(1, sizeof(*m));
    if (m == NULL) {
        return NULL;
    }
    m->kind = kind_of(type);
    m->rule = strdup(rule);
    if (m->rule == NULL) {
        free(m);
        return NULL;
    }
    m->rule_len = strlen(m->rule);
    m->compile_ok = true;
    if (m->kind == RK_WILDCARD && strncmp(m->rule, "@mtc:", 5) == 0) {
        m->compile_ok = m->rule_len > 5;
        for (size_t i = 5; i < m->rule_len; i++) {
            m->rule[i] = (char)fold_ascii((unsigned char)m->rule[i]);
        }
    }

    if (m->kind == RK_REGEX) {
        int errcode = 0;
        PCRE2_SIZE erroff = 0;
        m->re = pcre2_compile((PCRE2_SPTR)rule, PCRE2_ZERO_TERMINATED,
                              PCRE2_CASELESS | PCRE2_UTF | PCRE2_UCP,
                              &errcode, &erroff, NULL);
        if (m->re == NULL) {
            PCRE2_UCHAR msg[128];
            pcre2_get_error_message(errcode, msg, sizeof(msg));
            MT_ERROR("regex compile failed: %s (pattern %s at %zu)",
                     (char *)msg, rule, (size_t)erroff);
            m->compile_ok = false;
        } else {
            m->md = pcre2_match_data_create_from_pattern(m->re, NULL);
            m->mctx = pcre2_match_context_create(NULL);
            if (m->md == NULL || m->mctx == NULL) {
                mt_rule_matcher_free(m);
                return NULL;
            }
            pcre2_set_match_limit(m->mctx, MT_REGEX_MATCH_LIMIT);
            pcre2_set_depth_limit(m->mctx, MT_REGEX_DEPTH_LIMIT);
        }
    }
    return m;
}

void mt_rule_matcher_free(mt_rule_matcher_t *m)
{
    if (m == NULL) {
        return;
    }
    if (m->md != NULL) {
        pcre2_match_data_free(m->md);
    }
    if (m->mctx != NULL) {
        pcre2_match_context_free(m->mctx);
    }
    if (m->re != NULL) {
        pcre2_code_free(m->re);
    }
    free(m->rule);
    free(m);
}

bool mt_rule_matcher_ok(const mt_rule_matcher_t *m)
{
    return m->compile_ok;
}

static bool namespace_match(const char *rule, size_t rule_len,
                            const char *domain, size_t domain_len)
{
    /* Go: domainName == rule || (domainLen >= ruleLen+1 &&
     *     domainName[domainLen-ruleLen-1] == '.' && suffix == rule) */
    if (domain_len == rule_len && memcmp(domain, rule, rule_len) == 0) {
        return true;
    }
    if (domain_len < rule_len + 1) {
        return false;
    }
    return domain[domain_len - rule_len - 1] == '.' &&
           memcmp(domain + domain_len - rule_len, rule, rule_len) == 0;
}

bool mt_rule_matcher_match(mt_rule_matcher_t *m, const char *domain)
{
    size_t domain_len = strlen(domain);
    switch (m->kind) {
    case RK_DOMAIN:
        return domain_len == m->rule_len &&
               memcmp(domain, m->rule, domain_len) == 0;
    case RK_NAMESPACE:
        return namespace_match(m->rule, m->rule_len, domain, domain_len);
    case RK_WILDCARD:
        if (strncmp(m->rule, "@mtc:", 5) == 0) {
            return m->compile_ok && domain_wildcard_match(m->rule + 5, domain);
        }
        return mt_wildcard_match(m->rule, domain);
    case RK_REGEX: {
        if (m->re == NULL) {
            return false;
        }
        int rc = pcre2_match(m->re, (PCRE2_SPTR)domain, domain_len, 0, 0,
                             m->md, m->mctx);
        return rc >= 0;
    }
    case RK_NEVER:
        return false;
    }
    return false;
}

/* ---------------- per-group index ---------------- */

/* exact-domain open-addressing hash set (FNV-1a) */
typedef struct exact_set {
    char **slots;
    size_t cap; /* power of two */
    size_t len;
} exact_set_t;

static uint64_t fnv1a(const char *s)
{
    uint64_t h = UINT64_C(1469598103934665603);
    for (const unsigned char *p = (const unsigned char *)s; *p != 0; p++) {
        h ^= *p;
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static mt_err_t exact_set_insert(exact_set_t *set, const char *s);

static mt_err_t exact_set_grow(exact_set_t *set)
{
    size_t new_cap = set->cap == 0 ? 16 : set->cap * 2;
    char **old_slots = set->slots;
    size_t old_cap = set->cap;

    set->slots = calloc(new_cap, sizeof(char *));
    if (set->slots == NULL) {
        set->slots = old_slots;
        return MT_ERR_NOMEM;
    }
    set->cap = new_cap;
    set->len = 0;
    for (size_t i = 0; i < old_cap; i++) {
        if (old_slots[i] != NULL) {
            /* re-insert existing pointer (no re-dup) */
            uint64_t h = fnv1a(old_slots[i]);
            size_t idx = (size_t)h & (set->cap - 1);
            while (set->slots[idx] != NULL) {
                idx = (idx + 1) & (set->cap - 1);
            }
            set->slots[idx] = old_slots[i];
            set->len++;
        }
    }
    free(old_slots);
    return MT_OK;
}

static mt_err_t exact_set_insert(exact_set_t *set, const char *s)
{
    if (set->cap == 0 || set->len * 4 >= set->cap * 3) {
        mt_err_t err = exact_set_grow(set);
        if (err != MT_OK) {
            return err;
        }
    }
    uint64_t h = fnv1a(s);
    size_t idx = (size_t)h & (set->cap - 1);
    while (set->slots[idx] != NULL) {
        if (strcmp(set->slots[idx], s) == 0) {
            return MT_OK; /* dedup */
        }
        idx = (idx + 1) & (set->cap - 1);
    }
    set->slots[idx] = strdup(s);
    if (set->slots[idx] == NULL) {
        return MT_ERR_NOMEM;
    }
    set->len++;
    return MT_OK;
}

static bool exact_set_contains(const exact_set_t *set, const char *s)
{
    if (set->cap == 0) {
        return false;
    }
    uint64_t h = fnv1a(s);
    size_t idx = (size_t)h & (set->cap - 1);
    while (set->slots[idx] != NULL) {
        if (strcmp(set->slots[idx], s) == 0) {
            return true;
        }
        idx = (idx + 1) & (set->cap - 1);
    }
    return false;
}

static void exact_set_clear(exact_set_t *set)
{
    for (size_t i = 0; i < set->cap; i++) {
        free(set->slots[i]);
    }
    free(set->slots);
    memset(set, 0, sizeof(*set));
}

/* Reversed-label namespace trie. Node 0 is the implicit root. Edges are
 * indexed by (parent ID, label bytes) in one open-addressing table. IDs and
 * label offsets survive realloc; no pointer into these arrays escapes.
 *
 * Each label is owned once, with no per-node allocations or per-parent
 * hash tables. Slots contain IDs, never fingerprints: parent, length AND
 * every label byte are checked, including on hash collisions. */
typedef struct ns_node {
    uint32_t parent;
    uint32_t offset;
    uint32_t length;
    uint32_t first_child, next_sibling;
    uint8_t child_count; /* exact through 8; 9 means use the hash index */
    bool terminal;
} ns_node_t;

typedef struct ns_trie {
    ns_node_t *nodes;
    char *labels;
    uint32_t *slots;
    size_t n_nodes, cap_nodes;
    size_t n_labels, cap_labels;
    size_t cap_slots;
    uint32_t first_child; /* implicit root */
    uint8_t child_count;
    bool has_empty_rule;
} ns_trie_t;

static uint32_t ns_hash(uint32_t parent, const char *label, size_t len)
{
    uint32_t hash = UINT32_C(2166136261) ^ parent;
    for (size_t i = 0; i < len; i++) {
        hash ^= (unsigned char)label[i];
        hash *= UINT32_C(16777619);
    }
    hash ^= hash >> 16;
    hash *= UINT32_C(0x7feb352d);
    hash ^= hash >> 15;
    return hash;
}

static size_t ns_slot(const ns_trie_t *t, uint32_t parent,
                       const char *label, size_t len, uint32_t hash)
{
    size_t slot = (size_t)hash & (t->cap_slots - 1);
    while (t->slots[slot] != 0) {
        const ns_node_t *node = &t->nodes[t->slots[slot] - 1];
        if (node->parent == parent && node->length == len &&
            (len == 0 || memcmp(t->labels + node->offset, label, len) == 0)) {
            break;
        }
        slot = (slot + 1) & (t->cap_slots - 1);
    }
    return slot;
}

/* Most suffix paths have only one child. Follow the packed sibling list
 * for small fanouts, avoiding a hash computation/probe for each DNS label.
 * Every edge is also indexed, so crossing the 8-child threshold is atomic
 * and needs no separate migration/allocation. */
static uint32_t ns_find(const ns_trie_t *t, uint32_t parent,
                        const char *label, size_t len)
{
    uint8_t count = parent ? t->nodes[parent - 1].child_count : t->child_count;
    if (count <= 8) {
        uint32_t child = parent ? t->nodes[parent - 1].first_child : t->first_child;
        while (child != 0) {
            const ns_node_t *node = &t->nodes[child - 1];
            if (node->length == len &&
                (len == 0 || memcmp(t->labels + node->offset, label, len) == 0)) {
                return child;
            }
            child = node->next_sibling;
        }
        return 0;
    }
    return t->slots[ns_slot(t, parent, label, len, ns_hash(parent, label, len))];
}

static mt_err_t ns_grow_slots(ns_trie_t *t)
{
    if (t->cap_slots > SIZE_MAX / 2 / sizeof(*t->slots)) {
        return MT_ERR_NOMEM;
    }
    size_t cap = t->cap_slots ? t->cap_slots * 2 : 8;
    uint32_t *slots = calloc(cap, sizeof(*slots));
    if (slots == NULL) { return MT_ERR_NOMEM; }
    for (size_t i = 0; i < t->n_nodes; i++) {
        const ns_node_t *node = &t->nodes[i];
        const char *label = node->length ? t->labels + node->offset : "";
        size_t slot = (size_t)ns_hash(node->parent, label, node->length) & (cap - 1);
        while (slots[slot] != 0) { slot = (slot + 1) & (cap - 1); }
        slots[slot] = (uint32_t)i + 1;
    }
    free(t->slots);
    t->slots = slots;
    t->cap_slots = cap;
    return MT_OK;
}

/* Checked geometric growth, including on 32-bit router targets. */
static size_t ns_capacity(size_t current, size_t needed, size_t limit, size_t initial)
{
    if (needed > limit) { return 0; }
    size_t cap = current ? current : initial;
    if (cap > limit) { cap = limit; }
    while (cap < needed) { cap = cap > limit / 2 ? limit : cap * 2; }
    return cap;
}

static mt_err_t ns_child(ns_trie_t *t, uint32_t parent,
                         const char *label, size_t len, uint32_t *out)
{
    uint32_t id = ns_find(t, parent, label, len);
    if (id != 0) { *out = id; return MT_OK; }
    /* Exhaustion is an error, never truncation, eviction, or a false success.
     * Limits describe the representation, not a small configured rule cap. */
    if (t->n_nodes >= UINT32_MAX || len > UINT32_MAX - t->n_labels) {
        return MT_ERR_NOMEM;
    }
    if (t->cap_slots == 0 || t->n_nodes >= t->cap_slots / 2) {
        mt_err_t err = ns_grow_slots(t);
        if (err != MT_OK) { return err; }
    }
    if (t->n_nodes == t->cap_nodes) {
        size_t limit = SIZE_MAX / sizeof(*t->nodes);
        if (limit > UINT32_MAX) { limit = UINT32_MAX; }
        size_t cap = ns_capacity(t->cap_nodes, t->n_nodes + 1, limit, 4);
        if (cap == 0) { return MT_ERR_NOMEM; }
        ns_node_t *nodes = realloc(t->nodes, cap * sizeof(*nodes));
        if (nodes == NULL) { return MT_ERR_NOMEM; }
        t->nodes = nodes;
        t->cap_nodes = cap;
    }
    size_t needed = t->n_labels + len;
    if (needed > t->cap_labels) {
        size_t cap = ns_capacity(t->cap_labels, needed, UINT32_MAX, 32);
        if (cap == 0) { return MT_ERR_NOMEM; }
        char *labels = realloc(t->labels, cap);
        if (labels == NULL) { return MT_ERR_NOMEM; }
        t->labels = labels;
        t->cap_labels = cap;
    }
    /* Publish an edge only after its storage is ready. A failed insertion
     * may leave nonterminal prefixes/capacity, but cannot create a match or
     * remove any previously accepted rule. Retrying the insertion is safe. */
    size_t slot = ns_slot(t, parent, label, len, ns_hash(parent, label, len));
    uint32_t *first = parent ? &t->nodes[parent - 1].first_child : &t->first_child;
    uint8_t *count = parent ? &t->nodes[parent - 1].child_count : &t->child_count;
    ns_node_t *node = &t->nodes[t->n_nodes];
    *node = (ns_node_t){.parent = parent, .offset = (uint32_t)t->n_labels,
                        .length = (uint32_t)len, .next_sibling = *first};
    if (len != 0) { memcpy(t->labels + t->n_labels, label, len); }
    t->n_labels = needed;
    t->n_nodes++;
    *out = (uint32_t)t->n_nodes;
    t->slots[slot] = *out;
    *first = *out;
    if (*count <= 8) { (*count)++; }
    return MT_OK;
}

static void ns_trie_clear(ns_trie_t *t)
{
    free(t->nodes);
    free(t->labels);
    free(t->slots);
    memset(t, 0, sizeof(*t));
}

static mt_err_t ns_trie_insert(ns_trie_t *t, const char *rule)
{
    size_t len = strlen(rule);
    if (len == 0) {
        t->has_empty_rule = true;
        return MT_OK;
    }
    uint32_t node = 0;
    const char *end = rule + len;
    while (end > rule) {
        const char *start = end;
        while (start > rule && start[-1] != '.') { start--; }
        mt_err_t err = ns_child(t, node, start, (size_t)(end - start), &node);
        if (err != MT_OK) { return err; }
        end = start > rule ? start - 1 : rule;
    }
    t->nodes[node - 1].terminal = true;
    return MT_OK;
}

static bool ns_trie_match(const ns_trie_t *t, const char *domain)
{
    size_t len = strlen(domain);
    if (t->has_empty_rule && (len == 0 || domain[len - 1] == '.')) { return true; }
    if (t->cap_slots == 0) { return false; }
    uint32_t node = 0;
    const char *end = domain + len;
    while (end > domain) {
        const char *start = end;
        while (start > domain && start[-1] != '.') { start--; }
        size_t label_len = (size_t)(end - start);
        node = ns_find(t, node, start, label_len);
        if (node == 0) { return false; }
        if (t->nodes[node - 1].terminal) { return true; }
        end = start > domain ? start - 1 : domain;
    }
    return false;
}

struct mt_matcher {
    exact_set_t exact;
    ns_trie_t ns;
    mt_rule_matcher_t **scan; /* native keyword/glob, legacy wildcard, regex */
    size_t n_scan;
};

mt_matcher_t *mt_matcher_new(void)
{
    return calloc(1, sizeof(mt_matcher_t));
}

void mt_matcher_free(mt_matcher_t *m)
{
    if (m == NULL) {
        return;
    }
    exact_set_clear(&m->exact);
    ns_trie_clear(&m->ns);
    for (size_t i = 0; i < m->n_scan; i++) {
        mt_rule_matcher_free(m->scan[i]);
    }
    free(m->scan);
    free(m);
}

mt_err_t mt_matcher_add(mt_matcher_t *m, const char *type, const char *rule)
{
    rule_kind_t kind = kind_of(type);
    switch (kind) {
    case RK_DOMAIN:
        return exact_set_insert(&m->exact, rule);
    case RK_NAMESPACE:
        return ns_trie_insert(&m->ns, rule);
    case RK_WILDCARD:
    case RK_REGEX: {
        mt_rule_matcher_t *rm = mt_rule_matcher_new(type, rule);
        if (rm == NULL) {
            return MT_ERR_NOMEM;
        }
        mt_rule_matcher_t **grown =
            realloc(m->scan, (m->n_scan + 1) * sizeof(*grown));
        if (grown == NULL) {
            mt_rule_matcher_free(rm);
            return MT_ERR_NOMEM;
        }
        m->scan = grown;
        m->scan[m->n_scan++] = rm;
        return MT_OK;
    }
    case RK_NEVER:
        return MT_OK; /* subnet/subnet6 never participate in DNS matching */
    }
    return MT_OK;
}

bool mt_matcher_match(mt_matcher_t *m, const char *domain)
{
    if (exact_set_contains(&m->exact, domain)) {
        return true;
    }
    if (ns_trie_match(&m->ns, domain)) {
        return true;
    }
    for (size_t i = 0; i < m->n_scan; i++) {
        if (mt_rule_matcher_match(m->scan[i], domain)) {
            return true;
        }
    }
    return false;
}
