/* Reproducible microbenchmark; compile against each candidate's unmodified
 * query_log.c + wire.c. No pass/fail wall-clock threshold. */
#define _POSIX_C_SOURCE 200809L
#include "magitrickle/dns_query_log.h"
#include "magitrickle/dnswire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double seconds(void) {
    struct timespec ts; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
static size_t name_for(uint8_t name[255], unsigned id, bool long_name) {
    size_t p = 0;
    if (long_name) {
        for (unsigned label = 0; label < 4; label++) {
            size_t len = label == 3 ? 61 : 63;
            name[p++] = (uint8_t)len;
            memset(name + p, 'x', len); p += len;
        }
    } else {
        name[p++] = 24; memset(name + p, 'x', 24); p += 24;
        name[p++] = 3; memcpy(name + p, "org", 3); p += 3;
    }
    /* Four bytes are opaque DNS label bytes, not a formatting cost. */
    name[1] = (uint8_t)id; name[2] = (uint8_t)(id >> 8);
    name[3] = (uint8_t)(id >> 16); name[4] = (uint8_t)(id >> 24);
    name[p++] = 0;
    return p;
}
int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: %s byte-budget long unique iterations\n", argv[0]); return 2; }
    size_t budget = (size_t)strtoul(argv[1], NULL, 10);
    bool long_name = atoi(argv[2]) != 0, unique = atoi(argv[3]) != 0;
    unsigned iterations = (unsigned)strtoul(argv[4], NULL, 10);
    if (!iterations) return 2;
#ifdef MT_DNS_QUERY_LOG_RECORD_BYTES
    mt_dns_query_log_t *log = mt_dns_query_log_create_budget(budget);
#else
    /* Allows A/B compilation against the pre-packed, production 1 MiB journal. */
    if (budget != MT_DNS_QUERY_LOG_MEMORY_LIMIT) return 2;
    mt_dns_query_log_t *log = mt_dns_query_log_create();
#endif
    if (!log || !mt_dns_query_log_set_enabled(log, true)) return 2;
    size_t cap = mt_dns_query_log_capacity(log) * 2;
    uint8_t name[255], addr[] = {192, 0, 2, 4};
    for (size_t i = 0; i < cap; i++) {
        size_t len = name_for(name, unique ? (unsigned)i : (unsigned)(i % 200), long_name);
        if (!mt_dns_query_log_append_wire(log, addr, 4, name, len, 1, false, 0)) return 3;
    }
    double start = seconds();
    for (unsigned i = 0; i < iterations; i++) {
        size_t len = name_for(name, unique ? i + (unsigned)cap : i % 200, long_name);
        if (!mt_dns_query_log_append_wire(log, addr, 4, name, len, 1, false, i)) return 3;
    }
    double elapsed = seconds() - start;
    mt_dns_query_log_stats_t stats; mt_dns_query_log_stats(log, &stats);
    printf("%zu,%u,%u,%u,%.3f,%zu,%zu\n", budget, long_name, unique, iterations,
           elapsed * 1e9 / iterations, stats.allocated_bytes, stats.count);
    mt_dns_query_log_destroy(log);
    return 0;
}
