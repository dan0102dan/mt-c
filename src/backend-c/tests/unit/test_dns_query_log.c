#include "greatest.h"
#include "magitrickle/dns_query_log.h"
#include "magitrickle/dnswire.h"
#include <stdio.h>
#include <string.h>

TEST compact_keys_and_history(void) {
    mt_dns_query_log_t *log = mt_dns_query_log_create_sized(1000);
    ASSERT(log != NULL);
    for (size_t i = 0; i < 1000; i++) {
        char domain[64], client[48];
        snprintf(domain, sizeof(domain), "resource-%03zu.example.org", i % 200);
        snprintf(client, sizeof(client), "192.168.1.%zu", i % 20 + 1);
        mt_dns_query_log_append(log, client, domain, (uint16_t)i, i % 2 != 0, (int64_t)i);
    }
    mt_dns_query_log_stats_t stats;
    mt_dns_query_log_stats(log, &stats);
    ASSERT_EQ(1000u, stats.count);
    ASSERT_EQ(24000u, stats.entry_bytes);
    ASSERT_EQ(200u, stats.domains);
    ASSERT_EQ(20u, stats.clients);
    ASSERT(stats.allocated_bytes < 100 * 1024);
    mt_dns_query_entry_t row;
    ASSERT(mt_dns_query_log_get(log, 0, &row));
    ASSERT_EQ(1000u, row.id);
    ASSERT_EQ(999, row.time_ms);
    ASSERT_EQ(999u, row.qtype);
    ASSERT(row.tcp);
    ASSERT_STR_EQ("resource-199.example.org", row.domain);
    ASSERT_STR_EQ("192.168.1.20", row.client);
    ASSERT(mt_dns_query_log_get_id(log, 1, &row));
    ASSERT_EQ(0, row.time_ms);
    ASSERT(!mt_dns_query_log_get(log, 1000, &row));
    mt_dns_query_log_destroy(log);
    PASS();
}

TEST churn_reclaims_keys_and_compacts_pools(void) {
    enum { CAP = 257, ITERATIONS = 20000 };
    mt_dns_query_log_t *log = mt_dns_query_log_create_sized(CAP);
    ASSERT(log != NULL);
    for (size_t i = 0; i < ITERATIONS; i++) {
        char name[200], client[48];
        /* Alternating sizes and repeated keys exercise holes, reused handles,
         * collisions, shrinking and growth without disturbing ring order. */
        snprintf(name, sizeof(name), "r%zu.%s.example", i % 91,
            i % 3 ? "short" : "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz");
        snprintf(client, sizeof(client), "2001:db8::%zx", i % 71 + 1);
        mt_dns_query_log_append(log, client, name, (uint16_t)i, i % 2 != 0, (int64_t)i);
        size_t count = i + 1 < CAP ? i + 1 : CAP;
        ASSERT_EQ(count, mt_dns_query_log_size(log));
        for (size_t j = 0; j < count; j++) {
            size_t old = i - j;
            snprintf(name, sizeof(name), "r%zu.%s.example", old % 91,
                old % 3 ? "short" : "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz");
            snprintf(client, sizeof(client), "2001:db8::%zx", old % 71 + 1);
            mt_dns_query_entry_t row;
            ASSERT(mt_dns_query_log_get(log, j, &row));
            ASSERT_EQ(old + 1, row.id);
            ASSERT_EQ((int64_t)old, row.time_ms);
            ASSERT_EQ((uint16_t)old, row.qtype);
            ASSERT_EQ(old % 2 != 0, row.tcp);
            ASSERT_STR_EQ(name, row.domain);
            ASSERT_STR_EQ(client, row.client);
        }
    }
    mt_dns_query_log_stats_t before, after;
    mt_dns_query_log_stats(log, &before);
    for (size_t i = 0; i < CAP; i++) mt_dns_query_log_append(log, NULL, "one.example", 1, false, 0);
    mt_dns_query_log_stats(log, &after);
    ASSERT_EQ(1u, after.domains);
    ASSERT_EQ(1u, after.clients);
    ASSERT(after.pool_bytes < before.pool_bytes);
    uint64_t sequence = mt_dns_query_log_sequence(log);
    mt_dns_query_log_clear(log);
    mt_dns_query_log_stats(log, &after);
    ASSERT_EQ(0u, after.count);
    ASSERT_EQ(0u, after.domains);
    ASSERT_EQ(0u, after.clients);
    ASSERT_EQ(0u, after.pool_bytes);
    mt_dns_query_log_append(log, NULL, "after.example", 1, false, 0);
    mt_dns_query_entry_t row;
    ASSERT(mt_dns_query_log_get(log, 0, &row));
    ASSERT_EQ(sequence + 1, row.id);
    ASSERT_STR_EQ("unknown", row.client);
    ASSERT(!mt_dns_query_log_get_id(log, sequence, &row));
    mt_dns_query_log_destroy(log);
    PASS();
}

