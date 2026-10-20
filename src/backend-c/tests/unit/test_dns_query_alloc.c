#include "greatest.h"
#include "magitrickle/dns_query_log.h"
#include <stdlib.h>
#include <string.h>

void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t size);
void *__real_realloc(void *p, size_t size);
void *__real_memmove(void *dst, const void *src, size_t size);
void *__wrap_malloc(size_t n);
void *__wrap_calloc(size_t n, size_t size);
void *__wrap_realloc(void *p, size_t size);
void *__wrap_memmove(void *dst, const void *src, size_t size);
static bool fail_alloc, counting;
static size_t moves, reallocs;
void *__wrap_malloc(size_t n) { return fail_alloc ? NULL : __real_malloc(n); }
void *__wrap_calloc(size_t n, size_t size) { return fail_alloc ? NULL : __real_calloc(n, size); }
void *__wrap_realloc(void *p, size_t size) {
    if (counting) reallocs++;
    return fail_alloc ? NULL : __real_realloc(p, size);
}
void *__wrap_memmove(void *dst, const void *src, size_t size) {
    if (counting) moves++;
    return __real_memmove(dst, src, size);
}
static void max_name(uint8_t name[255], unsigned sequence) {
    size_t at = 0;
    for (size_t i = 0; i < 4; i++) {
        size_t n = i == 3 ? 61 : 63;
        name[at++] = (uint8_t)n; memset(name + at, 'x', n); at += n;
    }
    name[at] = 0;
    name[1] = (uint8_t)(sequence >> 16);
    name[2] = (uint8_t)(sequence >> 8);
    name[3] = (uint8_t)sequence;
}
TEST churn_does_not_move_retained_names_or_resize_pools(void) {
    const size_t capacities[] = {1000, 10000};
    for (size_t c = 0; c < 2; c++) {
        mt_dns_query_log_t *log = mt_dns_query_log_create_sized(capacities[c]); ASSERT(log);
        uint8_t name[255];
        for (size_t i = 0; i < capacities[c]; i++) {
            max_name(name, (unsigned)i);
            ASSERT(mt_dns_query_log_append_wire(log, NULL, 0, name, sizeof(name), 1, false, 0));
        }
        moves = reallocs = 0; counting = true;
        bool ok = true;
        for (size_t i = 0; i < capacities[c] * 2; i++) {
            max_name(name, (unsigned)(i + capacities[c]));
            if (!mt_dns_query_log_append_wire(log, NULL, 0, name, sizeof(name), 1, false, 0)) ok = false;
        }
        counting = false;
        ASSERT(ok); ASSERT_EQ(0u, moves); ASSERT_EQ(0u, reallocs);
        ASSERT_EQ(capacities[c], mt_dns_query_log_size(log));
        mt_dns_query_log_destroy(log);
    }
    PASS();
}
TEST allocation_failure_preserves_history_and_explicit_capture_consent(void) {
    fail_alloc = true;
    mt_dns_query_log_t *absent = mt_dns_query_log_create();
    fail_alloc = false;
    ASSERT(absent == NULL); ASSERT(!mt_dns_query_log_enabled(absent));
    mt_dns_query_log_t *off = mt_dns_query_log_create(); ASSERT(off);
    fail_alloc = true;
    bool enabled = mt_dns_query_log_set_enabled(off, true);
    fail_alloc = false;
    ASSERT(!enabled); ASSERT(!mt_dns_query_log_enabled(off));
    mt_dns_query_log_destroy(off);
    mt_dns_query_log_t *log = mt_dns_query_log_create_sized(2); ASSERT(log);
    mt_dns_query_log_append(log, "10.0.0.1", "old.example", 1, false, 1);
    mt_dns_query_log_append(log, "10.0.0.2", "last.example", 28, false, 2);
    uint8_t name[255]; max_name(name, 123);
    fail_alloc = true;
    bool appended = mt_dns_query_log_append_wire(log, NULL, 0, name, sizeof(name), 1, false, 3);
    fail_alloc = false;
    ASSERT(!appended); ASSERT_EQ(2u, mt_dns_query_log_size(log));
    ASSERT_EQ(2u, mt_dns_query_log_sequence(log));
    mt_dns_query_entry_t row; ASSERT(mt_dns_query_log_get_id(log, 1, &row));
    ASSERT_STR_EQ("old.example", row.domain);
    mt_dns_query_log_stats_t stats; mt_dns_query_log_stats(log, &stats); ASSERT_EQ(1u, stats.dropped);
    /* Repeated names need no allocation and still record under memory pressure. */
    fail_alloc = true;
    mt_dns_query_log_append(log, "10.0.0.3", "last.example", 1, false, 4);
    fail_alloc = false;
    ASSERT_EQ(3u, mt_dns_query_log_sequence(log));
    ASSERT(mt_dns_query_log_get_id(log, 3, &row)); ASSERT_STR_EQ("10.0.0.3", row.client);
    mt_dns_query_log_destroy(log); PASS();
}
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(churn_does_not_move_retained_names_or_resize_pools);
    RUN_TEST(allocation_failure_preserves_history_and_explicit_capture_consent);
    GREATEST_MAIN_END();
}
