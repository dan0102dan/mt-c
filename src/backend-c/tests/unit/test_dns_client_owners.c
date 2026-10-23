#include "greatest.h"
#include "../../src/interfaces/dns_clients_internal.h"
#include <stdlib.h>
#include <string.h>

TEST ambiguous_owners_and_scopes_never_choose_an_alias(void)
{
    mt_dns_client_table_t table = {0};
    const char *a = "02:11:22:33:44:55", *b = "02:11:22:33:44:66";
    ASSERT_EQ(MT_OK, mt_dns_client_table_add_owned(&table, "192.0.2.1", "Phone", a, NULL));
    ASSERT_EQ(MT_OK, mt_dns_client_table_add_owned(&table, "192.0.2.1", "Phone", b, NULL));
    ASSERT_EQ(MT_OK, mt_dns_client_table_add_owned(&table, "192.0.2.2", "Phone", a, NULL));
    ASSERT_EQ(MT_OK, mt_dns_client_table_add_owned(&table, "192.0.2.2", NULL, b, NULL));
    ASSERT_EQ(MT_OK, mt_dns_client_table_add_owned(&table, "fe80::1", "Phone", a, "Bridge0"));
    ASSERT_EQ(MT_OK, mt_dns_client_table_add_owned(&table, "fe80::1", "Phone", a, "Bridge1"));
    ASSERT_EQ(MT_OK, mt_dns_client_table_add_owned(&table, "192.0.2.3", "First", a, NULL));
    ASSERT_EQ(MT_OK, mt_dns_client_table_add_owned(&table, "192.0.2.3", "Second", a, NULL));
    mt_dns_client_t *out = NULL;
    size_t count = 0;
    mt_dns_client_table_finish(&table, &out, &count);
    ASSERT_EQ(0u, count); ASSERT(out == NULL); ASSERT(table.items == NULL);
    PASS();
}
TEST unnamed_observations_and_mac_case_preserve_an_unambiguous_name(void)
{
    mt_dns_client_table_t table = {0};
    ASSERT_EQ(MT_OK, mt_dns_client_table_add_owned(&table, "::ffff:192.0.2.4", NULL,
        "02:AA:BB:CC:DD:EE", NULL));
    ASSERT_EQ(MT_OK, mt_dns_client_table_add_owned(&table, "192.0.2.4", "  Phone  ",
        "02:aa:bb:cc:dd:ee", "Bridge0"));
    ASSERT_EQ(MT_OK, mt_dns_client_table_add_owned(&table, "192.0.2.4", "Phone",
        "02:aa:bb:cc:dd:ee", "Bridge0"));
    ASSERT_EQ(MT_OK, mt_dns_client_table_add_owned(&table, "2001:db8::4", "", "02:00:00:00:00:01", NULL));
    ASSERT_EQ(MT_OK, mt_dns_client_table_add_owned(&table, "192.0.2.5", "Invalid", "bogus", NULL));
    mt_dns_client_t *out = NULL;
    size_t count = 0;
    mt_dns_client_table_finish(&table, &out, &count);
    ASSERT_EQ(1u, count);
    ASSERT_STR_EQ("192.0.2.4", out[0].ip); ASSERT_STR_EQ("Phone", out[0].name);
    free(out);
    PASS();
}
TEST bounded_observations_compact_without_losing_later_groups(void)
{
    mt_dns_client_table_t table = {0};
    for (unsigned i = 1; i <= 200; i++) {
        char ip[MT_DNS_CLIENT_IP_LEN], name[MT_DNS_CLIENT_NAME_LEN];
        (void)snprintf(ip, sizeof(ip), "2001:db8::%x", i);
        (void)snprintf(name, sizeof(name), "Device %u", i);
        ASSERT_EQ(MT_OK, mt_dns_client_table_add_owned(&table, ip, NULL, "02:00:00:00:00:01", NULL));
        ASSERT_EQ(MT_OK, mt_dns_client_table_add_owned(&table, ip, name, "02:00:00:00:00:01", "Bridge0"));
    }
    mt_dns_client_t *out = NULL;
    size_t count = 0;
    mt_dns_client_table_finish(&table, &out, &count);
    ASSERT_EQ(200u, count);
    for (size_t i = 0; i < count; i++) {
        unsigned number = 0;
        char ip[MT_DNS_CLIENT_IP_LEN];
        ASSERT_EQ(1, sscanf(out[i].name, "Device %u", &number));
        (void)snprintf(ip, sizeof(ip), "2001:db8::%x", number);
        ASSERT_STR_EQ(ip, out[i].ip);
        if (i) { ASSERT(strcmp(out[i - 1].ip, out[i].ip) < 0); }
    }
    free(out);
    for (unsigned i = 0; i < MT_DNS_CLIENT_LIMIT; i++) {
        ASSERT_EQ(MT_OK, mt_dns_client_table_add_owned(&table, "2001:db8::1", NULL,
            "02:00:00:00:00:01", NULL));
    }
    ASSERT_EQ(MT_ERR_LIMIT, mt_dns_client_table_add_owned(&table, "2001:db8::2", "Phone",
        "02:00:00:00:00:02", NULL));
    free(table.items);
    PASS();
}
GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(ambiguous_owners_and_scopes_never_choose_an_alias);
    RUN_TEST(unnamed_observations_and_mac_case_preserve_an_unambiguous_name);
    RUN_TEST(bounded_observations_compact_without_losing_later_groups);
    GREATEST_MAIN_END();
}
