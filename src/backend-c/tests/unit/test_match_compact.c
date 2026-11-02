/* Exercise the production index with a moving, fail-at-N allocator. No test
 * hooks or ownership changes are exposed by the public matcher interface. */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t checks, allocation_calls, fail_at, live_blocks, live_bytes;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

typedef union allocation_header {
    struct { size_t size; } data;
    max_align_t alignment;
} allocation_header_t;

static bool allocation_fails(void)
{
    allocation_calls++;
    return fail_at != 0 && allocation_calls == fail_at;
}

static void *moving_malloc(size_t size)
{
    if (allocation_fails() || size > SIZE_MAX - sizeof(allocation_header_t)) { return NULL; }
    allocation_header_t *h = malloc(sizeof(*h) + size);
    if (!h) { return NULL; }
    h->data.size = size;
    live_blocks++; live_bytes += size;
    return h + 1;
}

static void moving_free(void *ptr)
{
    if (!ptr) { return; }
    allocation_header_t *h = (allocation_header_t *)ptr - 1;
    CHECK(live_blocks != 0 && live_bytes >= h->data.size);
    live_blocks--; live_bytes -= h->data.size;
    free(h);
}

static void *moving_calloc(size_t count, size_t size)
{
    if (count && size > SIZE_MAX / count) { return NULL; }
    void *ptr = moving_malloc(count * size);
    if (ptr) { memset(ptr, 0, count * size); }
    return ptr;
}

static void *moving_realloc(void *ptr, size_t size)
{
    /* Force EVERY successful realloc to move, even when libc could extend
     * in place. This catches pointers accidentally retained across growth. */
    void *next = moving_malloc(size);
    if (!next) { return NULL; }
    if (ptr) {
        allocation_header_t *h = (allocation_header_t *)ptr - 1;
        memcpy(next, ptr, h->data.size < size ? h->data.size : size);
        moving_free(ptr);
    }
    return next;
}

static char *moving_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *copy = moving_malloc(n);
    if (copy) { memcpy(copy, s, n); }
    return copy;
}

/* Rename public definitions so the normal make test library can be linked
 * unchanged. Header prototypes are renamed too, before its first inclusion. */
#define mt_rule_matcher_new compact_rule_new
#define mt_rule_matcher_free compact_rule_free
#define mt_rule_matcher_ok compact_rule_ok
#define mt_rule_matcher_match compact_rule_match
#define mt_matcher_new compact_matcher_new
#define mt_matcher_free compact_matcher_free
#define mt_matcher_add compact_matcher_add
#define mt_matcher_match compact_matcher_match
#define malloc moving_malloc
#define calloc moving_calloc
#define realloc moving_realloc
#define strdup moving_strdup
#define free moving_free
#include "../../src/rules/match.c"
#undef malloc
#undef calloc
#undef realloc
#undef strdup
#undef free
#undef mt_rule_matcher_new
#undef mt_rule_matcher_free
#undef mt_rule_matcher_ok
#undef mt_rule_matcher_match
#undef mt_matcher_new
#undef mt_matcher_free
#undef mt_matcher_add
#undef mt_matcher_match

#define trie_node legacy_trie_node
#define trie_node_t legacy_trie_node_t
#define ns_trie legacy_ns_trie
#define ns_trie_t legacy_ns_trie_t
#define trie_find legacy_trie_find
#define trie_child legacy_trie_child
#define trie_node_clear legacy_trie_clear
#define ns_trie_insert legacy_ns_insert
#define ns_trie_match legacy_ns_match
#include "../reference/namespace_trie_v1.h"
#undef trie_node
#undef trie_node_t
#undef ns_trie
#undef ns_trie_t
#undef trie_find
#undef trie_child
#undef trie_node_clear
#undef ns_trie_insert
#undef ns_trie_match

static void clear_pair(ns_trie_t *current, legacy_ns_trie_t *old)
{
    ns_trie_clear(current);
    legacy_trie_clear(&old->root);
    CHECK(live_blocks == 0 && live_bytes == 0);
}

