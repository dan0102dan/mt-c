/* Real packet-rule compiler with fake transports; optional isolated kernel test. */
#include "greatest.h"
#include "fake_iptables.h"
#include "fake_ipset_nl.h"
#include "magitrickle/netfilter_cleaner.h"
#include <assert.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int test_addresses(struct ifaddrs **out);
static void test_free_addresses(struct ifaddrs *addresses);
static void *test_calloc(size_t count, size_t size);
static char *test_strdup(const char *value);
static void test_free(void *pointer);
#define getifaddrs test_addresses
#define freeifaddrs test_free_addresses
#define calloc test_calloc
#define strdup test_strdup
#define free test_free
#include "../../src/netfilter/tproxy_routes.c"
#undef getifaddrs
#undef freeifaddrs
#undef calloc
#undef strdup
#undef free

/* Constructor fault injection stays within this translation unit; the normal
 * fake transports and other library allocations are unaffected. */
static struct {
    bool enabled;
    size_t calls, fail_at, live;
    void *pointers[32];
} allocations;
static bool allocation_fails(void)
{
    return allocations.enabled && ++allocations.calls == allocations.fail_at;
}
static void *track_allocation(void *pointer)
{
    if (allocations.enabled && pointer) {
        assert(allocations.live < sizeof(allocations.pointers) / sizeof(allocations.pointers[0]));
        allocations.pointers[allocations.live++] = pointer;
    }
    return pointer;
}
static void *test_calloc(size_t count, size_t size)
{
    return allocation_fails() ? NULL : track_allocation(calloc(count, size));
}
static char *test_strdup(const char *value)
{
    return allocation_fails() ? NULL : track_allocation(strdup(value));
}
static void test_free(void *pointer)
{
    if (allocations.enabled && pointer) {
        size_t i = 0;
        while (i < allocations.live && allocations.pointers[i] != pointer) { i++; }
        assert(i < allocations.live);
        allocations.pointers[i] = allocations.pointers[--allocations.live];
    }
    free(pointer);
}

static struct sockaddr_in local4 = {.sin_family = AF_INET};
static struct ifaddrs local_address = {.ifa_name = "br-lan", .ifa_addr = (struct sockaddr *)&local4};
static bool use_real_addresses;
static uint8_t local_last_octet = 254;
static int test_addresses(struct ifaddrs **out)
{
    if (use_real_addresses) { return getifaddrs(out); }
    const uint8_t address[] = {203, 0, 113, local_last_octet};
    memcpy(&local4.sin_addr, address, 4);
    *out = &local_address;
    return 0;
}
static void test_free_addresses(struct ifaddrs *addresses)
{
    if (addresses != &local_address) { freeifaddrs(addresses); }
}

