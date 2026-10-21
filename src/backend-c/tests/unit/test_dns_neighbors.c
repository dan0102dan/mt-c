#include "greatest.h"
#include "magitrickle/dns_neighbors.h"
#ifdef MT_ENTWARE_KN
#include <arpa/inet.h>
#include <assert.h>
#include <linux/neighbour.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <stdlib.h>
#include <string.h>

#ifndef NLM_F_DUMP_INTR
#define NLM_F_DUMP_INTR 0x10
#endif

static const unsigned char lenovo_mac[6] = {0x14, 0x5a, 0xfc, 0x2f, 0x64, 0xd3};
static const char *lenovo_ip = "fe80::4a17:e738:40c9:6a7";
static const char *hotspot = "{\"host\":[{\"ip\":\"192.168.5.125\",\"mac\":\"14:5A:FC:2F:64:D3\","
                             "\"name\":\"Lenovo\",\"hostname\":\"Why\",\"active\":true}]}";
typedef struct { unsigned char data[512]; size_t len; } packet_t;

static void attribute(packet_t *p, unsigned short type, const void *value, size_t size)
{
    struct rtattr attr = {.rta_len = (unsigned short)(sizeof(attr) + size), .rta_type = type};
    size_t step = (sizeof(attr) + size + 3u) & ~(size_t)3u;
    assert(p->len + step <= sizeof(p->data));
    memcpy(p->data + p->len, &attr, sizeof(attr));
    memcpy(p->data + p->len + sizeof(attr), value, size);
    p->len += step;
}
static packet_t neighbor(const char *ip, const unsigned char *mac, int ifindex, unsigned short state)
{
    packet_t p = {0};
    struct nlmsghdr h = {.nlmsg_type = RTM_NEWNEIGH, .nlmsg_seq = 1, .nlmsg_flags = NLM_F_MULTI};
    struct ndmsg nd = {.ndm_family = AF_INET6, .ndm_type = RTN_UNICAST,
                      .ndm_ifindex = ifindex, .ndm_state = state};
    memcpy(p.data, &h, sizeof(h)); p.len = sizeof(h);
    memcpy(p.data + p.len, &nd, sizeof(nd)); p.len += sizeof(nd);
    struct in6_addr addr;
    assert(inet_pton(AF_INET6, ip, &addr) == 1);
    attribute(&p, NDA_DST, &addr, sizeof(addr));
    if (mac) { attribute(&p, NDA_LLADDR, mac, 6); }
    h.nlmsg_len = (uint32_t)p.len;
    memcpy(p.data, &h, sizeof(h));
    return p;
}
static mt_err_t parse(mt_dns_neighbors_t *t, const packet_t *p)
{
    bool done = false;
    return mt_dns_neighbors_parse(t, p->data, p->len, 1, &done);
}
static const char *name_for(const mt_dns_client_t *clients, size_t count, const char *ip)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(clients[i].ip, ip) == 0) { return clients[i].name; }
    }
    return NULL;
}
static mt_err_t labels(const char *json, mt_dns_neighbors_t *neighbors,
                       mt_dns_client_t **clients, size_t *count)
{
    mt_dns_neighbors_finish(neighbors);
    return mt_dns_clients_parse_hotspot_neighbors(json, strlen(json), neighbors->items,
                                                  neighbors->count, clients, count);
}

TEST lenovo_ipv6_joins_hotspot_via_observed_mac(void)
{
    mt_dns_neighbors_t t = {0};
    packet_t p = neighbor(lenovo_ip, lenovo_mac, 10, NUD_REACHABLE);
    ASSERT_EQ(MT_OK, parse(&t, &p));
    p = neighbor("fdce:427e:77fb:0:b965:72de:de:eac3", lenovo_mac, 10, NUD_STALE);
    ASSERT_EQ(MT_OK, parse(&t, &p));
    const unsigned char other[6] = {2, 3, 4, 5, 6, 7};
    p = neighbor("fe80::dead", other, 20, NUD_REACHABLE);
    ASSERT_EQ(MT_OK, parse(&t, &p));
    mt_dns_client_t *clients = NULL; size_t count = 0;
    ASSERT_EQ(MT_OK, labels(hotspot, &t, &clients, &count));
    ASSERT_EQ(3u, count);
    ASSERT(name_for(clients, count, lenovo_ip));
    ASSERT_STR_EQ("Lenovo", name_for(clients, count, lenovo_ip));
    ASSERT_STR_EQ("Lenovo", name_for(clients, count, "192.168.5.125"));
    ASSERT_STR_EQ("Lenovo", name_for(clients, count, "fdce:427e:77fb:0:b965:72de:de:eac3"));
    ASSERT(!name_for(clients, count, "fe80::dead"));
    free(clients); free(t.items);
    PASS();
}