static void same_match(const ns_trie_t *current, const legacy_ns_trie_t *old,
                         const char *name)
{
    bool want = legacy_ns_match(old, name);
    bool got = ns_trie_match(current, name);
    if (got != want) { fprintf(stderr, "namespace mismatch for '%s'\n", name); }
    CHECK(got == want);
}

static void word(size_t code, char *buffer)
{
    size_t n = 0;
    while (code) {
        code--;
        buffer[n++] = ".aA"[code % 3];
        code /= 3;
    }
    buffer[n] = '\0';
}

static void exhaustive_small_names(void)
{
    for (size_t r = 0; r < 364; r++) { /* every word of length <=5 */
        ns_trie_t current = {0}; legacy_ns_trie_t old = {0};
        char pattern[32], name[32];
        word(r, pattern);
        CHECK(ns_trie_insert(&current, pattern) == MT_OK);
        CHECK(legacy_ns_insert(&old, pattern) == MT_OK);
        size_t calls = allocation_calls;
        CHECK(ns_trie_insert(&current, pattern) == MT_OK);
        CHECK(allocation_calls == calls); /* duplicates need no allocation */
        for (size_t q = 0; q < 1093; q++) { /* every word of length <=6 */
            word(q, name);
            same_match(&current, &old, name);
        }
        clear_pair(&current, &old);
    }
}

static uint32_t random_state = UINT32_C(0x62c41e8b);
static uint32_t next_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static void random_name(char *s, size_t capacity)
{
    const char alphabet[] = "...abcABC012-_\\\x80\xff";
    size_t length = next_random() % (capacity - 1);
    for (size_t i = 0; i < length; i++) {
        s[i] = alphabet[next_random() % (sizeof(alphabet) - 1)];
    }
    s[length] = '\0';
}

static void random_sets_and_order(void)
{
    for (size_t run = 0; run < 200; run++) {
        ns_trie_t current = {0}; legacy_ns_trie_t old = {0};
        char patterns[64][100], query[128];
        for (size_t i = 0; i < 64; i++) { random_name(patterns[i], sizeof(patterns[i])); }
        for (size_t j = 0; j < 64; j++) {
            size_t i = run % 2 ? 63 - j : j;
            CHECK(ns_trie_insert(&current, patterns[i]) == MT_OK);
            CHECK(legacy_ns_insert(&old, patterns[i]) == MT_OK);
            same_match(&current, &old, patterns[i]);
            snprintf(query, sizeof(query), "sub.%s", patterns[i]);
            same_match(&current, &old, query);
            for (size_t k = 0; k < 16; k++) {
                random_name(query, sizeof(query));
                same_match(&current, &old, query);
            }
        }
        clear_pair(&current, &old);
    }
}

static void every_large_rule_survives_growth(void)
{
    ns_trie_t current = {0}; legacy_ns_trie_t old = {0};
    char pattern[96], query[128];
    for (size_t i = 0; i < 20000; i++) {
        snprintf(pattern, sizeof(pattern), "host%zu.site%zu.tld%zu", i, i / 8, i % 31);
        CHECK(ns_trie_insert(&current, pattern) == MT_OK);
        CHECK(legacy_ns_insert(&old, pattern) == MT_OK);
        memset(pattern, 'X', sizeof(pattern)); /* index owns its label bytes */
    }
    size_t calls = allocation_calls;
    for (size_t i = 0; i < 20000; i++) {
        snprintf(pattern, sizeof(pattern), "host%zu.site%zu.tld%zu", i, i / 8, i % 31);
        CHECK(ns_trie_match(&current, pattern));
        same_match(&current, &old, pattern);
        snprintf(query, sizeof(query), "child.%s", pattern);
        CHECK(ns_trie_match(&current, query));
        same_match(&current, &old, query);
        snprintf(query, sizeof(query), "host%zu.site%zu.tld%zu", i, i / 8, (i + 1) % 31);
        CHECK(!ns_trie_match(&current, query));
        same_match(&current, &old, query);
    }
    CHECK(allocation_calls == calls); /* no query-path allocation */
    clear_pair(&current, &old);
}

