/* Desired-list reconciliation microbenchmark, with the real matcher/cache.
 * Usage: rules-reconcile-bench RULES DOMAINS ITERATIONS
 *
 * Compile this identical harness against the baseline and candidate ruleset.c
 * by defining MT_BENCH_RULESET_SOURCE to an absolute quoted source path.
 * Use -ffunction-sections -fdata-sections -Wl,--gc-sections and link match.c,
 * wildcard.c, cache.c, models.c, lookup.c, err.c and logging/log.c. Unused
 * ruleset lifecycle/netfilter functions are discarded; no kernel I/O occurs.
 *
 * Rules are unique namespaces. Every cached name matches one namespace and
 * owns a distinct IPv4 address. Setup is excluded from process CPU time;
 * desired-list construction, result validation and freeing are included.
 * Peak RSS includes setup. This is not an end-to-end DNS or router benchmark.
 */
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <sys/resource.h>
#include <time.h>

#ifndef MT_BENCH_RULESET_SOURCE
#define MT_BENCH_RULESET_SOURCE "../../src/backend-c/src/netfilter/ruleset.c"
#endif
#include MT_BENCH_RULESET_SOURCE

static bool parse_count(const char *text, size_t *out) {
    if (!*text) { return false; }
    for (const char *p = text; *p; p++) { if (*p < '0' || *p > '9') { return false; } }
    errno = 0;
    unsigned long long value = strtoull(text, NULL, 10);
    if (errno || value > SIZE_MAX) { return false; }
    *out = (size_t)value;
    return true;
}

static bool cpu_ns(uint64_t *out) {
    struct timespec ts;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) { return false; }
    *out = (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
    return true;
}

static void clear_lists(new4_list_t *a, new6_list_t *b) {
    mt_lookup_clear(&a->index); mt_lookup_clear(&b->index);
    free(a->items); free(b->items);
}

int main(int argc, char **argv) {
    size_t rules, domains, iterations;
    if (argc != 4 || !parse_count(argv[1], &rules) || !rules ||
        !parse_count(argv[2], &domains) || domains > UINT32_C(0xffffff) ||
        !parse_count(argv[3], &iterations) || !iterations ||
        iterations > UINT64_MAX / (domains ? domains : 1)) {
        fprintf(stderr, "usage: %s RULES(>0) DOMAINS(0..16777215) ITERATIONS(>0)\n", argv[0]);
        return 2;
    }
    mt_group_t *group = mt_group_new();
    mt_cache_t *cache = mt_cache_create(domains ? domains : 1);
    if (!group || !cache) { mt_group_free(group); mt_cache_destroy(cache); return 1; }
    group->enable = true;
    mt_log_set_level(MT_LOG_DISABLED);
    for (size_t i = 0; i < rules; i++) {
        char name[80]; snprintf(name, sizeof(name), "rule%zu.example", i);
        mt_rule_t *rule = mt_rule_new();
        if (!rule) { goto setup_error; }
        rule->type = strdup(MT_RULE_NAMESPACE); rule->rule = strdup(name); rule->enable = true;
        if (!rule->type || !rule->rule || mt_group_add_rule(group, rule) != MT_OK) {
            mt_rule_free(rule); goto setup_error;
        }
    }
    for (size_t i = 0; i < domains; i++) {
        char name[100]; snprintf(name, sizeof(name), "host%zu.rule%zu.example", i, i % rules);
        uint32_t ip = (uint32_t)(i + 1);
        uint8_t addr[] = {10, (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), (uint8_t)ip};
        mt_cache_add_address(cache, name, addr, 4, 60, 1000);
    }
    if (mt_cache_dropped(cache) || mt_cache_domain_count(cache) != domains) { goto setup_error; }

    mt_ruleset_t rs = {.group = group};
    uint64_t before, after, entries = 0, checksum = 0;
    const uint64_t expected_sum = (uint64_t)domains * ((uint64_t)domains + 1) / 2;
    if (!cpu_ns(&before)) { goto setup_error; }
    for (size_t i = 0; i < iterations; i++) {
        new4_list_t a = {0}; new6_list_t b = {0}; mt_err_t err = MT_OK;
        sync_build_new_lists(&rs, cache, 1000, &a, &b, &err);
        bool valid = err == MT_OK && a.n == domains && b.n == 0;
        uint64_t sum = 0;
        for (size_t j = 0; j < a.n && valid; j++) {
            const new4_entry_t *e = &a.items[j];
            uint32_t key = ((uint32_t)e->subnet.addr[1] << 16) |
                           ((uint32_t)e->subnet.addr[2] << 8) | e->subnet.addr[3];
            valid = e->subnet.addr[0] == 10 && e->subnet.cidr == 0 && e->has_ttl && e->ttl == 60 &&
                    key > 0 && key <= domains;
            sum += key;
        }
        entries += a.n;
        clear_lists(&a, &b);
        if (!valid || sum != expected_sum) {
            fprintf(stderr, "desired-list validation failed on iteration %zu\n", i);
            goto setup_error;
        }
        checksum ^= sum;
    }
    if (!cpu_ns(&after)) { goto setup_error; }
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) { goto setup_error; }
    printf("{\"fixture\":\"namespace_all_hit_v4\",\"rules\":%zu,\"domains\":%zu,"
           "\"iterations\":%zu,\"build_verify_clear_cpu_ns\":%" PRIu64 ","
           "\"validated_ipv4_entries\":%" PRIu64 ",\"checksum\":%" PRIu64 ","
           "\"peak_rss_kib\":%ld}\n",
           rules, domains, iterations, after - before, entries, checksum, usage.ru_maxrss);
    mt_group_free(group); mt_cache_destroy(cache);
    return 0;

setup_error:
    fprintf(stderr, "benchmark setup or reconciliation failed\n");
    mt_group_free(group); mt_cache_destroy(cache);
    return 1;
}