TEST stale_is_usable_failed_incomplete_and_proxy_are_not(void)
{
    const unsigned short states[] = {NUD_REACHABLE, NUD_STALE, NUD_DELAY, NUD_PROBE, NUD_PERMANENT,
                                     NUD_NOARP, NUD_FAILED, NUD_INCOMPLETE, NUD_NONE};
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        mt_dns_neighbors_t t = {0};
        packet_t p = neighbor(lenovo_ip, lenovo_mac, 10, states[i]);
        ASSERT_EQ(MT_OK, parse(&t, &p));
        ASSERT_EQ(i < 6 ? 1u : 0u, t.count);
        free(t.items);
    }
    mt_dns_neighbors_t t = {0};
    packet_t p = neighbor(lenovo_ip, lenovo_mac, 10, NUD_REACHABLE);
    struct ndmsg nd;
    memcpy(&nd, p.data + sizeof(struct nlmsghdr), sizeof(nd));
    nd.ndm_flags = NTF_PROXY;
    memcpy(p.data + sizeof(struct nlmsghdr), &nd, sizeof(nd));
    ASSERT_EQ(MT_OK, parse(&t, &p)); ASSERT_EQ(0u, t.count);
    p = neighbor(lenovo_ip, NULL, 10, NUD_REACHABLE);
    ASSERT_EQ(MT_OK, parse(&t, &p)); ASSERT_EQ(0u, t.count);
    const unsigned char zero[6] = {0}, multicast[6] = {1, 2, 3, 4, 5, 6};
    p = neighbor(lenovo_ip, zero, 10, NUD_REACHABLE);
    ASSERT_EQ(MT_OK, parse(&t, &p)); ASSERT_EQ(0u, t.count);
    p = neighbor(lenovo_ip, multicast, 10, NUD_REACHABLE);
    ASSERT_EQ(MT_OK, parse(&t, &p)); ASSERT_EQ(0u, t.count);
    p = neighbor("ff02::1", lenovo_mac, 10, NUD_REACHABLE);
    ASSERT_EQ(MT_OK, parse(&t, &p)); ASSERT_EQ(0u, t.count);
    free(t.items);
    PASS();
}

TEST non_ethernet_neighbors_do_not_discard_the_lan_snapshot(void)
{
    mt_dns_neighbors_t t = {0};
    packet_t p = neighbor(lenovo_ip, lenovo_mac, 10, NUD_REACHABLE);
    ASSERT_EQ(MT_OK, parse(&t, &p));
    p = neighbor("fe80::22", NULL, 20, NUD_PERMANENT);
    const unsigned char address[8] = {2, 3, 4, 5, 6, 7, 8, 9};
    attribute(&p, NDA_LLADDR, address, sizeof(address));
    struct nlmsghdr h;
    memcpy(&h, p.data, sizeof(h)); h.nlmsg_len = (uint32_t)p.len;
    memcpy(p.data, &h, sizeof(h));
    ASSERT_EQ(MT_OK, parse(&t, &p)); ASSERT_EQ(1u, t.count);
    ASSERT_STR_EQ(lenovo_ip, t.items[0].ip);
    free(t.items);
    PASS();
}

TEST ambiguous_ip_scopes_and_mac_conflicts_are_removed(void)
{
    for (unsigned variant = 0; variant < 3; variant++) {
        mt_dns_neighbors_t t = {0};
        packet_t p = neighbor(lenovo_ip, lenovo_mac, 10, NUD_REACHABLE);
        ASSERT_EQ(MT_OK, parse(&t, &p));
        unsigned char mac[6]; memcpy(mac, lenovo_mac, sizeof(mac));
        if (variant == 1) { mac[5]++; }
        p = neighbor(lenovo_ip, mac, variant == 2 ? 20 : 10, NUD_STALE);
        ASSERT_EQ(MT_OK, parse(&t, &p));
        mt_dns_neighbors_finish(&t);
        ASSERT_EQ(variant == 0 ? 1u : 0u, t.count);
        mt_dns_client_t *clients = NULL; size_t count = 0;
        ASSERT_EQ(MT_OK, labels(hotspot, &t, &clients, &count));
        ASSERT_EQ(variant == 0 ? 2u : 1u, count);
        free(clients); free(t.items);
    }
    PASS();
}