static void hash_collisions_are_not_matches(void)
{
    const char *one = "lkwc7n5swv4qee", *two = "g0ixqmcbqrtcyx";
    CHECK(ns_hash(0, one, strlen(one)) == ns_hash(0, two, strlen(two)));
    ns_trie_t current = {0};
    for (size_t i = 0; i < 10; i++) {
        char filler[32];
        snprintf(filler, sizeof(filler), "filler%zu", i);
        CHECK(ns_trie_insert(&current, filler) == MT_OK);
    }
    CHECK(current.child_count == 9); /* exercise hashing, not the small fanout path */
    CHECK(ns_trie_insert(&current, one) == MT_OK);
    CHECK(ns_trie_match(&current, one));
    CHECK(!ns_trie_match(&current, two));
    CHECK(ns_trie_insert(&current, two) == MT_OK);
    CHECK(ns_trie_match(&current, one) && ns_trie_match(&current, two));
    CHECK(ns_trie_insert(&current, "shared.first") == MT_OK);
    CHECK(ns_trie_insert(&current, "other.second") == MT_OK);
    CHECK(!ns_trie_match(&current, "shared.second"));
    CHECK(!ns_trie_match(&current, "other.first"));
    ns_trie_clear(&current);
    CHECK(live_blocks == 0);
}

static void long_labels_and_deep_names(void)
{
    ns_trie_t current = {0}; legacy_ns_trie_t old = {0};
    char *long_name = malloc(70001);
    CHECK(long_name != NULL);
    memset(long_name, 'x', 70000); long_name[70000] = '\0';
    CHECK(ns_trie_insert(&current, long_name) == MT_OK);
    CHECK(legacy_ns_insert(&old, long_name) == MT_OK);
    CHECK(ns_trie_match(&current, long_name));
    same_match(&current, &old, long_name);
    long_name[69999] = 'y';
    CHECK(!ns_trie_match(&current, long_name));
    same_match(&current, &old, long_name);
    clear_pair(&current, &old);
    /* Destruction is iterative in the compact representation. */
    for (size_t i = 0; i < 70000; i++) { long_name[i] = i % 2 ? '.' : 'a'; }
    CHECK(ns_trie_insert(&current, long_name) == MT_OK);
    CHECK(ns_trie_match(&current, long_name));
    ns_trie_clear(&current);
    free(long_name);
    CHECK(live_blocks == 0 && live_bytes == 0);
}

static void failure_sweep(void)
{
    size_t successful_calls = 0, failures = 0;
    for (size_t failure = 0; failure == 0 || failure <= successful_calls; failure++) {
        allocation_calls = 0; fail_at = failure;
        mt_matcher_t *m = compact_matcher_new();
        if (!m) {
            CHECK(failure == 1); failures++;
            CHECK(live_blocks == 0); continue;
        }
        char pattern[96]; size_t inserted = 0;
        for (; inserted < 4097; inserted++) {
            snprintf(pattern, sizeof(pattern), "leaf%zu.mid%zu.root%zu", inserted, inserted, inserted % 13);
            mt_err_t err = compact_matcher_add(m, MT_RULE_NAMESPACE, pattern);
            if (err != MT_OK) {
                CHECK(err == MT_ERR_NOMEM && failure != 0);
                failures++;
                CHECK(!compact_matcher_match(m, pattern));
                /* A failed new rule must not make its proper suffix match. */
                CHECK(!compact_matcher_match(m, strchr(pattern, '.') + 1));
                break;
            }
        }
        if (failure == 0) { successful_calls = allocation_calls; }
        fail_at = 0;
        for (size_t i = 0; i < inserted; i++) {
            snprintf(pattern, sizeof(pattern), "leaf%zu.mid%zu.root%zu", i, i, i % 13);
            CHECK(compact_matcher_match(m, pattern));
        }
        if (inserted < 4097) {
            snprintf(pattern, sizeof(pattern), "leaf%zu.mid%zu.root%zu", inserted, inserted, inserted % 13);
            CHECK(compact_matcher_add(m, MT_RULE_NAMESPACE, pattern) == MT_OK);
            CHECK(compact_matcher_match(m, pattern));
        }
        compact_matcher_free(m);
        CHECK(live_blocks == 0 && live_bytes == 0);
    }
    fail_at = 0;
    CHECK(failures == successful_calls);
    printf("allocation failure sites checked: %zu\n", failures);
}