TEST binary_names_and_addresses_are_lossless(void) {
    mt_dns_query_log_t *log = mt_dns_query_log_create_sized(16);
    ASSERT(log != NULL);
    uint8_t name[255]; size_t pos = 0;
    const size_t lengths[] = {63, 63, 63, 61};
    for (size_t label = 0; label < 4; label++) {
        name[pos++] = (uint8_t)lengths[label];
        for (size_t i = 0; i < lengths[label]; i++) name[pos++] = (uint8_t)(i + label * 63);
    }
    name[pos++] = 0;
    ASSERT_EQ(sizeof(name), pos);
    const uint8_t v4[] = {192, 0, 2, 4};
    const uint8_t v6[] = {0,0,0,0,0,0,0,0,0,0,255,255,192,0,2,4};
    ASSERT(mt_dns_query_log_append_wire(log, v4, sizeof(v4), name, pos, 65535, false, 1));
    mt_dns_query_entry_t row;
    ASSERT(mt_dns_query_log_get(log, 0, &row));
    char expected[1024];
    ASSERT_EQ(MT_OK, mt_dns_name_to_string(name, pos, expected, sizeof(expected)));
    expected[strlen(expected) - 1] = '\0';
    ASSERT_STR_EQ(expected, row.domain);
    ASSERT_STR_EQ("192.0.2.4", row.client);
    /* The text adapter must reverse escaping, not intern the expanded text. */
    mt_dns_query_log_append(log, "::ffff:192.0.2.4", expected, 28, true, 2);
    ASSERT(mt_dns_query_log_append_wire(log, v6, sizeof(v6), name, pos, 1, false, 3));
    mt_dns_query_log_stats_t stats;
    mt_dns_query_log_stats(log, &stats);
    ASSERT_EQ(1u, stats.domains);
    ASSERT_EQ(2u, stats.clients);
    ASSERT_EQ(3u, stats.count);
    ASSERT(mt_dns_query_log_get(log, 1, &row));
    ASSERT_STR_EQ(expected, row.domain);
    ASSERT_STR_EQ("::ffff:192.0.2.4", row.client);
    mt_dns_query_log_append(log, NULL, "EXAMPLE.org.", 1, false, 4);
    mt_dns_query_log_append(log, NULL, "example.org", 1, false, 5);
    mt_dns_query_log_append(log, NULL, ".", 1, false, 6);
    ASSERT(mt_dns_query_log_get(log, 0, &row));
    ASSERT_STR_EQ(".", row.domain);
    mt_dns_query_log_stats(log, &stats);
    ASSERT_EQ(4u, stats.domains); /* Preserve case, root and original bytes. */
    mt_dns_query_log_destroy(log);
    PASS();
}

static void notified(void *ud) { (*(unsigned *)ud)++; }
TEST invalid_inputs_and_subscriber_lifetime(void) {
    ASSERT(mt_dns_query_log_create_sized(0) == NULL);
    ASSERT(mt_dns_query_log_create_sized(MT_DNS_QUERY_LOG_MAX_CAPACITY + 1) == NULL);
    mt_dns_query_log_t *log = mt_dns_query_log_create_sized(1);
    ASSERT(log != NULL);
    unsigned notifications = 0;
    int subscription = mt_dns_query_log_subscribe(log, notified, &notifications);
    ASSERT(subscription > 0);
    const char *bad[] = {NULL, "", "a..b", ".x", "trailing\\", "bad\\999", 
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.example"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++)
        mt_dns_query_log_append(log, NULL, bad[i], 1, false, 0);
    const uint8_t compressed[] = {0xc0, 0x01};
    ASSERT(!mt_dns_query_log_append_wire(log, NULL, 0, compressed, 2, 1, false, 0));
    ASSERT_EQ(0u, notifications);
    ASSERT_EQ(0u, mt_dns_query_log_size(log));
    for (size_t i = 0; i < 3000; i++) {
        char name[40]; snprintf(name, sizeof(name), "item-%zu.example", i);
        mt_dns_query_log_append(log, NULL, name, 1, false, 0);
    }
    ASSERT_EQ(3000u, notifications);
    ASSERT_EQ(1u, mt_dns_query_log_size(log));
    mt_dns_query_log_clear(log);
    ASSERT_EQ(3001u, notifications);
    mt_dns_query_log_unsubscribe(log, subscription);
    mt_dns_query_log_append(log, NULL, "last.example", 1, false, 0);
    ASSERT_EQ(3001u, notifications);
    mt_dns_query_log_destroy(log);
    PASS();
}
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(compact_keys_and_history);
    RUN_TEST(churn_reclaims_keys_and_compacts_pools);
    RUN_TEST(binary_names_and_addresses_are_lossless);
    RUN_TEST(invalid_inputs_and_subscriber_lifetime);
    GREATEST_MAIN_END();
}