TEST missing_ipv4_hostname_fallback_and_inactive_hosts(void)
{
    const char *cases[] = {
        "{\"host\":{\"mac\":\"14:5a:fc:2f:64:d3\",\"hostname\":\"Why\"}}",
        "{\"host\":{\"mac\":\"14:5a:fc:2f:64:d3\",\"name\":\"  \",\"hostname\":\"Why\"}}",
        "{\"host\":{\"mac\":\"14:5a:fc:2f:64:d3\",\"name\":\"Lenovo\",\"active\":false}}",
        "{\"host\":{\"ip\":\"192.168.5.125\",\"name\":\"Lenovo\",\"mac\":\"14:5a:fc:2f:64:d3-extra\"}}",
        "{\"host\":{\"ip\":\"192.168.5.125\",\"name\":\"Lenovo\",\"mac\":\"14:5a:fc:2f:64:dz\"}}",
        "{\"host\":{\"ip\":\"192.168.5.125\",\"name\":\"Lenovo\"}}",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        mt_dns_neighbors_t t = {0};
        packet_t p = neighbor(lenovo_ip, lenovo_mac, 10, NUD_REACHABLE);
        ASSERT_EQ(MT_OK, parse(&t, &p));
        mt_dns_client_t *clients = NULL; size_t count = 0;
        ASSERT_EQ(MT_OK, labels(cases[i], &t, &clients, &count));
        if (i < 2) {
            ASSERT_EQ(1u, count); ASSERT_STR_EQ("Why", name_for(clients, count, lenovo_ip));
        } else { ASSERT(!name_for(clients, count, lenovo_ip)); }
        free(clients); free(t.items);
    }
    PASS();
}

TEST conflicting_inventory_names_do_not_guess_ipv6_owner(void)
{
    const char *json = "{\"host\":[{\"ip\":\"192.168.5.125\",\"mac\":\"14:5a:fc:2f:64:d3\",\"name\":\"Lenovo\"},"
                       "{\"ip\":\"192.168.5.126\",\"mac\":\"14:5a:fc:2f:64:d3\",\"name\":\"Other\"}]}";
    mt_dns_neighbors_t t = {0};
    packet_t p = neighbor(lenovo_ip, lenovo_mac, 10, NUD_REACHABLE);
    ASSERT_EQ(MT_OK, parse(&t, &p));
    mt_dns_client_t *clients = NULL; size_t count = 0;
    ASSERT_EQ(MT_OK, labels(json, &t, &clients, &count));
    ASSERT_EQ(2u, count); ASSERT(!name_for(clients, count, lenovo_ip));
    free(clients); free(t.items);
    PASS();
}

TEST enrichment_limit_preserves_explicit_ipv4_labels(void)
{
    mt_dns_neighbors_t t = {.count = MT_DNS_NEIGHBOR_LIMIT, .capacity = MT_DNS_NEIGHBOR_LIMIT};
    t.items = calloc(t.count, sizeof(*t.items)); ASSERT(t.items);
    for (size_t i = 0; i < t.count; i++) {
        (void)snprintf(t.items[i].ip, sizeof(t.items[i].ip), "fe80::%x", (unsigned)i + 1);
        memcpy(t.items[i].mac, lenovo_mac, 6); t.items[i].ifindex = 10;
    }
    mt_dns_client_t *clients = NULL; size_t count = 0;
    ASSERT_EQ(MT_OK, labels(hotspot, &t, &clients, &count));
    ASSERT_EQ(1u, count); ASSERT_STR_EQ("Lenovo", clients[0].name);
    ASSERT_STR_EQ("192.168.5.125", clients[0].ip);
    free(clients); free(t.items);
    PASS();
}

TEST truncated_unaligned_and_malformed_netlink_are_bounded(void)
{
    packet_t p = neighbor(lenovo_ip, lenovo_mac, 10, NUD_REACHABLE);
    for (size_t len = 1; len < p.len; len++) {
        mt_dns_neighbors_t t = {0}; bool done = false;
        ASSERT_EQ(MT_ERR_PROTO, mt_dns_neighbors_parse(&t, p.data, len, 1, &done));
        free(t.items);
    }
    mt_dns_neighbors_t t = {0}; bool done = false;
    unsigned char unaligned[513]; memcpy(unaligned + 1, p.data, p.len);
    ASSERT_EQ(MT_OK, mt_dns_neighbors_parse(&t, unaligned + 1, p.len, 1, &done));
    ASSERT_EQ(1u, t.count); free(t.items); t = (mt_dns_neighbors_t){0};
    ASSERT_EQ(MT_ERR_PROTO, mt_dns_neighbors_parse(&t, p.data, p.len, 2, &done));
    struct rtattr invalid = {.rta_len = 3, .rta_type = NDA_DST};
    memcpy(p.data + sizeof(struct nlmsghdr) + sizeof(struct ndmsg), &invalid, sizeof(invalid));
    ASSERT_EQ(MT_ERR_PROTO, parse(&t, &p));
    ASSERT_EQ(0u, t.count); free(t.items);
    PASS();
}