static void checked_arithmetic(void)
{
    CHECK(ns_capacity(0, 1, UINT32_MAX, 4) == 4);
    CHECK(ns_capacity(16, 17, 31, 4) == 31);
    CHECK(ns_capacity(16, 32, 31, 4) == 0);
    CHECK(ns_capacity(SIZE_MAX / 2 + 1, SIZE_MAX, SIZE_MAX, 4) == SIZE_MAX);
    ns_trie_t current = {.n_nodes = UINT32_MAX};
    uint32_t id = 0;
    CHECK(ns_child(&current, 0, "x", 1, &id) == MT_ERR_NOMEM);
    current.n_nodes = 0; current.n_labels = UINT32_MAX;
    CHECK(ns_child(&current, 0, "x", 1, &id) == MT_ERR_NOMEM);
    current.n_labels = 0; current.cap_slots = SIZE_MAX / sizeof(uint32_t);
    CHECK(ns_grow_slots(&current) == MT_ERR_NOMEM);
    CHECK(live_blocks == 0);
}

static void mixed_rules_keep_their_semantics(void)
{
    const char *types[] = {MT_RULE_DOMAIN, MT_RULE_NAMESPACE, MT_RULE_WILDCARD,
        MT_RULE_WILDCARD, MT_RULE_REGEX, MT_RULE_REGEX, MT_RULE_SUBNET, MT_RULE_SUBNET6, "unknown"};
    const char *patterns[] = {"exact.example", "zone.example", "legacy?.test",
        "@mtc:*imported.example", "^re[0-9]+[.]example$", "[", "10.0.0.0/8", "fd00::/8", "*"};
    mt_rule_matcher_t *rules[9];
    mt_matcher_t *m = compact_matcher_new();
    CHECK(m != NULL);
    for (size_t i = 0; i < 9; i++) {
        rules[i] = compact_rule_new(types[i], patterns[i]);
        CHECK(rules[i] != NULL);
        CHECK(compact_matcher_add(m, types[i], patterns[i]) == MT_OK);
    }
    const char *names[] = {"exact.example", "EXACT.example", "child.exact.example",
        "zone.example", "child.zone.example", "notzone.example", "ZONE.example",
        "legacyX.test", "legacyXAtest", "legacy.test", "IMPORTED.EXAMPLE", "notimportedXexample",
        "re42.example", "RE42.EXAMPLE", "other", "10.0.0.0/8", "fd00::/8", "", "["};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        bool expected = false;
        for (size_t j = 0; j < 9; j++) { expected |= compact_rule_match(rules[j], names[i]); }
        CHECK(compact_matcher_match(m, names[i]) == expected);
    }
    for (size_t i = 0; i < 9; i++) { compact_rule_free(rules[i]); }
    compact_matcher_free(m);
    CHECK(live_blocks == 0 && live_bytes == 0);
}

static void compact_storage_bound(void)
{
    ns_trie_t current = {0};
    char name[64];
    for (size_t i = 0; i < 100000; i++) {
        snprintf(name, sizeof(name), "host%zu.bench.example.com", i);
        CHECK(ns_trie_insert(&current, name) == MT_OK);
    }
    CHECK(current.n_nodes == 100003);
    /* Counts requested payload bytes, not RSS or allocator metadata. */
    CHECK(live_bytes < 64u * 100000u);
    CHECK(live_blocks == 3); /* nodes, labels, slots: no allocation per rule */
    ns_trie_clear(&current);
    CHECK(live_blocks == 0 && live_bytes == 0);
}

int main(void)
{
    mt_log_set_level(MT_LOG_DISABLED);
    exhaustive_small_names();
    random_sets_and_order();
    every_large_rule_survives_growth();
    hash_collisions_are_not_matches();
    long_labels_and_deep_names();
    failure_sweep();
    checked_arithmetic();
    mixed_rules_keep_their_semantics();
    compact_storage_bound();
    printf("PASS: 9 compact matcher test groups, %zu checks\n", checks);
    return 0;
}
