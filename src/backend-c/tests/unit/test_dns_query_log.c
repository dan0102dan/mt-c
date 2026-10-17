#include "greatest.h"

#include <string.h>

#include "magitrickle/dns_query_log.h"

TEST captures_requests_newest_first(void)
{
    mt_dns_query_log_t *log = mt_dns_query_log_create();
    ASSERT(log != NULL);
    mt_dns_query_log_append(log, "192.168.1.2", "example.org", 1, false, 123456);
    mt_dns_query_log_append(log, "2001:db8::1", "example.com", 28, true, 123999);

    ASSERT_EQ(2u, (unsigned)mt_dns_query_log_size(log));
    mt_dns_query_entry_t row;
    ASSERT(mt_dns_query_log_get(log, 0, &row));
    ASSERT_STR_EQ("2001:db8::1", row.client);
    ASSERT_STR_EQ("example.com", row.domain);
    ASSERT_EQ(28, row.qtype);
    ASSERT(row.tcp);
    ASSERT_EQ(123999, row.time_ms);
    ASSERT(mt_dns_query_log_get(log, 1, &row));
    ASSERT_STR_EQ("192.168.1.2", row.client);
    ASSERT_EQ(1, row.qtype);
    ASSERT(!row.tcp);
    ASSERT(!mt_dns_query_log_get(log, 2, &row));
    mt_dns_query_log_destroy(log);
    PASS();
}

TEST discards_oldest_without_growing(void)
{
    mt_dns_query_log_t *log = mt_dns_query_log_create();
    ASSERT(log != NULL);
    for (size_t i = 0; i < MT_DNS_QUERY_LOG_CAPACITY + 7; i++) {
        mt_dns_query_log_append(log, "10.0.0.4", "example.net",
                                 1, false, (int64_t)i);
    }
    ASSERT_EQ(MT_DNS_QUERY_LOG_CAPACITY, (unsigned)mt_dns_query_log_size(log));
    mt_dns_query_entry_t newest, oldest;
    ASSERT(mt_dns_query_log_get(log, 0, &newest));
    ASSERT(mt_dns_query_log_get(log, MT_DNS_QUERY_LOG_CAPACITY - 1, &oldest));
    ASSERT_EQ(MT_DNS_QUERY_LOG_CAPACITY + 7, newest.id);
    ASSERT_EQ(8, oldest.id);
    mt_dns_query_log_destroy(log);
    PASS();
}

TEST clear_erases_entries_and_preserves_sequence(void)
{
    mt_dns_query_log_t *log = mt_dns_query_log_create();
    ASSERT(log != NULL);
    mt_dns_query_log_append(log, "127.0.0.1", "secret.example", 1, false, 11);
    mt_dns_query_log_clear(log);
    ASSERT_EQ(0u, (unsigned)mt_dns_query_log_size(log));
    mt_dns_query_entry_t row;
    ASSERT(!mt_dns_query_log_get(log, 0, &row));
    mt_dns_query_log_append(log, NULL, "after-clear.example", 12, true, 12);
    ASSERT(mt_dns_query_log_get(log, 0, &row));
    ASSERT_EQ(2, row.id);
    ASSERT_STR_EQ("unknown", row.client);
    ASSERT_STR_EQ("after-clear.example", row.domain);
    mt_dns_query_log_destroy(log);
    PASS();
}

TEST null_and_long_inputs_are_safe(void)
{
    mt_dns_query_log_t *log = mt_dns_query_log_create();
    ASSERT(log != NULL);
    mt_dns_query_log_append(log, "127.0.0.1", NULL, 1, false, 0);
    mt_dns_query_log_append(log, "127.0.0.1", "", 1, false, 0);
    ASSERT_EQ(0u, (unsigned)mt_dns_query_log_size(log));
    char domain[MT_DNS_QUERY_LOG_DOMAIN_LEN + 50];
    memset(domain, 'x', sizeof(domain) - 1);
    domain[sizeof(domain) - 1] = '\0';
    mt_dns_query_log_append(log, NULL, domain, 1, false, 0);
    mt_dns_query_entry_t row;
    ASSERT(mt_dns_query_log_get(log, 0, &row));
    ASSERT_EQ(MT_DNS_QUERY_LOG_DOMAIN_LEN - 1, strlen(row.domain));
    mt_dns_query_log_destroy(log);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(captures_requests_newest_first);
    RUN_TEST(discards_oldest_without_growing);
    RUN_TEST(clear_erases_entries_and_preserves_sequence);
    RUN_TEST(null_and_long_inputs_are_safe);
    GREATEST_MAIN_END();
}
