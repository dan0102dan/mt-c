#include "greatest.h"
#include <stdlib.h>
#include <string.h>
#include "magitrickle/dns_clients.h"

TEST address_normalization_is_not_prefix_stripping(void)
{
    char ip[MT_DNS_CLIENT_IP_LEN];
    const char *mapped[] = {"::ffff:192.168.1.42", "::FFFF:c0a8:012a", "0:0:0:0:0:ffff:c0a8:12a"};
    for (size_t i = 0; i < sizeof(mapped) / sizeof(*mapped); i++) {
        ASSERT(mt_dns_client_address(mapped[i], ip, sizeof(ip)));
        ASSERT_STR_EQ("192.168.1.42", ip);
    }
    ASSERT(mt_dns_client_address("2001:0db8::ffff:12", ip, sizeof(ip)));
    ASSERT_STR_EQ("2001:db8::ffff:12", ip);
    ASSERT(mt_dns_client_address("::192.168.1.42", ip, sizeof(ip)));
    ASSERT(strcmp(ip, "192.168.1.42") != 0);
    ASSERT(!mt_dns_client_address("::ffff:999.168.1.42", ip, sizeof(ip)));
    ASSERT(!mt_dns_client_address("not-an-ip", ip, sizeof(ip)));
    ASSERT(!mt_dns_client_address(NULL, ip, sizeof(ip)));
    ASSERT(!mt_dns_client_address("127.0.0.1", ip, 2));
    PASS();
}
TEST labels_preserve_unicode_and_reject_unsafe_or_truncated_values(void)
{
    char label[MT_DNS_CLIENT_NAME_LEN];
    ASSERT(mt_dns_client_label("  Телефон 📱  ", label));
    ASSERT_STR_EQ("Телефон 📱", label);
    ASSERT(!mt_dns_client_label("\xc0\x80", label));
    ASSERT(!mt_dns_client_label("\xed\xa0\x80", label));
    ASSERT(!mt_dns_client_label("\xf4\x90\x80\x80", label));
    ASSERT(!mt_dns_client_label("\xe2\x82", label));
    ASSERT(!mt_dns_client_label("name\nsecond line", label));
    ASSERT(!mt_dns_client_label("  ", label));
    ASSERT(!mt_dns_client_label("*", label));
    char too_long[MT_DNS_CLIENT_NAME_LEN + 1];
    memset(too_long, 'x', sizeof(too_long) - 1); too_long[sizeof(too_long) - 1] = '\0';
    ASSERT(!mt_dns_client_label(too_long, label));
    PASS();
}
TEST leases_ignore_expired_unnamed_and_ambiguous_addresses(void)
{
    const char *text =
        "200 aa:bb:cc:dd:ee:01 192.168.1.42 phone id\n"
        "200 aa:bb:cc:dd:ee:01 192.168.1.42 phone id\n"
        "0 aa:bb:cc:dd:ee:02 192.168.1.43 desktop id\n"
        "99 aa:bb:cc:dd:ee:03 192.168.1.44 expired id\n"
        "200 aa:bb:cc:dd:ee:04 192.168.1.45 * id\n"
        "200 aa:bb:cc:dd:ee:05 192.168.1.46 alpha id\n"
        "200 aa:bb:cc:dd:ee:06 192.168.1.46 beta id\n"
        "bad lease\n";
    mt_dns_client_t *clients = NULL; size_t n = 0;
    ASSERT_EQ(MT_OK, mt_dns_clients_parse_leases(text, strlen(text), 100, &clients, &n));
    ASSERT_EQ(2u, (unsigned)n);
    ASSERT_STR_EQ("192.168.1.42", clients[0].ip);
    ASSERT_STR_EQ("phone", clients[0].name);
    ASSERT_STR_EQ("desktop", clients[1].name);
    free(clients);
    ASSERT_EQ(MT_OK, mt_dns_clients_parse_leases("", 0, 100, &clients, &n));
    ASSERT_EQ(0u, (unsigned)n); ASSERT(clients == NULL);
    ASSERT_EQ(MT_ERR_LIMIT, mt_dns_clients_parse_leases("", MT_DNS_CLIENT_BODY_LIMIT + 1, 0, &clients, &n));
    PASS();
}
#ifdef MT_ENTWARE_KN
TEST hotspot_prefers_user_name_and_falls_back_to_hostname(void)
{
    const char *json = "{\"host\":["
        "{\"ip\":\"192.168.1.42\",\"name\":\"My phone\",\"hostname\":\"android-123\",\"active\":true},"
        "{\"ip\":\"192.168.1.43\",\"name\":\"\",\"hostname\":\"laptop\"},"
        "{\"ip\":\"192.168.1.44\",\"name\":\"old\",\"active\":false},"
        "{\"ip\":\"192.168.1.45\",\"name\":\"  \",\"hostname\":\"\"},"
        "{\"ip\":\"0:0:0:0:0:ffff:c0a8:012e\",\"name\":\"Телефон\"},"
        "{\"ip\":\"2001:db8::7\",\"name\":\"IPv6 host\"},"
        "{\"ip\":\"not-an-address\",\"name\":\"bad\"}]}";
    mt_dns_client_t *clients = NULL; size_t n = 0;
    ASSERT_EQ(MT_OK, mt_dns_clients_parse_hotspot(json, strlen(json), &clients, &n));
    ASSERT_EQ(4u, (unsigned)n);
    ASSERT_STR_EQ("My phone", clients[0].name);
    ASSERT_STR_EQ("laptop", clients[1].name);
    ASSERT_STR_EQ("192.168.1.46", clients[2].ip);
    ASSERT_STR_EQ("Телефон", clients[2].name);
    ASSERT_STR_EQ("2001:db8::7", clients[3].ip);
    free(clients);
    PASS();
}
TEST hotspot_empty_errors_and_conflicts_do_not_invent_names(void)
{
    mt_dns_client_t *clients = NULL; size_t n = 0;
    const char *bad[] = {"garbage", "{\"host\":null}", "{\"host\":[]}trailing", "[]{}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        ASSERT_EQ(MT_ERR_PROTO, mt_dns_clients_parse_hotspot(bad[i], strlen(bad[i]), &clients, &n));
        ASSERT(clients == NULL); ASSERT_EQ(0u, (unsigned)n);
    }
    const char *empty[] = {"{}", "[]", "{\"host\":[]}"};
    for (size_t i = 0; i < sizeof(empty) / sizeof(*empty); i++) {
        ASSERT_EQ(MT_OK, mt_dns_clients_parse_hotspot(empty[i], strlen(empty[i]), &clients, &n));
        ASSERT(clients == NULL); ASSERT_EQ(0u, (unsigned)n);
    }
    const char *error = "{\"status\":[{\"status\":\"error\"}]}";
    ASSERT_EQ(MT_ERR_UPSTREAM, mt_dns_clients_parse_hotspot(error, strlen(error), &clients, &n));
    const char *ambiguous = "[{\"ip\":\"192.168.1.2\",\"name\":\"first\"},{\"ip\":\"::ffff:192.168.1.2\",\"name\":\"second\"}]";
    ASSERT_EQ(MT_OK, mt_dns_clients_parse_hotspot(ambiguous, strlen(ambiguous), &clients, &n));
    ASSERT_EQ(0u, (unsigned)n); ASSERT(clients == NULL);
    ASSERT_EQ(MT_ERR_LIMIT, mt_dns_clients_parse_hotspot("", MT_DNS_CLIENT_BODY_LIMIT + 1, &clients, &n));
    PASS();
}
#endif
GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(address_normalization_is_not_prefix_stripping);
    RUN_TEST(labels_preserve_unicode_and_reject_unsafe_or_truncated_values);
    RUN_TEST(leases_ignore_expired_unnamed_and_ambiguous_addresses);
#ifdef MT_ENTWARE_KN
    RUN_TEST(hotspot_prefers_user_name_and_falls_back_to_hostname);
    RUN_TEST(hotspot_empty_errors_and_conflicts_do_not_invent_names);
#endif
    GREATEST_MAIN_END();
}
