/* Build twice with the same flags/dependencies and different MATCH_SOURCE.
 * This measures the production matcher, NOT a complete router daemon. */
#ifndef MATCH_SOURCE
#define MATCH_SOURCE "../../src/backend-c/src/rules/match.c"
#endif
#include MATCH_SOURCE
#include <errno.h>
#include <stdio.h>
#include <time.h>

static size_t payload_bytes(const mt_matcher_t *m)
{
#ifdef COMPACT_NAMESPACE
    return sizeof(*m) + m->ns.cap_nodes * sizeof(*m->ns.nodes) +
           m->ns.cap_labels + m->ns.cap_slots * sizeof(*m->ns.slots);
#else
    /* Defined below: the old representation owns nodes, labels, child vectors,
     * per-parent hash tables and a second copy of each indexed child label. */
    return sizeof(*m);
#endif
}

#ifndef COMPACT_NAMESPACE
static size_t old_tree_payload(const trie_node_t *node)
{
    size_t total = node->label ? strlen(node->label) + 1 : 0;
    total += node->cap_children * sizeof(*node->children);
    total += node->child_index.cap * sizeof(*node->child_index.entries);
    for (size_t i = 0; i < node->child_index.cap; i++) {
        const mt_lookup_entry_t *entry = &node->child_index.entries[i];
        if (entry->key) { total += entry->key_len ? entry->key_len : 1; }
    }
    for (size_t i = 0; i < node->n_children; i++) {
        total += sizeof(*node->children[i]) + old_tree_payload(node->children[i]);
    }
    return total;
}
#endif

static long status_kib(const char *field)
{
    FILE *file = fopen("/proc/self/status", "r");
    if (!file) { return -1; }
    char line[256]; long value = -1;
    size_t n = strlen(field);
    while (fgets(line, sizeof(line), file)) {
        if (!strncmp(line, field, n) && line[n] == ':') {
            if (sscanf(line + n + 1, "%ld", &value) != 1) { value = -1; }
            break;
        }
    }
    fclose(file);
    return value;
}

static double cpu_time(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) { exit(2); }
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void name_for(char *name, size_t capacity, size_t i, const char *shape, bool hit)
{
    const char *first = hit ? "host" : "miss";
    int n;
    if (!strcmp(shape, "shared")) {
        n = snprintf(name, capacity, "%s%zu.bench.example.com", first, i);
    } else if (!strcmp(shape, "diverse")) {
        n = snprintf(name, capacity, "%s%zu.site%zu.tld%zu", first, i, i, i % 31);
    } else {
        n = snprintf(name, capacity, "%s.a.b.c.d.e.id%zu.bench.example.com", first, i);
    }
    if (n < 0 || (size_t)n >= capacity) { exit(2); }
}

int main(int argc, char **argv)
{
    if (argc != 4) { fprintf(stderr, "usage: namespace-memory RULES shared|diverse|deep QUERIES\n"); return 2; }
    errno = 0; char *end;
    unsigned long raw_rules = strtoul(argv[1], &end, 10);
    if (errno || *end || raw_rules == 0 || raw_rules > 1000000) { return 2; }
    unsigned long raw_queries = strtoul(argv[3], &end, 10);
    if (errno || *end || raw_queries == 0 || raw_queries > 100000000) { return 2; }
    const char *shape = argv[2];
    if (strcmp(shape, "shared") && strcmp(shape, "diverse") && strcmp(shape, "deep")) { return 2; }
    size_t rules = (size_t)raw_rules, queries = (size_t)raw_queries;
    char query[1024][128], name[128];
    for (size_t i = 0; i < 1024; i++) {
        size_t selected = ((i / 2) * 7919) % rules;
        name_for(query[i], sizeof(query[i]), selected, shape, i % 2 == 0);
    }
    long rss_before = status_kib("VmRSS");
    double begin = cpu_time();
    mt_matcher_t *m = mt_matcher_new();
    if (!m) { return 3; }
    for (size_t i = 0; i < rules; i++) {
        name_for(name, sizeof(name), i, shape, true);
        if (mt_matcher_add(m, MT_RULE_NAMESPACE, name) != MT_OK) { return 3; }
    }
    double build_cpu = cpu_time() - begin;
    long rss_rules = status_kib("VmRSS");
    size_t payload = payload_bytes(m);
#ifndef COMPACT_NAMESPACE
    payload += old_tree_payload(&m->ns.root);
#endif
    /* Independently require every input rule to match, with no positive
     * sample selection that could hide a lost rule after table growth. */
    for (size_t i = 0; i < rules; i++) {
        name_for(name, sizeof(name), i, shape, true);
        if (!mt_matcher_match(m, name)) { return 4; }
        name_for(name, sizeof(name), i, shape, false);
        if (mt_matcher_match(m, name)) { return 4; }
    }
    size_t hits = 0;
    begin = cpu_time();
    for (size_t i = 0; i < queries; i++) { hits += mt_matcher_match(m, query[i % 1024]) ? 1u : 0u; }
    double match_cpu = cpu_time() - begin;
    if (hits != (queries + 1) / 2) { return 4; }
    long rss_queries = status_kib("VmRSS"), peak = status_kib("VmHWM");
    begin = cpu_time();
    mt_matcher_free(m);
    double free_cpu = cpu_time() - begin;
    long rss_free = status_kib("VmRSS");
    printf("{\"rules\":%zu,\"shape\":\"%s\",\"queries\":%zu,\"hits\":%zu,"
           "\"payload_bytes\":%zu,\"rss_before_kib\":%ld,\"rss_rules_kib\":%ld,"
           "\"rss_queries_kib\":%ld,\"rss_free_kib\":%ld,\"peak_kib\":%ld,"
           "\"build_cpu_ms\":%.6f,\"query_cpu_ns\":%.6f,\"free_cpu_ms\":%.6f}\n",
           rules, shape, queries, hits, payload, rss_before, rss_rules, rss_queries,
           rss_free, peak, build_cpu * 1000, match_cpu * 1e9 / (double)queries, free_cpu * 1000);
    return 0;
}