static struct {
    mt_fake_ipt_t *fake[2]; mt_ipt_t *ipt[2];
    mt_fake_ipset_nl_t *sets; mt_tproxy_routes_t *r;
#ifdef MT_ENTWARE_KN
    mt_kn_policy_scope_t policy;
#endif
} fixture;
static void fixture_routes_new(bool all_tcp, bool quic)
{
    const char *ingress[] = {"br-lan", "br-guest"};
    mt_tproxy_route_config_t c = {.interface = "vpn0", .ingress_interfaces = ingress,
        .n_ingress = 2, .port = 18888, .ttl_seconds = 600, .max_entries = 3,
        .tcp_all_ports = all_tcp, .udp_port = quic ? 18889 : 0, .max_connections = quic ? 2u : 0u};
    mt_tproxy_route_deps_t d = {.ipt4 = fixture.ipt[0], .ipt6 = fixture.ipt[1],
        .rtnl = (mt_rtnl_t *)&fixture, .chain_prefix = "TST_", .ipset_prefix = "tst_", .start_idx = 1000,
#ifdef MT_ENTWARE_KN
        .policy = &fixture.policy,
#endif
    };
    fixture.r = mt_tproxy_routes_new(&c, &d); assert(fixture.r);
    fixture.r->local_mark = 1000; fixture.r->alternate_mark = 1001; fixture.r->enabled = true;
    assert(make_names(fixture.r) == MT_OK);
    fixture.sets = mt_fake_ipset_nl_new(); assert(fixture.sets);
    fixture.r->nl = mt_fake_ipset_nl_as_transport(fixture.sets);
    size_t count = fixture.r->tuple_sets ? 2u : TP_SETS;
    for (size_t s = 0; s < count; s++) {
        for (size_t i = 0; i < 2; i++) {
            mt_err_t err = fixture.r->tuple_sets ? fixture.r->nl->ops->create_portset(fixture.r->nl,
                fixture.r->sets[s][i], family_id(i), false, 600, 3) :
                fixture.r->nl->ops->create(fixture.r->nl, fixture.r->sets[s][i], family_id(i), 600);
            assert(err == MT_OK);
            fixture.r->set_created[s][i] = true;
        }
    }
    if (quic) {
        for (size_t i = 0; i < 2; i++) {
            assert(fixture.r->nl->ops->create_portset(fixture.r->nl, fixture.r->flow_sets[i],
                family_id(i), true, 0, fixture.r->pin_capacity) == MT_OK);
            fixture.r->flow_set_created[i] = true;
        }
    }
}
static void setup(void *unused)
{
    (void)unused; memset(&fixture, 0, sizeof(fixture)); local_last_octet = 254;
    for (size_t i = 0; i < 2; i++) {
        fixture.fake[i] = mt_fake_ipt_new(i ? MT_IPT_PROTO_IPV6 : MT_IPT_PROTO_IPV4); assert(fixture.fake[i]);
        fixture.ipt[i] = mt_ipt_new(mt_fake_ipt_as_executable(fixture.fake[i])); assert(fixture.ipt[i]);
    }
    assert(mt_netfilter_register_base_chains(fixture.ipt[0], fixture.ipt[1]) == MT_OK);
    fixture_routes_new(false, false);
}
static void extensions(bool all_tcp, bool quic)
{
    mt_tproxy_routes_free(fixture.r);
    fixture_routes_new(all_tcp, quic);
}
static void teardown(void *unused)
{
    (void)unused; mt_tproxy_routes_free(fixture.r);
    for (size_t i = 0; i < 2; i++) { mt_ipt_free(fixture.ipt[i]); }
}
static bool rule_has(const mt_ipt_rule_t *rule, const char *text)
{
    char *str = mt_ipt_rule_string(rule); bool found = str && strstr(str, text); free(str); return found;
}
static bool contains(size_t family, const char *table, const char *chain, const char *text)
{
    mt_ipt_rule_t *const *rules = NULL; size_t count = 0;
    if (!mt_fake_ipt_get_rules(fixture.fake[family], table, chain, &rules, &count)) { return false; }
    for (size_t i = 0; i < count; i++) { if (rule_has(rules[i], text)) { return true; } }
    return false;
}
static mt_err_t stage_commit(void)
{
    mt_err_t err = mt_tproxy_routes_prepare(fixture.r);
    if (err != MT_OK) { fprintf(stderr, "prepare failed %s errno=%d\n", mt_err_str(err), errno); }
    for (size_t i = 0; i < 2 && err == MT_OK; i++) { err = mt_ipt_commit(fixture.ipt[i]); if (err != MT_OK) { fprintf(stderr,"commit family=%zu failed %s\n",i,mt_err_str(err)); } }
    return err;
}
TEST constructor_allocation_failures_release_partial_state(void)
{
    const char *ingress[] = {"br-lan", "br-guest"};
    mt_tproxy_route_config_t c = {.interface = "vpn0", .ingress_interfaces = ingress,
        .n_ingress = 2, .port = 18888, .ttl_seconds = 600, .max_entries = 3};
    for (unsigned quic = 0; quic < 2; quic++) {
        c.udp_port = quic ? 18889 : 0; c.max_connections = quic ? 2u : 0u;
        memset(&allocations, 0, sizeof(allocations));
        allocations.enabled = true;
        mt_tproxy_routes_t *r = mt_tproxy_routes_new(&c, &fixture.r->deps);
        bool created = r != NULL;
        size_t required = allocations.calls;
        mt_tproxy_routes_free(r);
        allocations.enabled = false;
        ASSERT(created); ASSERT(required > 0); ASSERT_EQ(0u, allocations.live);
        for (size_t fail = 1; fail <= required; fail++) {
            memset(&allocations, 0, sizeof(allocations));
            allocations.enabled = true; allocations.fail_at = fail;
            r = mt_tproxy_routes_new(&c, &fixture.r->deps);
            allocations.enabled = false;
            ASSERT_EQ(NULL, r); ASSERT_EQ(0u, allocations.live);
        }
    }
    memset(&allocations, 0, sizeof(allocations));
    c.max_connections = 0;
    ASSERT_EQ(NULL, mt_tproxy_routes_new(&c, &fixture.r->deps));
    PASS();
}
TEST precedence_and_active_flow_pinning(void)
{
    for (size_t i = 0; i < 2; i++) {
        ASSERT_EQ(MT_OK, mt_ipt_register_chain_override(fixture.ipt[i], "mangle", "TST_manual"));
        const char *mark[] = {"-j", "MARK", "--set-mark", "1234"};
        ASSERT_EQ(MT_OK, mt_ipt_append(fixture.ipt[i], "mangle", "TST_manual", mark, 4));
        const char *manual[] = {"-j", "TST_manual"};
        ASSERT_EQ(MT_OK, mt_ipt_append_ordered(fixture.ipt[i], "mangle", "PREROUTING", 1999, manual, 2));
    }
    ASSERT_EQ(MT_OK, stage_commit());
    for (size_t i = 0; i < 2; i++) {
        mt_ipt_rule_t *const *rules = NULL; size_t n = 0;
        ASSERT(mt_fake_ipt_get_rules(fixture.fake[i], "mangle", "PREROUTING", &rules, &n));
        ASSERT_EQ(5u, n);
        ASSERT(rule_has(rules[0], fixture.r->chains[TP_EARLY]));
        ASSERT(rule_has(rules[1], fixture.r->chains[TP_EARLY]));
        ASSERT(rule_has(rules[0], "-i br-") && rule_has(rules[0], "-p tcp"));
        ASSERT(rule_has(rules[2], "TST_manual"));
        ASSERT(rule_has(rules[3], fixture.r->chains[TP_ROUTE]));
        ASSERT(rule_has(rules[4], fixture.r->chains[TP_ROUTE]));
        ASSERT(contains(i, "mangle", fixture.r->chains[TP_ROUTE], "-m mark ! --mark 0 -j RETURN"));
        ASSERT(contains(i, "mangle", fixture.r->chains[TP_ROUTE], "-m connmark ! --mark 0 -j RETURN"));
        ASSERT(contains(i, "mangle", fixture.r->chains[TP_ROUTE], "-m conntrack ! --ctstate NEW -j RETURN"));
        ASSERT(contains(i, "mangle", fixture.r->chains[TP_ROUTE], "-p tcp ! --syn -j RETURN"));
        ASSERT(contains(i, "mangle", fixture.r->chains[TP_EARLY], "--ctdir REPLY -j RETURN"));
        ASSERT(contains(i, "mangle", fixture.r->chains[TP_EARLY], "-m connmark --mark 1000"));
    }
    PASS();
}
TEST ingress_ports_input_nat_and_local_exclusions(void)
{
    ASSERT_EQ(MT_OK, stage_commit());
    for (size_t i = 0; i < 2; i++) {
        ASSERT_FALSE(mt_fake_ipt_chain_exists(fixture.fake[i], "mangle", "OUTPUT"));
        ASSERT(contains(i, "mangle", fixture.r->chains[TP_PROXY], "-j TPROXY --on-port 18888"));
        ASSERT(contains(i, "mangle", fixture.r->chains[TP_PROXY],
            i ? "--on-ip ::1 --tproxy-mark 1000" : "--on-ip 127.0.0.1 --tproxy-mark 1000"));
        ASSERT(contains(i, "mangle", fixture.r->chains[TP_ROUTE], "-p tcp --dport 80 -m set"));
        ASSERT(contains(i, "mangle", fixture.r->chains[TP_ROUTE], "-p tcp --dport 443 -m set"));
        ASSERT_FALSE(contains(i, "mangle", fixture.r->chains[TP_ROUTE], "-p udp"));
        ASSERT(contains(i, "filter", fixture.r->chains[TP_INPUT], "-i br-lan -p tcp --dport 443"));
        ASSERT(contains(i, "filter", fixture.r->chains[TP_INPUT], "--ctdir ORIGINAL -m mark --mark 1000 -m connmark --mark 1000"));
        ASSERT_FALSE(contains(i, "filter", fixture.r->chains[TP_INPUT], "--dport 18888"));
        ASSERT(contains(i, "filter", fixture.r->chains[TP_FORWARD], "-o vpn0"));
        ASSERT(contains(i, "filter", fixture.r->chains[TP_FORWARD], "--mark 1001 -m connmark --mark 1001"));
        ASSERT(contains(i, "nat", fixture.r->chains[TP_NAT], "-o vpn0 -p tcp"));
        ASSERT(contains(i, "nat", fixture.r->chains[TP_NAT], "-m mark --mark 1001 -j MASQUERADE"));
    }
    ASSERT(contains(0, "mangle", fixture.r->chains[TP_ROUTE], "-d 127.0.0.0/8 -j RETURN"));
    ASSERT(contains(0, "mangle", fixture.r->chains[TP_ROUTE], "-d 192.168.0.0/16 -j RETURN"));
    ASSERT(contains(0, "mangle", fixture.r->chains[TP_ROUTE], "-d 203.0.113.254 -j RETURN"));
    ASSERT(contains(1, "mangle", fixture.r->chains[TP_ROUTE], "! -d 2000::/3 -j RETURN"));
    ASSERT(contains(1, "mangle", fixture.r->chains[TP_ROUTE], "-d fe80::/10 -j RETURN"));
    ASSERT(contains(1, "mangle", fixture.r->chains[TP_ROUTE], "-d ff00::/8 -j RETURN"));
    PASS();
}
TEST learning_is_per_host_port_family_and_route(void)
{
    const uint8_t ip[] = {203, 0, 113, 10};
    const uint8_t ip6[] = {0x20, 1, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    fixture.r->family[0].active = fixture.r->family[1].active = true;
    ASSERT_EQ(MT_OK, mt_tproxy_routes_learn(fixture.r, AF_INET, IPPROTO_TCP, ip, 443, true));
    ASSERT_EQ(MT_OK, mt_tproxy_routes_learn(fixture.r, AF_INET, IPPROTO_TCP, ip, 80, false));
    ASSERT_EQ(MT_OK, mt_tproxy_routes_learn(fixture.r, AF_INET6, IPPROTO_TCP, ip6, 443, true));
    ASSERT_EQ(1u, mt_fake_ipset_nl_entry_count(fixture.sets, fixture.r->sets[3][0]));
    ASSERT_EQ(1u, mt_fake_ipset_nl_entry_count(fixture.sets, fixture.r->sets[0][0]));
    ASSERT_EQ(1u, mt_fake_ipset_nl_entry_count(fixture.sets, fixture.r->sets[3][1]));
    size_t calls; const fake_ipset_call_t *log = mt_fake_ipset_nl_calls(fixture.sets, &calls);
    ASSERT_EQ(128, log[calls - 1].cidr); ASSERT_EQ(600u, log[calls - 1].timeout);
    ASSERT_EQ(32, log[calls - 2].cidr);
    mt_tproxy_route_entry_t *out = NULL; size_t n = 0;
    ASSERT_EQ(MT_OK, mt_tproxy_routes_snapshot(fixture.r, &out, &n)); ASSERT_EQ(3u, n);
    for (size_t i = 0; i < n; i++) { ASSERT(out[i].expires_in_seconds > 0 && out[i].expires_in_seconds <= 600); }
    free(out);
    ASSERT_EQ(MT_OK, mt_tproxy_routes_learn(fixture.r, AF_INET, IPPROTO_TCP, ip, 443, false));
    ASSERT_EQ(0u, mt_fake_ipset_nl_entry_count(fixture.sets, fixture.r->sets[3][0]));
    ASSERT_EQ(1u, mt_fake_ipset_nl_entry_count(fixture.sets, fixture.r->sets[1][0])); ASSERT_EQ(3u, fixture.r->n_cache);
    PASS();
}
TEST address_change_refreshes_local_guards(void)
{
    ASSERT_EQ(MT_OK, stage_commit());
    uint64_t before = fixture.r->local_signature;
    ASSERT(contains(0, "mangle", fixture.r->chains[TP_ROUTE], "-d 203.0.113.254 -j RETURN"));
    local_last_octet = 253;
    /* A LAN address event must update the guards even though the alternate
     * interface's route itself did not change. */
    ASSERT_EQ(MT_OK, mt_tproxy_routes_reconcile(fixture.r, "br-lan"));
    ASSERT(before != fixture.r->local_signature);
    ASSERT_FALSE(contains(0, "mangle", fixture.r->chains[TP_ROUTE], "-d 203.0.113.254 -j RETURN"));
    ASSERT(contains(0, "mangle", fixture.r->chains[TP_ROUTE], "-d 203.0.113.253 -j RETURN"));
    uint64_t after = fixture.r->local_signature;
    ASSERT_EQ(MT_OK, mt_tproxy_routes_reconcile(fixture.r, "br-lan"));
    ASSERT_EQ(after, fixture.r->local_signature);
    PASS();
}
TEST bounds_expiry_and_failed_learns(void)
{
    uint8_t ip[] = {203, 0, 113, 1};
    for (uint8_t i = 1; i <= 4; i++) { ip[3] = i; ASSERT_EQ(MT_OK, mt_tproxy_routes_learn(fixture.r, AF_INET, IPPROTO_TCP, ip, 443, false)); }
    ASSERT_EQ(3u, fixture.r->n_cache); ASSERT_EQ(3u, mt_fake_ipset_nl_entry_count(fixture.sets, fixture.r->sets[1][0]));
    fixture.r->cache[0].expires = 1;
    mt_tproxy_route_entry_t *out = NULL; size_t n = 0;
    ASSERT_EQ(MT_OK, mt_tproxy_routes_snapshot(fixture.r, &out, &n)); ASSERT_EQ(2u, n); free(out);
    ASSERT_EQ(MT_OK, mt_tproxy_routes_clear(fixture.r)); ASSERT_EQ(0u, fixture.r->n_cache);
    ASSERT_EQ(0u, mt_fake_ipset_nl_entry_count(fixture.sets, fixture.r->sets[1][0]));
    mt_fake_ipset_nl_fail_next(fixture.sets, FAKE_IPSET_CALL_ADD, MT_ERR_SYS);
    ASSERT_EQ(MT_ERR_SYS, mt_tproxy_routes_learn(fixture.r, AF_INET, IPPROTO_TCP, ip, 443, false)); ASSERT_EQ(0u, fixture.r->n_cache);
    ASSERT_EQ(MT_ERR_AGAIN, mt_tproxy_routes_learn(fixture.r, AF_INET, IPPROTO_TCP, ip, 443, true)); ASSERT_EQ(0u, fixture.r->n_cache);
    PASS();
}
TEST reject_private_invalid_families_and_nonweb(void)
{
    const uint8_t private4[] = {192, 168, 1, 1}, loop4[] = {127, 0, 0, 1};
    const uint8_t public4[] = {203, 0, 113, 9}, multicast4[] = {239, 1, 2, 3};
    const uint8_t local6[] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    ASSERT_EQ(MT_ERR_INVAL, mt_tproxy_routes_learn(fixture.r, AF_INET, IPPROTO_TCP, private4, 443, false));
    ASSERT_EQ(MT_ERR_INVAL, mt_tproxy_routes_learn(fixture.r, AF_INET, IPPROTO_TCP, loop4, 80, false));
    ASSERT_EQ(MT_ERR_INVAL, mt_tproxy_routes_learn(fixture.r, AF_INET, IPPROTO_TCP, multicast4, 80, false));
    ASSERT_EQ(MT_ERR_INVAL, mt_tproxy_routes_learn(fixture.r, AF_INET6, IPPROTO_TCP, local6, 443, false));
    ASSERT_EQ(MT_ERR_INVAL, mt_tproxy_routes_learn(fixture.r, AF_INET, IPPROTO_TCP, public4, 22, false));
    ASSERT_EQ(MT_ERR_INVAL, mt_tproxy_routes_learn(fixture.r, AF_UNSPEC, IPPROTO_TCP, public4, 80, false)); ASSERT_EQ(0u, fixture.r->n_cache);
    PASS();
}
TEST all_tcp_ports_are_opt_in_and_exact(void)
{
    extensions(true, false);
    ASSERT_EQ(MT_OK, stage_commit());
    for (size_t i = 0; i < 2; i++) {
        ASSERT(contains(i, "mangle", fixture.r->chains[TP_ROUTE], "-p tcp --dport 1:65535 -m set"));
        ASSERT(contains(i, "mangle", fixture.r->chains[TP_ROUTE], "dst,dst -j RETURN"));
        ASSERT(contains(i, "filter", fixture.r->chains[TP_INPUT], "-p tcp --dport 1:65535"));
        ASSERT_FALSE(contains(i, "mangle", fixture.r->chains[TP_PROXY], "-p udp"));
        ASSERT_FALSE(contains(i, "mangle", fixture.r->chains[TP_EARLY], "-m socket"));
    }
    const uint8_t ip[] = {203, 0, 113, 40};
    ASSERT_EQ(MT_OK, mt_tproxy_routes_learn(fixture.r, AF_INET, IPPROTO_TCP, ip, 22, false));
    ASSERT_EQ(MT_OK, mt_tproxy_routes_learn(fixture.r, AF_INET, IPPROTO_TCP, ip, 65535, false));
    ASSERT_EQ(2u, mt_fake_ipset_nl_entry_count(fixture.sets, fixture.r->sets[0][0]));
    ASSERT_EQ(MT_ERR_INVAL, mt_tproxy_routes_learn(fixture.r, AF_INET, IPPROTO_TCP, ip, 0, false));
    ASSERT_EQ(MT_ERR_INVAL, mt_tproxy_routes_learn(fixture.r, AF_INET, IPPROTO_UDP, ip, 443, false));
    ASSERT_EQ(MT_ERR_INVAL, mt_tproxy_routes_learn(fixture.r, AF_INET, IPPROTO_ICMP, ip, 443, false));
    PASS();
}

TEST quic_scope_and_cache_do_not_overlap_tcp(void)
{
    extensions(false, true);
    ASSERT_EQ(MT_OK, stage_commit());
    for (size_t i = 0; i < 2; i++) {
        ASSERT(contains(i, "mangle", fixture.r->chains[TP_PROXY], "-p udp -j TPROXY --on-port 18889"));
        ASSERT(contains(i, "mangle", "PREROUTING", "-i br-lan -p udp --dport 443"));
        ASSERT(contains(i, "mangle", fixture.r->chains[TP_ROUTE], "-p udp --dport 443 -m set"));
        ASSERT_FALSE(contains(i, "mangle", fixture.r->chains[TP_ROUTE], "--dport 1:65535"));
        ASSERT_FALSE(contains(i, "mangle", fixture.r->chains[TP_ROUTE], "-p udp --dport 53"));
        ASSERT(contains(i, "filter", fixture.r->chains[TP_INPUT], "-p udp --dport 443 -m conntrack --ctdir ORIGINAL"));
        ASSERT(contains(i, "filter", fixture.r->chains[TP_FORWARD], "-o vpn0 -p udp --dport 443"));
        ASSERT(contains(i, "nat", fixture.r->chains[TP_NAT], "-p udp --dport 443 -m conntrack --ctdir ORIGINAL -m mark --mark 1001"));
        ASSERT_FALSE(mt_fake_ipt_chain_exists(fixture.fake[i], "mangle", "OUTPUT"));
        mt_ipt_rule_t *const *rules = NULL; size_t n = 0;
        ASSERT(mt_fake_ipt_get_rules(fixture.fake[i], "mangle", fixture.r->chains[TP_EARLY], &rules, &n));
        ASSERT(n >= 5); ASSERT(rule_has(rules[1], "--ctdir REPLY -j RETURN"));
        ASSERT(rule_has(rules[0], fixture.r->flow_sets[i]));
        ASSERT(rule_has(rules[0], "src,src,dst -m socket --transparent"));
        ASSERT(rule_has(rules[2], "-m connmark --mark 1000"));
    }
    const uint8_t ip[] = {203, 0, 113, 41};
    fixture.r->family[0].active = true;
    ASSERT_EQ(MT_OK, mt_tproxy_routes_learn(fixture.r, AF_INET, IPPROTO_TCP, ip, 443, true));
    ASSERT_EQ(MT_OK, mt_tproxy_routes_learn(fixture.r, AF_INET, IPPROTO_UDP, ip, 443, false));
    ASSERT_EQ(2u, fixture.r->n_cache);
    ASSERT_EQ(1u, mt_fake_ipset_nl_entry_count(fixture.sets, fixture.r->sets[0][0]));
    ASSERT_EQ(1u, mt_fake_ipset_nl_entry_count(fixture.sets, fixture.r->sets[1][0]));
    ASSERT_EQ(MT_OK, mt_tproxy_routes_learn(fixture.r, AF_INET, IPPROTO_UDP, ip, 443, true));
    ASSERT_EQ(2u, mt_fake_ipset_nl_entry_count(fixture.sets, fixture.r->sets[1][0]));
    ASSERT_EQ(0u, mt_fake_ipset_nl_entry_count(fixture.sets, fixture.r->sets[0][0]));
    ASSERT_EQ(MT_ERR_INVAL, mt_tproxy_routes_learn(fixture.r, AF_INET, IPPROTO_TCP, ip, 22, false));
    ASSERT_EQ(MT_ERR_INVAL, mt_tproxy_routes_learn(fixture.r, AF_INET, IPPROTO_UDP, ip, 80, false));
    mt_tproxy_route_entry_t *entries = NULL; size_t count = 0;
    ASSERT_EQ(MT_OK, mt_tproxy_routes_snapshot(fixture.r, &entries, &count)); ASSERT_EQ(2u, count);
    ASSERT(entries[0].protocol != entries[1].protocol); free(entries);
    ASSERT_EQ(MT_OK, clear_selected(fixture.r, true)); ASSERT_EQ(0u, fixture.r->n_cache);
    ASSERT_EQ(0u, mt_fake_ipset_nl_entry_count(fixture.sets, fixture.r->sets[1][0]));
    PASS();
}

TEST udp_reverse_conntrack_keeps_owned_relay_and_scoped_input(void)
{
    extensions(false, true);
    ASSERT_EQ(MT_OK, stage_commit());
    for (size_t f = 0; f < 2; f++) {
        mt_ipt_rule_t *const *rules = NULL; size_t n = 0;
        ASSERT(mt_fake_ipt_get_rules(fixture.fake[f], "mangle", fixture.r->chains[TP_EARLY], &rules, &n));
        ASSERT(n >= 2);
        /* A server reply can recreate conntrack first after expiry/deletion.
         * The next client datagram is then REPLY, but the exact live tuple
         * must reach its relay before the ordinary reply guard returns. */
        ASSERT(rule_has(rules[0], "-p udp --dport 443 -m set --match-set"));
        ASSERT(rule_has(rules[0], fixture.r->flow_sets[f]));
        ASSERT(rule_has(rules[0], "src,src,dst -m socket --transparent"));
        ASSERT(rule_has(rules[0], fixture.r->chains[TP_PROXY]));
        ASSERT_FALSE(rule_has(rules[0], "--ctdir"));
        ASSERT(rule_has(rules[1], "--ctdir REPLY -j RETURN"));

        ASSERT(mt_fake_ipt_get_rules(fixture.fake[f], "filter", fixture.r->chains[TP_INPUT], &rules, &n));
        size_t live_rules = 0;
        for (size_t i = 0; i < n; i++) {
            if (!rule_has(rules[i], "--ctdir")) {
                live_rules++;
                ASSERT(rule_has(rules[i], "-i br-lan ") || rule_has(rules[i], "-i br-guest "));
                ASSERT(rule_has(rules[i], "-p udp --dport 443"));
                ASSERT(rule_has(rules[i], "-m mark --mark 1000 -m connmark --mark 1000"));
                ASSERT(rule_has(rules[i], fixture.r->flow_sets[f]));
                ASSERT(rule_has(rules[i], "src,src,dst -m socket --transparent -j ACCEPT"));
            } else {
                ASSERT(rule_has(rules[i], "--ctdir ORIGINAL"));
            }
        }
        ASSERT_EQ(fixture.r->n_ingress, live_rules);
        ASSERT(contains(f, "filter", fixture.r->chains[TP_INPUT],
            "-p udp --dport 443 -m conntrack --ctdir ORIGINAL"));
        /* The exception belongs only to already owned UDP sockets. New-flow
         * capture still rejects REPLY traffic before its learned-cache lookup. */
        ASSERT(mt_fake_ipt_get_rules(fixture.fake[f], "mangle", fixture.r->chains[TP_ROUTE], &rules, &n));
        ASSERT(n > 0); ASSERT(rule_has(rules[0], "--ctdir REPLY -j RETURN"));
    }
    PASS();
}

static mt_tproxy_udp_tuple_t udp_tuple(uint16_t source_port)
{
    return (mt_tproxy_udp_tuple_t){.family = AF_INET, .client_address = {192, 168, 1, 10},
        .destination_address = {203, 0, 113, 50}, .client_port = source_port, .destination_port = 443};
}

TEST udp_pins_are_bounded_and_survive_learning_and_cache_clear(void)
{
    extensions(false, true);
    mt_tproxy_udp_tuple_t one = udp_tuple(50000), two = udp_tuple(50001), three = udp_tuple(50002);
    uint32_t first, second, third;
    ASSERT_EQ(MT_OK, mt_tproxy_routes_pin_udp(fixture.r, &one, &first)); ASSERT(first != 0);
    ASSERT_EQ(MT_ERR_EXIST, mt_tproxy_routes_pin_udp(fixture.r, &one, &third)); ASSERT_EQ(0u, third);
    ASSERT_EQ(MT_OK, mt_tproxy_routes_pin_udp(fixture.r, &two, &second));
    ASSERT_EQ(MT_ERR_LIMIT, mt_tproxy_routes_pin_udp(fixture.r, &three, &third)); ASSERT_EQ(0u, third);
    ASSERT_EQ(2u, mt_fake_ipset_nl_entry_count(fixture.sets, fixture.r->flow_sets[0]));
    size_t n;
    const fake_ipset_call_t *calls = mt_fake_ipset_nl_calls(fixture.sets, &n);
    ASSERT_EQ(FAKE_IPSET_CALL_ADD_PORT, calls[n - 1].kind); ASSERT_EQ(0u, calls[n - 1].timeout);
    ASSERT(calls[n - 1].port_key.has_ip2); ASSERT_EQ(IPPROTO_UDP, calls[n - 1].port_key.protocol);
    ASSERT_EQ(two.client_port, calls[n - 1].port_key.port);
    ASSERT_EQ(0, memcmp(two.destination_address, calls[n - 1].port_key.ip2, 4));
    ASSERT_EQ(MT_OK, mt_tproxy_routes_learn(fixture.r, AF_INET, IPPROTO_UDP, one.destination_address, 443, false));
    ASSERT_EQ(MT_OK, mt_tproxy_routes_clear(fixture.r));
    ASSERT_EQ(2u, mt_fake_ipset_nl_entry_count(fixture.sets, fixture.r->flow_sets[0]));
    (void)mt_fake_ipset_nl_calls(fixture.sets, &n);
    mt_tproxy_routes_unpin_udp(fixture.r, first);
    size_t after; (void)mt_fake_ipset_nl_calls(fixture.sets, &after); ASSERT_EQ(n, after);
    ASSERT_EQ(MT_OK, mt_tproxy_routes_pin_udp(fixture.r, &three, &third)); ASSERT(third != first);
    mt_tproxy_routes_unpin_udp(fixture.r, first); /* stale token must not retire the replacement */
    ASSERT_EQ(third, atomic_load(&fixture.r->pins[third & TP_PIN_INDEX_MASK].state));
    mt_tproxy_routes_unpin_udp(fixture.r, second); mt_tproxy_routes_unpin_udp(fixture.r, third);
    ASSERT_EQ(MT_OK, stage_commit());
    ASSERT_EQ(0u, mt_fake_ipset_nl_entry_count(fixture.sets, fixture.r->flow_sets[0]));
    mt_tproxy_routes_unpin_udp(NULL, first);
    PASS();
}

TEST udp_pin_cleanup_failure_retains_bounded_retired_slot(void)
{
    extensions(true, true);
    mt_tproxy_udp_tuple_t tuple = udp_tuple(51000);
    uint32_t old, token;
    ASSERT_EQ(MT_OK, mt_tproxy_routes_pin_udp(fixture.r, &tuple, &old));
    mt_tproxy_routes_unpin_udp(fixture.r, old);
    mt_fake_ipset_nl_fail_next(fixture.sets, FAKE_IPSET_CALL_DEL_PORT, MT_ERR_SYS);
    ASSERT_EQ(MT_ERR_SYS, mt_tproxy_routes_pin_udp(fixture.r, &tuple, &token)); ASSERT_EQ(0u, token);
    ASSERT_EQ(1u, mt_fake_ipset_nl_entry_count(fixture.sets, fixture.r->flow_sets[0]));
    ASSERT(atomic_load(&fixture.r->pins[old & TP_PIN_INDEX_MASK].state) & TP_PIN_RETIRED);
    mt_fake_ipset_nl_fail_next(fixture.sets, FAKE_IPSET_CALL_ADD_PORT, MT_ERR_SYS);
    ASSERT_EQ(MT_ERR_SYS, mt_tproxy_routes_pin_udp(fixture.r, &tuple, &token)); ASSERT_EQ(0u, token);
    ASSERT_EQ(0u, mt_fake_ipset_nl_entry_count(fixture.sets, fixture.r->flow_sets[0]));
    ASSERT_EQ(MT_OK, mt_tproxy_routes_pin_udp(fixture.r, &tuple, &token));
    ASSERT_EQ(MT_OK, stage_commit());
    ASSERT_EQ(MT_OK, mt_tproxy_routes_disable(fixture.r));
    for (size_t f = 0; f < 2; f++) {
        ASSERT_FALSE(mt_fake_ipset_nl_set_exists(fixture.sets, fixture.r->flow_sets[f]));
        ASSERT_FALSE(contains(f, "mangle", "PREROUTING", "-p udp --dport 443"));
    }
    PASS();
}
TEST rebuild_preserves_cache_disable_preserves_foreign_rules(void)
{
    const char *foreign[] = {"-p", "icmp", "-j", "ACCEPT"};
    for (size_t i = 0; i < 2; i++) { ASSERT_EQ(MT_OK, mt_ipt_append(fixture.ipt[i], "mangle", "PREROUTING", foreign, 4)); }
    ASSERT_EQ(MT_OK, stage_commit());
    const uint8_t ip[] = {203, 0, 113, 7}; ASSERT_EQ(MT_OK, mt_tproxy_routes_learn(fixture.r, AF_INET, IPPROTO_TCP, ip, 80, false));
    for (size_t i = 0; i < 2; i++) { mt_fake_ipt_reset(fixture.fake[i]); mt_ipt_reset_staged(fixture.ipt[i]); }
    ASSERT_EQ(MT_OK, mt_netfilter_register_base_chains(fixture.ipt[0], fixture.ipt[1]));
    for (size_t i = 0; i < 2; i++) { ASSERT_EQ(MT_OK, mt_ipt_append(fixture.ipt[i], "mangle", "PREROUTING", foreign, 4)); }
    ASSERT_EQ(MT_OK, stage_commit()); ASSERT_EQ(1u, fixture.r->n_cache);
    ASSERT_EQ(1u, mt_fake_ipset_nl_entry_count(fixture.sets, fixture.r->sets[0][0]));
    ASSERT_EQ(MT_OK, mt_tproxy_routes_disable(fixture.r));
    ASSERT_EQ(MT_OK, mt_tproxy_routes_disable(fixture.r));
    for (size_t i = 0; i < 2; i++) {
        ASSERT(contains(i, "mangle", "PREROUTING", "-p icmp -j ACCEPT"));
        ASSERT_FALSE(mt_fake_ipt_chain_exists(fixture.fake[i], "mangle", fixture.r->chains[TP_ROUTE]));
        ASSERT_FALSE(mt_fake_ipt_chain_exists(fixture.fake[i], "filter", fixture.r->chains[TP_INPUT]));
        ASSERT_FALSE(mt_fake_ipset_nl_set_exists(fixture.sets, fixture.r->sets[0][i]));
    }
    ASSERT_EQ(UINT32_MAX, mt_tproxy_routes_mark(fixture.r, true)); PASS();
}
#ifdef MT_ENTWARE_KN
TEST unknown_policy_stops_capture_before_mark_changes(void)
{
    mt_kn_policy_config_t config = {.enabled = true, .mode = MT_KN_POLICY_INCLUDE, .policy_id = "policy0"};
    ASSERT_EQ(MT_OK, mt_kn_policy_scope_init(&fixture.policy, &config, "TST_"));
    for (size_t i = 0; i < 2; i++) { ASSERT_EQ(MT_OK, mt_kn_policy_prepare(&fixture.policy, fixture.ipt[i])); }
    ASSERT_EQ(MT_OK, stage_commit());
    for (size_t i = 0; i < 2; i++) {
        mt_ipt_rule_t *const *rules; size_t n;
        ASSERT(mt_fake_ipt_get_rules(fixture.fake[i], "mangle", fixture.r->chains[TP_ROUTE], &rules, &n));
        ASSERT(n > 2); ASSERT(rule_has(rules[1], "-j RETURN"));
        ASSERT(contains(i, "mangle", fixture.policy.dispatch, fixture.r->chains[TP_ROUTE]));
    }
    PASS();
}
#endif
static bool isolated(void)
{
    struct stat self, init;
    return getuid() == 0 && stat("/proc/self/ns/net", &self) == 0 && stat("/proc/1/ns/net", &init) == 0 && self.st_ino != init.st_ino;
}
TEST real_kernel_routes_and_teardown(void)
{
    if (!getenv("MT_TEST_TPROXY_NETNS")) { SKIP(); }
    ASSERTm("Refusing to modify host network namespace", isolated());
    use_real_addresses = true;
    ASSERT_EQ(0, system("ip link set lo up")); ASSERT_EQ(0, system("ip link add tplan type dummy"));
    ASSERT_EQ(0, system("ip link set tplan up")); ASSERT_EQ(0, system("ip addr add 192.0.2.1/24 dev tplan"));
    ASSERT_EQ(0, system("ip link add tpvpn type dummy")); ASSERT_EQ(0, system("ip link set tpvpn up"));
    ASSERT_EQ(0, system("ip addr add 198.51.100.2/24 dev tpvpn"));
    ASSERT_EQ(0, system("ip route add default via 198.51.100.1 dev tpvpn"));
    mt_rtnl_t *rtnl = mt_rtnl_open(); ASSERT(rtnl);
    mt_ipt_t *ipt = mt_ipt_new(mt_ipt_executable_real_new(MT_IPT_PROTO_IPV4)); ASSERT(ipt);
    ASSERT_EQ(MT_OK, mt_netfilter_register_base_chains(ipt, NULL));
    const char *ingress[] = {"tplan"};
    mt_tproxy_route_config_t c = {.interface = "tpvpn", .ingress_interfaces = ingress,
        .n_ingress = 1, .port = 18888, .ttl_seconds = 30, .max_entries = 4};
    mt_tproxy_route_deps_t d = {.ipt4 = ipt, .rtnl = rtnl, .chain_prefix = "TR_", .ipset_prefix = "tr_", .start_idx = 31000};
    mt_tproxy_routes_t *r = mt_tproxy_routes_new(&c, &d); ASSERT(r); ASSERT_EQ(MT_OK, mt_tproxy_routes_enable(r));
    mt_rtnl_default_route_t route;
    ASSERT_EQ(MT_OK, mt_rtnl_get_default_route(rtnl, AF_INET, r->local_mark, 10, &route));
    ASSERT(route.found && route.type == RTN_LOCAL && route.ifindex == (int)if_nametoindex("lo"));
    ASSERT_EQ(MT_OK, mt_rtnl_get_default_route(rtnl, AF_INET, r->alternate_mark, 10, &route));
    ASSERT(route.found && route.type == RTN_UNICAST && route.ifindex == (int)if_nametoindex("tpvpn"));
    ASSERT_EQ(MT_OK, mt_rtnl_get_default_route(rtnl, AF_INET, r->alternate_mark, 20, &route)); ASSERT(route.found && route.type == RTN_BLACKHOLE);
    ASSERT_EQ(MT_OK, mt_rtnl_rule_del_at(rtnl, AF_INET, r->local_mark, r->local_mark, TP_LOCAL_RULE_PRIORITY));
    ASSERT_EQ(MT_OK, mt_rtnl_route_del_local(rtnl, AF_INET, r->local_mark, 10, (int)if_nametoindex("lo")));
    ASSERT_EQ(MT_OK, mt_tproxy_routes_repair(r));
    ASSERT_EQ(MT_OK, mt_rtnl_get_default_route(rtnl, AF_INET, r->local_mark, 10, &route)); ASSERT(route.found && route.type == RTN_LOCAL);
    const uint8_t destination[] = {203, 0, 113, 1}; ASSERT_EQ(MT_OK, mt_tproxy_routes_learn(r, AF_INET, IPPROTO_TCP, destination, 443, true));
    ASSERT_EQ(0, system("ip link set tpvpn down")); ASSERT_EQ(MT_OK, mt_tproxy_routes_reconcile(r, "tpvpn"));
    ASSERT_EQ(MT_OK, mt_rtnl_get_default_route(rtnl, AF_INET, r->alternate_mark, 10, &route)); ASSERT_FALSE(route.found); ASSERT_EQ(0u, r->n_cache);
    uint32_t local = r->local_mark, alternate = r->alternate_mark;
    ASSERT_EQ(MT_OK, mt_tproxy_routes_disable(r));
    ASSERT_EQ(MT_OK, mt_tproxy_routes_disable(r));
    ASSERT_EQ(MT_OK, mt_rtnl_get_default_route(rtnl, AF_INET, local, 10, &route)); ASSERT_FALSE(route.found);
    ASSERT_EQ(MT_OK, mt_rtnl_get_default_route(rtnl, AF_INET, alternate, 20, &route)); ASSERT_FALSE(route.found);
    mt_tproxy_routes_free(r); mt_ipt_free(ipt); mt_rtnl_close(rtnl);
    ASSERT_EQ(0, system("ip link del tpvpn")); ASSERT_EQ(0, system("ip link del tplan")); PASS();
}
SUITE(routes)
{
    SET_SETUP(setup, NULL); SET_TEARDOWN(teardown, NULL);
    RUN_TEST(constructor_allocation_failures_release_partial_state);
    RUN_TEST(precedence_and_active_flow_pinning); RUN_TEST(ingress_ports_input_nat_and_local_exclusions);
    RUN_TEST(address_change_refreshes_local_guards);
    RUN_TEST(learning_is_per_host_port_family_and_route); RUN_TEST(bounds_expiry_and_failed_learns);
    RUN_TEST(reject_private_invalid_families_and_nonweb); RUN_TEST(rebuild_preserves_cache_disable_preserves_foreign_rules);
    RUN_TEST(all_tcp_ports_are_opt_in_and_exact); RUN_TEST(quic_scope_and_cache_do_not_overlap_tcp);
    RUN_TEST(udp_reverse_conntrack_keeps_owned_relay_and_scoped_input);
    RUN_TEST(udp_pins_are_bounded_and_survive_learning_and_cache_clear);
    RUN_TEST(udp_pin_cleanup_failure_retains_bounded_retired_slot);
#ifdef MT_ENTWARE_KN
    RUN_TEST(unknown_policy_stops_capture_before_mark_changes);
#endif
}
GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN(); RUN_SUITE(routes); RUN_TEST(real_kernel_routes_and_teardown); GREATEST_MAIN_END();
}
