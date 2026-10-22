#include "greatest.h"
#include <stdlib.h>
#include <stdio.h>
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
    const char *bad[] = {"garbage", "{\"host\":null}", "{\"unexpected\":true}", "{\"host\":[]}trailing", "[]{}"};
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

static const mt_dns_client_t *client_at(const mt_dns_client_t *clients, size_t count, const char *ip)
{
    for (size_t i = 0; i < count; i++) { if (strcmp(clients[i].ip, ip) == 0) { return &clients[i]; } }
    return NULL;
}
/* Schema fixture, not a captured router response. The reported Lenovo MAC,
 * alias and addresses use RCI's ID-keyed neighbour format, corroborated by
 * https://blog.rvb.name/posts/pbr-keenetic . Address status/interface fields
 * follow the official KeeneticOS CLI show ip neighbour documentation. */
static const char *lenovo_hotspot =
    "{\"host\":[{\"ip\":\"192.168.5.125\",\"mac\":\"14:5A:FC:2F:64:D3\","
    "\"name\":\"Lenovo\",\"hostname\":\"Why\",\"active\":true,\"interface\":{\"id\":\"Bridge0\"}}]}";
static const char *lenovo_neighbors =
    "{\"7\":{\"mac\":\"14:5a:fc:2f:64:d3\",\"via\":\"02:00:00:00:00:01\","
    "\"address-family\":\"ipv4\",\"address\":\"192.168.5.125\",\"interface\":\"Bridge0\",\"expired\":false},"
    "\"8\":{\"mac\":\"14:5a:fc:2f:64:d3\",\"via\":\"02:00:00:00:00:01\","
    "\"address-family\":\"ipv6\",\"interface\":\"Bridge0\",\"expired\":false,\"addresses\":{\"address\":["
    "{\"address\":\"fe80::4a17:e738:40c9:6a7\",\"status\":\"active\"},"
    "{\"address\":\"fdce:427e:77fb:0:b965:72de:de:eac3\",\"status\":\"active\"},"
    "{\"address\":\"2001:db8::1\",\"status\":\"expired\"},"
    "{\"address\":\"2001:db8::2\",\"status\":\"unknown\"},"
    "{\"address\":\"2001:db8::3\"}]}},"
    "\"9\":{\"mac\":\"02:00:00:00:00:02\",\"via\":\"14:5a:fc:2f:64:d3\","
    "\"address-family\":\"ipv6\",\"interface\":\"Bridge0\",\"addresses\":{\"address\":{"
    "\"address\":\"fe80::9\",\"status\":\"active\"}}}}";

