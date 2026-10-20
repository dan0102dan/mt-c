#include "greatest.h"
#include "magitrickle/dns_query_log.h"
#include "magitrickle/dnswire.h"
#include <arpa/inet.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static mt_dns_query_log_t *enabled_log(size_t budget) {
    mt_dns_query_log_t *log = mt_dns_query_log_create_budget(budget);
    if (log && !mt_dns_query_log_set_enabled(log, true)) { mt_dns_query_log_destroy(log); return NULL; }
    return log;
}
static int64_t sample_time(size_t i) {
    switch (i % 9) {
    case 0: return INT64_MIN;
    case 1: return INT64_MAX;
    case 2: return -1;
    case 3: return 0;
    case 4: return (int64_t)i * 1000000000;
    default: return 1800000000000 + (int64_t)i;
    }
}
static size_t sample_name(size_t i, uint8_t name[255]) {
    /* Exercise every size class, arbitrary escaped bytes, short and maximal
     * names, with both repeated and unique keys. */
    size_t length = i % 17 == 0 ? 255 : 8 + (i * 37 % 247);
    size_t p = 0, left = length - 1;
    while (left) {
        size_t n = left > 64 ? 63 : left - 1;
        if (left > 64 && left - 64 == 1) { n--; }
        name[p++] = (uint8_t)n;
        for (size_t j = 0; j < n; j++) { name[p++] = (uint8_t)(i + j * 7); }
        left -= n + 1;
    }
    name[p++] = 0;
    return p;
}
static bool check_row(mt_dns_query_log_t *log, size_t index) {
    mt_dns_query_entry_t row;
    if (!mt_dns_query_log_get(log, index, &row)) return false;
    size_t i = (size_t)(row.id - 1);
    uint8_t name[255], addr[16] = {0x20, 1, 0x0d, 0xb8};
    size_t n = sample_name(i, name);
    char text[1024], ip[48];
    if (mt_dns_name_to_string(name, n, text, sizeof(text)) != MT_OK) return false;
    if (strlen(text) > 1) text[strlen(text) - 1] = 0;
    size_t len = i % 3 == 0 ? 0 : i % 3 == 1 ? 4 : 16;
    addr[2] = (uint8_t)(i >> 8); addr[3] = (uint8_t)i;
    addr[14] = (uint8_t)(i >> 8); addr[15] = (uint8_t)i;
    if (!len) strcpy(ip, "unknown");
    else if (!inet_ntop(len == 4 ? AF_INET : AF_INET6, addr, ip, sizeof(ip))) return false;
    return row.time_ms == sample_time(i) && row.qtype == (uint16_t)i &&
        row.tcp == (i % 2 != 0) && !strcmp(row.domain, text) && !strcmp(row.client, ip);
}
TEST packed_budget_survives_mixed_sizes_clock_jumps_and_eviction(void) {
    const size_t budgets[] = {32768, 65536, MT_DNS_QUERY_LOG_MEMORY_LIMIT};
    for (size_t b = 0; b < sizeof(budgets) / sizeof(*budgets); b++) {
        mt_dns_query_log_t *log = enabled_log(budgets[b]); ASSERT(log);
        for (size_t i = 0; i < 50000; i++) {
            uint8_t name[255], addr[16] = {0x20, 1, 0x0d, 0xb8};
            size_t n = sample_name(i, name);
            addr[2] = (uint8_t)(i >> 8); addr[3] = (uint8_t)i;
            addr[14] = (uint8_t)(i >> 8); addr[15] = (uint8_t)i;
            size_t len = i % 3 == 0 ? 0 : i % 3 == 1 ? 4 : 16;
            ASSERT(mt_dns_query_log_append_wire(log, addr, len, name, n, (uint16_t)i, i % 2 != 0, sample_time(i)));
            mt_dns_query_log_stats_t stats; mt_dns_query_log_stats(log, &stats);
            ASSERT_EQ(budgets[b], stats.allocated_bytes);
            ASSERT(stats.count > 0 && stats.count <= stats.capacity);
            ASSERT(check_row(log, 0)); ASSERT(check_row(log, stats.count - 1));
            if (i % 173 == 0) for (size_t j = 0; j < stats.count; j++) ASSERT(check_row(log, j));
            mt_dns_query_entry_t row;
            ASSERT(!mt_dns_query_log_get_id(log, i + 1 - stats.count, &row));
        }
        mt_dns_query_log_clear(log);
        mt_dns_query_log_stats_t stats; mt_dns_query_log_stats(log, &stats);
        ASSERT_EQ(0u, stats.count); ASSERT_EQ(0u, stats.domains); ASSERT_EQ(0u, stats.pool_bytes);
        mt_dns_query_log_destroy(log);
    }
    PASS();
}
TEST record_count_is_limited_by_bytes_not_8192_or_60000(void) {
    mt_dns_query_log_t *log = enabled_log(MT_DNS_QUERY_LOG_MEMORY_LIMIT); ASSERT(log);
    const uint8_t name[] = {1, 'a', 0}, addr[] = {192, 0, 2, 1};
    for (size_t i = 0; i < 350000; i++) {
        ASSERT(mt_dns_query_log_append_wire(log, addr, 4, name, sizeof(name), 65535, i % 2 != 0, (int64_t)i));
    }
    mt_dns_query_log_stats_t stats; mt_dns_query_log_stats(log, &stats);
    ASSERT(stats.count > 95000); ASSERT_EQ(1u, stats.domains);
    ASSERT_EQ(MT_DNS_QUERY_LOG_MEMORY_LIMIT, stats.allocated_bytes);
    for (size_t j = 0; j < stats.count; j++) {
        mt_dns_query_entry_t row; ASSERT(mt_dns_query_log_get(log, j, &row));
        ASSERT_EQ(350000 - j, row.id); ASSERT_EQ((int64_t)row.id - 1, row.time_ms);
        ASSERT_EQ(65535u, row.qtype); ASSERT_STR_EQ("a", row.domain);
        ASSERT_STR_EQ("192.0.2.1", row.client);
    }
    printf("repeated: %zu rows in %zu bytes; record encoding %u bytes\n", stats.count, stats.allocated_bytes, MT_DNS_QUERY_LOG_RECORD_BYTES);
    /* Clear/stop retain the sequence, including when starting mid-page. */
    for (unsigned cycle = 0; cycle < 2000; cycle++) {
        if (cycle % 2) { mt_dns_query_log_clear(log); }
        else { ASSERT(mt_dns_query_log_set_enabled(log, false)); ASSERT(mt_dns_query_log_set_enabled(log, true)); }
        ASSERT(mt_dns_query_log_append_wire(log, addr, 4, name, sizeof(name), 1, false, INT64_MIN));
        mt_dns_query_entry_t row; ASSERT(mt_dns_query_log_get(log, 0, &row));
        ASSERT_EQ(350001u + cycle, row.id); ASSERT_EQ(INT64_MIN, row.time_ms);
    }
    mt_dns_query_log_destroy(log); PASS();
}
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(packed_budget_survives_mixed_sizes_clock_jumps_and_eviction);
    RUN_TEST(record_count_is_limited_by_bytes_not_8192_or_60000);
    GREATEST_MAIN_END();
}
