#include "greatest.h"
#include "magitrickle/dns_query_log.h"
#include <stdio.h>
#include <string.h>

TEST disabled_capture_does_not_store_or_allocate_history(void) {
    mt_dns_query_log_t *log = mt_dns_query_log_create(); ASSERT(log);
    mt_dns_query_log_stats_t empty, after;
    mt_dns_query_log_stats(log, &empty);
    ASSERT(!mt_dns_query_log_enabled(log));
    ASSERT_EQ(0u, empty.entry_bytes); ASSERT_EQ(0u, empty.index_bytes); ASSERT_EQ(0u, empty.pool_bytes);
    for (int i = 0; i < 100; i++) mt_dns_query_log_append(log, "192.0.2.1", "example.com", 1, false, i);
    mt_dns_query_log_stats(log, &after);
    ASSERT_EQ(0u, after.count); ASSERT_EQ(empty.allocated_bytes, after.allocated_bytes);
    ASSERT_EQ(0u, mt_dns_query_log_sequence(log));
    ASSERT(mt_dns_query_log_set_enabled(log, true));
    for (int i = 0; i < 4000; i++) mt_dns_query_log_append(log, "192.0.2.1", "example.com", 1, false, i);
    ASSERT_EQ(4000u, mt_dns_query_log_size(log));
    ASSERT(mt_dns_query_log_set_enabled(log, true)); /* Idempotent, not clear. */
    ASSERT_EQ(4000u, mt_dns_query_log_size(log));
    ASSERT(mt_dns_query_log_set_enabled(log, false));
    mt_dns_query_log_stats(log, &after);
    ASSERT_EQ(empty.allocated_bytes, after.allocated_bytes);
    ASSERT_EQ(0u, after.entry_bytes); ASSERT_EQ(0u, after.index_bytes); ASSERT_EQ(0u, after.pool_bytes);
    ASSERT(mt_dns_query_log_set_enabled(log, false));
    mt_dns_query_log_append(log, "192.0.2.1", "ignored.example", 65, false, 4001);
    ASSERT_EQ(4000u, mt_dns_query_log_sequence(log));
    ASSERT(mt_dns_query_log_set_enabled(log, true));
    mt_dns_query_log_append(log, "192.0.2.1", "new.example", 65, false, 4002);
    mt_dns_query_entry_t row; ASSERT(mt_dns_query_log_get(log, 0, &row));
    ASSERT_EQ(4001u, row.id); ASSERT_STR_EQ("new.example", row.domain);
    mt_dns_query_log_destroy(log);
    log = mt_dns_query_log_create(); ASSERT(log); ASSERT(!mt_dns_query_log_enabled(log));
    mt_dns_query_log_destroy(log); PASS();
}
TEST byte_pressure_evicts_oldest_and_never_exceeds_one_mib(void) {
    mt_dns_query_log_t *log = mt_dns_query_log_create(); ASSERT(log);
    ASSERT(mt_dns_query_log_set_enabled(log, true));
    for (size_t i = 0; i < 24000; i++) {
        uint8_t name[255], addr[16] = {0x20, 1, 0x0d, 0xb8}; size_t p = 0;
        for (size_t label = 0; label < 4; label++) {
            size_t length = label == 3 ? 61 : 63;
            name[p++] = (uint8_t)length; memset(name + p, 'x', length); p += length;
        }
        name[p++] = 0; name[1] = (uint8_t)(i >> 8); name[2] = (uint8_t)i;
        addr[14] = (uint8_t)(i >> 8); addr[15] = (uint8_t)i;
        ASSERT(mt_dns_query_log_append_wire(log, addr, 16, name, p, 28, true, (int64_t)i));
        mt_dns_query_log_stats_t stats; mt_dns_query_log_stats(log, &stats);
        ASSERT(stats.allocated_bytes <= MT_DNS_QUERY_LOG_MEMORY_LIMIT);
        ASSERT(stats.count > 0 && stats.count <= stats.capacity);
        mt_dns_query_entry_t row;
        ASSERT(mt_dns_query_log_get(log, stats.count - 1, &row));
        ASSERT_EQ(i + 2 - stats.count, row.id);
        ASSERT(mt_dns_query_log_get(log, 0, &row)); ASSERT_EQ(i + 1, row.id);
    }
    mt_dns_query_log_stats_t stats; mt_dns_query_log_stats(log, &stats);
    ASSERT(stats.count > 1000 && stats.count < stats.capacity);
    printf("budget: %zu rows, %zu / %zu bytes\n", stats.count, stats.allocated_bytes, MT_DNS_QUERY_LOG_MEMORY_LIMIT);
    ASSERT(mt_dns_query_log_set_enabled(log, false));
    mt_dns_query_log_stats(log, &stats); ASSERT_EQ(0u, stats.pool_bytes);
    mt_dns_query_log_destroy(log); PASS();
}
static void on_change(void *ud) { (*(unsigned *)ud)++; }
TEST observing_never_starts_capture_and_clear_keeps_capture_state(void) {
    mt_dns_query_log_t *log = mt_dns_query_log_create(); ASSERT(log);
    unsigned changes = 0; int subscription = mt_dns_query_log_subscribe(log, on_change, &changes);
    ASSERT(subscription > 0); ASSERT(!mt_dns_query_log_enabled(log));
    mt_dns_query_log_clear(log); ASSERT(!mt_dns_query_log_enabled(log));
    ASSERT(mt_dns_query_log_set_enabled(log, true));
    mt_dns_query_log_append(log, NULL, "example.org", 1, false, 1);
    mt_dns_query_log_clear(log); ASSERT(mt_dns_query_log_enabled(log));
    ASSERT_EQ(0u, mt_dns_query_log_size(log));
    ASSERT(mt_dns_query_log_set_enabled(log, false));
    ASSERT_EQ(5u, changes);
    mt_dns_query_log_unsubscribe(log, subscription); mt_dns_query_log_destroy(log); PASS();
}
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(disabled_capture_does_not_store_or_allocate_history);
    RUN_TEST(byte_pressure_evicts_oldest_and_never_exceeds_one_mib);
    RUN_TEST(observing_never_starts_capture_and_clear_keeps_capture_state);
    GREATEST_MAIN_END();
}