TEST keenetic_joins_reported_lenovo_addresses_by_mac_not_via(void)
{
    mt_dns_client_t *clients = NULL; size_t count = 0;
    ASSERT_EQ(MT_OK, mt_dns_clients_parse_keenetic(lenovo_hotspot, strlen(lenovo_hotspot),
        lenovo_neighbors, strlen(lenovo_neighbors), &clients, &count));
    ASSERT_EQ(3u, (unsigned)count);
    const char *ips[] = {"192.168.5.125", "fe80::4a17:e738:40c9:6a7", "fdce:427e:77fb:0:b965:72de:de:eac3"};
    for (size_t i = 0; i < sizeof(ips) / sizeof(*ips); i++) {
        const mt_dns_client_t *client = client_at(clients, count, ips[i]);
        ASSERT(client); ASSERT_STR_EQ("Lenovo", client->name);
    }
    free(clients); PASS();
}
TEST keenetic_accepts_single_records_without_requiring_an_ipv4_lease(void)
{
    const char *host = "{\"host\":{\"mac\":\"02:00:00:00:00:01\",\"name\":\"\",\"hostname\":\"IPv6 only\",\"active\":true}}";
    const char *neighbor = "{\"mac\":\"02:00:00:00:00:01\",\"address-family\":\"ipv6\",\"interface\":\"Bridge0\","
        "\"addresses\":{\"address\":{\"address\":\"2001:0db8::7\",\"status\":\"active\"}}}";
    mt_dns_client_t *clients = NULL; size_t count = 0;
    ASSERT_EQ(MT_OK, mt_dns_clients_parse_keenetic(host, strlen(host), neighbor, strlen(neighbor), &clients, &count));
    ASSERT_EQ(1u, (unsigned)count);
    ASSERT_STR_EQ("2001:db8::7", clients[0].ip); ASSERT_STR_EQ("IPv6 only", clients[0].name);
    free(clients); PASS();
}
TEST keenetic_rejects_inactive_expired_and_wrong_scope_joins(void)
{
    const char *hosts[] = {
        "{\"host\":{\"mac\":\"02:00:00:00:00:01\",\"name\":\"old\",\"active\":false}}",
        "{\"host\":{\"mac\":\"02:00:00:00:00:01\",\"name\":\"wrong segment\",\"interface\":{\"id\":\"Bridge1\"}}}",
        "{\"host\":{\"mac\":\"02:00:00:00:00:01\",\"name\":\"expired\"}}",
        "{\"host\":{\"mac\":\"ff:ff:ff:ff:ff:ff\",\"name\":\"invalid MAC\"}}"
    };
    const char *active = "[{\"mac\":\"02:00:00:00:00:01\",\"address-family\":\"ipv6\",\"interface\":\"Bridge0\","
        "\"addresses\":{\"address\":[{\"address\":\"fe80::1\",\"status\":\"active\"}]}}]";
    const char *expired = "[{\"mac\":\"02:00:00:00:00:01\",\"address-family\":\"ipv4\",\"expired\":true,\"address\":\"192.168.1.2\"}]";
    for (size_t i = 0; i < sizeof(hosts) / sizeof(*hosts); i++) {
        const char *neighbors = i == 2 ? expired : active;
        mt_dns_client_t *clients = NULL; size_t count = 0;
        ASSERT_EQ(MT_OK, mt_dns_clients_parse_keenetic(hosts[i], strlen(hosts[i]), neighbors, strlen(neighbors), &clients, &count));
        ASSERT_EQ(0u, (unsigned)count); ASSERT(clients == NULL);
    }
    PASS();
}
TEST keenetic_unknown_optional_scope_preserves_explicit_labels_without_widening_joins(void)
{
    const char *scopes[] = {"null", "\"Bridge0\"", "{}", "[]", "{\"id\":\"\"}", "{\"id\":false}",
        "{\"id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}"};
    const char *neighbors = "{"
        "\"unknown/client\":{\"mac\":\"02:00:00:00:00:01\",\"address-family\":\"ipv6\",\"interface\":\"Bridge0\",\"addresses\":{\"address\":["
        "{\"address\":\"fe80::20\",\"status\":\"active\"},{\"address\":\"fe80::40\",\"status\":\"active\"}]}},"
        "\"known:client\":{\"mac\":\"02:00:00:00:00:03\",\"address-family\":\"ipv6\",\"interface\":\"Bridge0\",\"addresses\":{\"address\":["
        "{\"address\":\"fe80::30\",\"status\":\"active\"},{\"address\":\"fe80::40\",\"status\":\"active\"}]}},"
        "\"offline\":{\"mac\":\"02:00:00:00:00:02\",\"address-family\":\"ipv6\",\"interface\":\"Bridge0\",\"addresses\":{\"address\":{"
        "\"address\":\"fe80::10\",\"status\":\"active\"}}}}";
    for (size_t i = 0; i < sizeof(scopes) / sizeof(*scopes); i++) {
        char hosts[1024];
        int len = snprintf(hosts, sizeof(hosts), "{\"host\":["
            "{\"ip\":\"192.168.1.10\",\"mac\":\"02:00:00:00:00:02\",\"name\":\"Offline\",\"active\":false,\"interface\":null},"
            "{\"ip\":\"192.168.1.20\",\"mac\":\"02:00:00:00:00:01\",\"name\":\"Direct\",\"active\":true,\"interface\":%s},"
            "{\"ip\":\"192.168.1.30\",\"mac\":\"02:00:00:00:00:03\",\"name\":\"Known\",\"interface\":{\"id\":\"Bridge0\"}}]}", scopes[i]);
        ASSERT(len > 0 && (size_t)len < sizeof(hosts));
        mt_dns_client_t *clients = NULL; size_t count = 0;
        ASSERT_EQ(MT_OK, mt_dns_clients_parse_keenetic(hosts, (size_t)len, neighbors, strlen(neighbors), &clients, &count));
        ASSERT_EQ(3u, (unsigned)count);
        const mt_dns_client_t *direct = client_at(clients, count, "192.168.1.20");
        const mt_dns_client_t *joined = client_at(clients, count, "fe80::30");
        ASSERT(direct); ASSERT_STR_EQ("Direct", direct->name);
        ASSERT(joined); ASSERT_STR_EQ("Known", joined->name);
        ASSERT(client_at(clients, count, "192.168.1.30"));
        ASSERT(!client_at(clients, count, "192.168.1.10"));
        ASSERT(!client_at(clients, count, "fe80::10"));
        ASSERT(!client_at(clients, count, "fe80::20"));
        /* The unrecognized host still contributes an unnamed observed owner. */
        ASSERT(!client_at(clients, count, "fe80::40"));
        free(clients);
    }
    PASS();
}
TEST keenetic_same_alias_and_unnamed_owners_cannot_hide_ambiguity(void)
{
    const char *hosts[] = {
        "[{\"mac\":\"02:00:00:00:00:01\",\"name\":\"Phone\"},{\"mac\":\"02:00:00:00:00:02\",\"name\":\"Phone\"}]",
        "[{\"mac\":\"02:00:00:00:00:01\",\"name\":\"Phone\"},{\"mac\":\"02:00:00:00:00:02\"}]",
        "[{\"mac\":\"02:00:00:00:00:01\",\"name\":\"Phone\"}]"
    };
    const char *neighbors = "["
        "{\"mac\":\"02:00:00:00:00:01\",\"address-family\":\"ipv6\",\"interface\":\"Bridge0\",\"addresses\":{\"address\":{\"address\":\"fe80::1\",\"status\":\"active\"}}},"
        "{\"mac\":\"02:00:00:00:00:02\",\"address-family\":\"ipv6\",\"interface\":\"Bridge0\",\"addresses\":{\"address\":{\"address\":\"fe80::1\",\"status\":\"active\"}}}]";
    for (size_t i = 0; i < sizeof(hosts) / sizeof(*hosts); i++) {
        mt_dns_client_t *clients = NULL; size_t count = 0;
        ASSERT_EQ(MT_OK, mt_dns_clients_parse_keenetic(hosts[i], strlen(hosts[i]), neighbors, strlen(neighbors), &clients, &count));
        ASSERT_EQ(0u, (unsigned)count); ASSERT(clients == NULL);
    }
    PASS();
}
TEST keenetic_direct_ipv6_and_duplicate_scopes_are_not_arbitrarily_named(void)
{
    const char *host = "[{\"ip\":\"fe80::1\",\"mac\":\"02:00:00:00:00:01\",\"name\":\"Phone\",\"interface\":{\"id\":\"Bridge0\"}}]";
    const char *neighbors[] = {
        "[{\"mac\":\"02:00:00:00:00:01\",\"address-family\":\"ipv6\",\"interface\":\"Bridge0\",\"addresses\":{\"address\":{\"address\":\"fe80::1\",\"status\":\"active\"}}}]",
        "[{\"mac\":\"02:00:00:00:00:01\",\"address-family\":\"ipv6\",\"interface\":\"Bridge1\",\"addresses\":{\"address\":{\"address\":\"fe80::1\",\"status\":\"active\"}}}]",
        "[{\"mac\":\"02:00:00:00:00:02\",\"address-family\":\"ipv6\",\"interface\":\"Bridge0\",\"addresses\":{\"address\":{\"address\":\"fe80::1\",\"status\":\"active\"}}}]"
    };
    for (size_t i = 0; i < sizeof(neighbors) / sizeof(*neighbors); i++) {
        mt_dns_client_t *clients = NULL; size_t count = 0;
        ASSERT_EQ(MT_OK, mt_dns_clients_parse_keenetic(host, strlen(host), neighbors[i], strlen(neighbors[i]), &clients, &count));
        ASSERT_EQ(i == 0 ? 1u : 0u, (unsigned)count);
        if (i == 0) { ASSERT_STR_EQ("Phone", clients[0].name); }
        free(clients);
    }
    PASS();
}
TEST hotspot_duplicate_owners_are_ambiguous_even_with_equal_names(void)
{
    const char *hosts[] = {
        "[{\"ip\":\"192.168.1.2\",\"mac\":\"02:00:00:00:00:01\",\"name\":\"Phone\"},{\"ip\":\"::ffff:192.168.1.2\",\"mac\":\"02:00:00:00:00:02\",\"name\":\"Phone\"}]",
        "[{\"ip\":\"192.168.1.2\",\"mac\":\"02:00:00:00:00:01\",\"name\":\"Phone\"},{\"ip\":\"192.168.1.2\",\"mac\":\"02:00:00:00:00:02\"}]",
        "[{\"ip\":\"fe80::1\",\"mac\":\"02:00:00:00:00:01\",\"name\":\"Phone\",\"interface\":{\"id\":\"Bridge0\"}},{\"ip\":\"fe80::1\",\"mac\":\"02:00:00:00:00:01\",\"name\":\"Phone\",\"interface\":{\"id\":\"Bridge1\"}}]"
    };
    for (size_t i = 0; i < sizeof(hosts) / sizeof(*hosts); i++) {
        mt_dns_client_t *clients = NULL; size_t count = 0;
        ASSERT_EQ(MT_OK, mt_dns_clients_parse_hotspot(hosts[i], strlen(hosts[i]), &clients, &count));
        ASSERT_EQ(0u, (unsigned)count); ASSERT(clients == NULL);
    }
    PASS();
}
TEST keenetic_bad_neighbor_snapshot_keeps_only_explicit_hotspot_addresses(void)
{
    const char *bad[] = {
        "garbage", "[]{}", "{\"status\":[{\"status\":\"error\"}]}", "{\"unexpected\":true}", "[null]",
        "[{\"mac\":\"14:5a:fc:2f:64:d3\",\"address-family\":\"ipv4\",\"address\":\"fe80::1\"}]",
        "[{\"mac\":\"14:5a:fc:2f:64:d3\",\"address-family\":\"ipv6\",\"addresses\":{\"address\":["
        "{\"address\":\"fe80::1\",\"status\":\"active\"},{\"address\":\"bad-address\",\"status\":\"active\"}]}}]",
        "[{\"mac\":\"14:5a:fc:2f:64:d3\",\"address-family\":\"ipv6\",\"addresses\":{\"address\":["
        "{\"address\":\"fe80::1\",\"status\":\"active\"},null]}}]",
        "[{\"mac\":\"14:5a:fc:2f:64:d3\",\"address-family\":\"ipv4\",\"address\":\"192.168.1.8\",\"expired\":\"no\"}]"
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        mt_dns_client_t *clients = NULL; size_t count = 0;
        ASSERT_EQ(MT_OK, mt_dns_clients_parse_keenetic(lenovo_hotspot, strlen(lenovo_hotspot), bad[i], strlen(bad[i]), &clients, &count));
        ASSERT_EQ(1u, (unsigned)count); ASSERT_STR_EQ("192.168.5.125", clients[0].ip);
        ASSERT_STR_EQ("Lenovo", clients[0].name); free(clients);
    }
    mt_dns_client_t *clients = NULL; size_t count = 0;
    ASSERT_EQ(MT_OK, mt_dns_clients_parse_keenetic(lenovo_hotspot, strlen(lenovo_hotspot), "", MT_DNS_CLIENT_BODY_LIMIT + 1, &clients, &count));
    ASSERT_EQ(1u, (unsigned)count); free(clients);
    const char *error = "{\"status\":[{\"status\":\"error\"}]}";
    ASSERT_EQ(MT_ERR_UPSTREAM, mt_dns_clients_parse_keenetic(error, strlen(error), lenovo_neighbors, strlen(lenovo_neighbors), &clients, &count));
    ASSERT_EQ(0u, (unsigned)count); ASSERT(clients == NULL);
    PASS();
}
TEST keenetic_oversized_enrichment_never_publishes_a_partial_join(void)
{
    size_t cap = (MT_DNS_CLIENT_LIMIT + 1u) * 64u + 256u;
    char *neighbors = malloc(cap); ASSERT(neighbors);
    size_t len = (size_t)snprintf(neighbors, cap,
        "[{\"mac\":\"14:5a:fc:2f:64:d3\",\"address-family\":\"ipv6\",\"addresses\":{\"address\":[");
    for (size_t i = 0; i <= MT_DNS_CLIENT_LIMIT; i++) {
        len += (size_t)snprintf(neighbors + len, cap - len, "%s{\"address\":\"fe80::1\",\"status\":\"active\"}", i ? "," : "");
    }
    len += (size_t)snprintf(neighbors + len, cap - len, "]}}]");
    mt_dns_client_t *clients = NULL; size_t count = 0;
    ASSERT_EQ(MT_OK, mt_dns_clients_parse_keenetic(lenovo_hotspot, strlen(lenovo_hotspot), neighbors, len, &clients, &count));
    ASSERT_EQ(1u, (unsigned)count); ASSERT_STR_EQ("192.168.5.125", clients[0].ip);
    free(clients); free(neighbors); PASS();
}
#endif
GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(address_normalization_is_not_prefix_stripping);
    RUN_TEST(labels_preserve_unicode_and_reject_unsafe_or_truncated_values);
#ifdef MT_ENTWARE_KN
    RUN_TEST(hotspot_prefers_user_name_and_falls_back_to_hostname);
    RUN_TEST(hotspot_empty_errors_and_conflicts_do_not_invent_names);
    RUN_TEST(keenetic_joins_reported_lenovo_addresses_by_mac_not_via);
    RUN_TEST(keenetic_accepts_single_records_without_requiring_an_ipv4_lease);
    RUN_TEST(keenetic_rejects_inactive_expired_and_wrong_scope_joins);
    RUN_TEST(keenetic_unknown_optional_scope_preserves_explicit_labels_without_widening_joins);
    RUN_TEST(keenetic_same_alias_and_unnamed_owners_cannot_hide_ambiguity);
    RUN_TEST(keenetic_direct_ipv6_and_duplicate_scopes_are_not_arbitrarily_named);
    RUN_TEST(hotspot_duplicate_owners_are_ambiguous_even_with_equal_names);
    RUN_TEST(keenetic_bad_neighbor_snapshot_keeps_only_explicit_hotspot_addresses);
    RUN_TEST(keenetic_oversized_enrichment_never_publishes_a_partial_join);
#endif
    GREATEST_MAIN_END();
}