TEST interrupted_failed_and_incomplete_dumps_are_rejected(void)
{
    mt_dns_neighbors_t t = {0}; bool done = false;
    struct { struct nlmsghdr header; int status; } end = {
        .header = {.nlmsg_len = sizeof(end), .nlmsg_seq = 1, .nlmsg_type = NLMSG_DONE},
    };
    ASSERT_EQ(MT_OK, mt_dns_neighbors_parse(&t, &end, sizeof(end), 1, &done)); ASSERT(done);
    done = false; end.header.nlmsg_flags = NLM_F_DUMP_INTR;
    ASSERT_EQ(MT_ERR_AGAIN, mt_dns_neighbors_parse(&t, &end, sizeof(end), 1, &done)); ASSERT(!done);
    end.header.nlmsg_flags = 0; end.status = -1;
    ASSERT_EQ(MT_ERR_UPSTREAM, mt_dns_neighbors_parse(&t, &end, sizeof(end), 1, &done)); ASSERT(!done);
    end.header.nlmsg_type = NLMSG_OVERRUN;
    ASSERT_EQ(MT_ERR_IO, mt_dns_neighbors_parse(&t, &end, sizeof(end), 1, &done));
    end.header.nlmsg_type = NLMSG_ERROR;
    ASSERT_EQ(MT_ERR_PROTO, mt_dns_neighbors_parse(&t, &end, sizeof(end), 1, &done));
    struct { struct nlmsghdr header; struct nlmsgerr error; } ack = {
        .header = {.nlmsg_len = sizeof(ack), .nlmsg_seq = 1, .nlmsg_type = NLMSG_ERROR},
    };
    ASSERT_EQ(MT_OK, mt_dns_neighbors_parse(&t, &ack, sizeof(ack), 1, &done)); ASSERT(!done);
    ack.error.error = -1;
    ASSERT_EQ(MT_ERR_UPSTREAM, mt_dns_neighbors_parse(&t, &ack, sizeof(ack), 1, &done));
    free(t.items);
    PASS();
}

TEST neighbor_limit_and_cancellation_leave_no_partial_result(void)
{
    mt_dns_neighbors_t t = {0};
    packet_t p = neighbor(lenovo_ip, lenovo_mac, 10, NUD_REACHABLE);
    for (size_t i = 0; i < MT_DNS_NEIGHBOR_LIMIT; i++) { ASSERT_EQ(MT_OK, parse(&t, &p)); }
    ASSERT_EQ(MT_ERR_LIMIT, parse(&t, &p)); free(t.items);
    atomic_bool stop; atomic_init(&stop, true);
    mt_dns_neighbor_t *out = NULL; size_t count = 123;
    ASSERT_EQ(MT_ERR_CANCELED, mt_dns_neighbors_read(&out, &count, &stop));
    ASSERT(out == NULL); ASSERT_EQ(0u, count);
    mt_dns_client_t *clients = NULL;
    ASSERT_EQ(MT_OK, mt_dns_clients_parse_hotspot_neighbors(hotspot, strlen(hotspot), NULL, 0, &clients, &count));
    ASSERT_EQ(1u, count); ASSERT_STR_EQ("Lenovo", clients[0].name);
    free(clients);
    PASS();
}
#endif

GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
#ifdef MT_ENTWARE_KN
    RUN_TEST(lenovo_ipv6_joins_hotspot_via_observed_mac);
    RUN_TEST(stale_is_usable_failed_incomplete_and_proxy_are_not);
    RUN_TEST(non_ethernet_neighbors_do_not_discard_the_lan_snapshot);
    RUN_TEST(ambiguous_ip_scopes_and_mac_conflicts_are_removed);
    RUN_TEST(missing_ipv4_hostname_fallback_and_inactive_hosts);
    RUN_TEST(conflicting_inventory_names_do_not_guess_ipv6_owner);
    RUN_TEST(enrichment_limit_preserves_explicit_ipv4_labels);
    RUN_TEST(truncated_unaligned_and_malformed_netlink_are_bounded);
    RUN_TEST(interrupted_failed_and_incomplete_dumps_are_rejected);
    RUN_TEST(neighbor_limit_and_cancellation_leave_no_partial_result);
#endif
    GREATEST_MAIN_END();
}
