/* Owned kernel state for adaptive TCP routing; see tproxy_routes.h.
 * No shell commands or packet inspection live here. */
#include "magitrickle/tproxy_routes.h"
#include "magitrickle/ipset.h"
#include "magitrickle/log.h"
#include "magitrickle/models.h"
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <limits.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#define TP_PORTS 2u
#define TP_SETS 4u
#define TP_CHAINS 7u
#define TP_ROUTE_PRIORITY 3000
/* After Linux's local-table rule (0), before source-policy defaults. These
 * exact owned marks are only assigned after the existing policy guards. */
#define TP_LOCAL_RULE_PRIORITY 1u
#define TP_ALTERNATE_RULE_PRIORITY 2u
static const uint16_t tp_ports[TP_PORTS] = {80, 443};
enum { TP_EARLY, TP_ROUTE, TP_PROXY, TP_ALTERNATE, TP_INPUT, TP_FORWARD, TP_NAT };
static const char *const tp_suffix[TP_CHAINS] = {"e", "r", "p", "a", "i", "f", "n"};
static const char *const tp_tables[TP_CHAINS] = {"mangle", "mangle", "mangle", "mangle", "filter", "filter", "nat"};

typedef struct tp_family {
    bool local_rule, alternate_rule, local_route, blackhole;
    bool active;
    mt_rtnl_default_route_t route;
} tp_family_t;
typedef struct tp_cache {
    mt_tproxy_route_entry_t entry;
    int64_t expires;
} tp_cache_t;

struct mt_tproxy_routes {
    mt_tproxy_route_deps_t deps;
    char *interface;
    char **ingress;
    size_t n_ingress;
    uint16_t port;
    uint32_t ttl_seconds, max_entries;
    uint32_t local_mark, alternate_mark;
    bool local_reserved, alternate_reserved, staged, enabled;
    int loopback;
    char chains[TP_CHAINS][29];
    char route_parent[64];
    char sets[TP_SETS][2][32];
    bool set_created[TP_SETS][2];
    mt_ipset_nl_t *nl;
    tp_family_t family[2];
    tp_cache_t *cache;
    size_t n_cache;
    uint64_t local_signature;
};

static int64_t monotonic_seconds(void)
{
    struct timespec ts;
    return clock_gettime(CLOCK_MONOTONIC, &ts) == 0 ? (int64_t)ts.tv_sec : 0;
}

static bool valid_interface(const char *name)
{
    return mt_adaptive_interface_valid(name);
}

static mt_ipt_t *family_ipt(const mt_tproxy_routes_t *r, size_t i)
{
    return i ? r->deps.ipt6 : r->deps.ipt4;
}

static int family_id(size_t i) { return i ? AF_INET6 : AF_INET; }

static mt_err_t make_names(mt_tproxy_routes_t *r)
{
    for (size_t i = 0; i < TP_CHAINS; i++) {
        int n = snprintf(r->chains[i], sizeof(r->chains[i]), "%st%08x_%s",
                         r->deps.chain_prefix, r->local_mark, tp_suffix[i]);
        if (n < 0 || (size_t)n >= sizeof(r->chains[i])) { return MT_ERR_INVAL; }
    }
    for (size_t s = 0; s < TP_SETS; s++) {
        for (size_t f = 0; f < 2; f++) {
            int n = snprintf(r->sets[s][f], sizeof(r->sets[s][f]), "%st%08x_%zu_%u",
                             r->deps.ipset_prefix, r->local_mark, s, f ? 6u : 4u);
            if (n < 0 || (size_t)n >= sizeof(r->sets[s][f])) { return MT_ERR_INVAL; }
        }
    }
    return MT_OK;
}

mt_tproxy_routes_t *mt_tproxy_routes_new(const mt_tproxy_route_config_t *c,
                                         const mt_tproxy_route_deps_t *deps)
{
    if (!c || !deps || !deps->rtnl || (!deps->ipt4 && !deps->ipt6) ||
        !deps->chain_prefix || !deps->ipset_prefix || !valid_interface(c->interface) ||
        !c->ingress_interfaces || c->n_ingress == 0 || c->n_ingress > 16 ||
        c->port == 0 || c->ttl_seconds == 0 || c->max_entries == 0 || c->max_entries > 65536) {
        return NULL;
    }
    for (size_t i = 0; i < c->n_ingress; i++) {
        if (!valid_interface(c->ingress_interfaces[i]) ||
            !strcmp(c->interface, c->ingress_interfaces[i])) { return NULL; }
        for (size_t j = 0; j < i; j++) {
            if (!strcmp(c->ingress_interfaces[i], c->ingress_interfaces[j])) { return NULL; }
        }
    }
    mt_tproxy_routes_t *r = calloc(1, sizeof(*r));
    if (!r) { return NULL; }
    r->deps = *deps;
    r->port = c->port;
    r->ttl_seconds = c->ttl_seconds;
    r->max_entries = c->max_entries;
    /* Count only copied names, so every allocation failure is destructible. */
    r->n_ingress = 0;
    r->interface = strdup(c->interface);
    r->ingress = calloc(c->n_ingress, sizeof(*r->ingress));
    r->cache = calloc(c->max_entries, sizeof(*r->cache));
    if (!r->interface || !r->ingress || !r->cache) { mt_tproxy_routes_free(r); return NULL; }
    for (size_t i = 0; i < c->n_ingress; i++) {
        r->ingress[i] = strdup(c->ingress_interfaces[i]);
        if (!r->ingress[i]) { mt_tproxy_routes_free(r); return NULL; }
        r->n_ingress++;
    }
    return r;
}

static mt_err_t append(mt_ipt_t *ipt, const char *table, const char *chain,
                        const char *const *args, size_t count)
{
    return mt_ipt_append(ipt, table, chain, args, count);
}
#define TP_APPEND(table, chain, ...) do { \
    const char *args_[] = {__VA_ARGS__}; \
    mt_err_t e_ = append(ipt, table, chain, args_, sizeof(args_) / sizeof(args_[0])); \
    if (e_ != MT_OK) { return e_; } \
} while (0)

static mt_err_t local_guards(mt_ipt_t *ipt, const char *chain, int family)
{
    /* Core address matches work on older Keenetic kernels where the addrtype
     * userspace module exists but the kernel match is unavailable. */
    static const char *const excluded4[] = {
        "0.0.0.0/8", "10.0.0.0/8", "100.64.0.0/10", "127.0.0.0/8",
        "169.254.0.0/16", "172.16.0.0/12", "192.168.0.0/16", "224.0.0.0/4", "240.0.0.0/4"
    };
    static const char *const excluded6[] = {
        "::/128", "::1/128", "::ffff:0:0/96", "fc00::/7", "fe80::/10", "ff00::/8"
    };
    const char *const *excluded = family == AF_INET ? excluded4 : excluded6;
    size_t count = family == AF_INET ? sizeof(excluded4) / sizeof(excluded4[0]) : sizeof(excluded6) / sizeof(excluded6[0]);
    if (family == AF_INET6) {
        TP_APPEND("mangle", chain, "!", "-d", "2000::/3", "-j", "RETURN");
    }
    for (size_t i = 0; i < count; i++) { TP_APPEND("mangle", chain, "-d", excluded[i], "-j", "RETURN"); }
    struct ifaddrs *addresses = NULL;
    if (getifaddrs(&addresses) != 0) { return mt_err_from_errno(errno); }
    mt_err_t err = MT_OK;
    for (const struct ifaddrs *a = addresses; a && err == MT_OK; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != family) { continue; }
        const void *bytes = family == AF_INET ?
            (const void *)&((const struct sockaddr_in *)a->ifa_addr)->sin_addr :
            (const void *)&((const struct sockaddr_in6 *)a->ifa_addr)->sin6_addr;
        char host[INET6_ADDRSTRLEN];
        if (!inet_ntop(family, bytes, host, sizeof(host))) { err = mt_err_from_errno(errno); break; }
        const char *args[] = {"-d", host, "-j", "RETURN"};
        err = append(ipt, "mangle", chain, args, 4);
        if (err == MT_OK && family == AF_INET && a->ifa_broadaddr &&
            (a->ifa_flags & 2u) && a->ifa_broadaddr->sa_family == AF_INET) {
            bytes = &((const struct sockaddr_in *)a->ifa_broadaddr)->sin_addr;
            if (!inet_ntop(family, bytes, host, sizeof(host))) { err = mt_err_from_errno(errno); break; }
            err = append(ipt, "mangle", chain, args, 4);
        }
    }
    freeifaddrs(addresses);
    return err;
}

static uint64_t address_hash(int family, const void *address)
{
    const unsigned char *bytes = address;
    size_t width = family == AF_INET ? 4u : 16u;
    uint64_t hash = UINT64_C(1469598103934665603) ^ (uint64_t)(unsigned)family;
    for (size_t i = 0; i < width; i++) { hash = (hash ^ bytes[i]) * UINT64_C(1099511628211); }
    return hash;
}

static mt_err_t local_signature(const mt_tproxy_routes_t *r, uint64_t *signature)
{
    struct ifaddrs *addresses = NULL;
    if (getifaddrs(&addresses) != 0) { return mt_err_from_errno(errno); }
    uint64_t sum = 0, count = 0;
    for (const struct ifaddrs *a = addresses; a; a = a->ifa_next) {
        if (!a->ifa_addr) { continue; }
        int family = a->ifa_addr->sa_family;
        if (family == AF_INET && r->deps.ipt4) {
            sum += address_hash(family, &((const struct sockaddr_in *)a->ifa_addr)->sin_addr); count++;
            if (a->ifa_broadaddr && (a->ifa_flags & 2u) && a->ifa_broadaddr->sa_family == AF_INET) {
                sum += address_hash(family, &((const struct sockaddr_in *)a->ifa_broadaddr)->sin_addr); count++;
            }
        } else if (family == AF_INET6 && r->deps.ipt6) {
            sum += address_hash(family, &((const struct sockaddr_in6 *)a->ifa_addr)->sin6_addr); count++;
        }
    }
    freeifaddrs(addresses);
    *signature = sum ^ (count * UINT64_C(1099511628211));
    return MT_OK;
}

static const char *capture_parent(mt_tproxy_routes_t *r)
{
#ifdef MT_ENTWARE_KN
    if (r->deps.policy && r->deps.policy->config.enabled) { return r->deps.policy->dispatch; }
#endif
    (void)r;
    return "PREROUTING";
}

static mt_err_t stage_family(mt_tproxy_routes_t *r, size_t f)
{
    mt_ipt_t *ipt = family_ipt(r, f);
    if (!ipt) { return MT_OK; }
    r->staged = true;
    for (size_t i = 0; i < TP_CHAINS; i++) {
        mt_err_t err = mt_ipt_register_chain_override(ipt, tp_tables[i], r->chains[i]);
        if (err != MT_OK) { return err; }
    }
    /* INPUT is additional to legacy MT base chains. Register it only for this
     * opt-in feature, before placing the narrowly scoped acceptance rules. */
    mt_err_t err = mt_ipt_register_chain_patch(ipt, "filter", "INPUT");
    if (err != MT_OK) { return err; }
    char local[16], alternate[16], listen_port[8];
    snprintf(local, sizeof(local), "%u", r->local_mark);
    snprintf(alternate, sizeof(alternate), "%u", r->alternate_mark);
    snprintf(listen_port, sizeof(listen_port), "%u", (unsigned)r->port);
    const char *early = r->chains[TP_EARLY], *route = r->chains[TP_ROUTE];
    const char *proxy = r->chains[TP_PROXY], *alt = r->chains[TP_ALTERNATE];

    TP_APPEND("mangle", early, "-m", "conntrack", "--ctdir", "REPLY", "-j", "RETURN");
    TP_APPEND("mangle", early, "-m", "connmark", "--mark", local, "-j", proxy);
    TP_APPEND("mangle", early, "-m", "connmark", "--mark", alternate, "-j", "MARK", "--set-mark", alternate);
    TP_APPEND("mangle", early, "-m", "connmark", "--mark", alternate, "-j", "ACCEPT");

    TP_APPEND("mangle", proxy, "-j", "MARK", "--set-mark", local);
    TP_APPEND("mangle", proxy, "-j", "CONNMARK", "--save-mark");
    TP_APPEND("mangle", proxy, "-p", "tcp", "-j", "TPROXY", "--on-port", listen_port, "--tproxy-mark", local);
    TP_APPEND("mangle", alt, "-j", "MARK", "--set-mark", alternate);
    TP_APPEND("mangle", alt, "-j", "CONNMARK", "--save-mark");
    TP_APPEND("mangle", alt, "-j", "ACCEPT");

    TP_APPEND("mangle", route, "-m", "conntrack", "--ctdir", "REPLY", "-j", "RETURN");
#ifdef MT_ENTWARE_KN
    err = mt_kn_policy_guard(r->deps.policy, ipt, "mangle", route);
    if (err != MT_OK) { return err; }
#endif
    TP_APPEND("mangle", route, "-m", "mark", "!", "--mark", "0", "-j", "RETURN");
    TP_APPEND("mangle", route, "-m", "connmark", "!", "--mark", "0", "-j", "RETURN");
    TP_APPEND("mangle", route, "-m", "conntrack", "!", "--ctstate", "NEW", "-j", "RETURN");
    TP_APPEND("mangle", route, "-p", "tcp", "!", "--syn", "-j", "RETURN");
    err = local_guards(ipt, route, family_id(f));
    if (err != MT_OK) { return err; }
    for (size_t p = 0; p < TP_PORTS; p++) {
        char port[8]; snprintf(port, sizeof(port), "%u", (unsigned)tp_ports[p]);
        TP_APPEND("mangle", route, "-p", "tcp", "--dport", port, "-m", "set", "--match-set", r->sets[p][f], "dst", "-j", "RETURN");
        TP_APPEND("mangle", route, "-p", "tcp", "--dport", port, "-m", "set", "--match-set", r->sets[p + TP_PORTS][f], "dst", "-j", alt);
        TP_APPEND("mangle", route, "-p", "tcp", "--dport", port, "-j", proxy);
    }
    snprintf(r->route_parent, sizeof(r->route_parent), "%s", capture_parent(r));
    for (size_t i = 0; i < r->n_ingress; i++) {
        const char *jump[] = {"-i", r->ingress[i], "-p", "tcp", "-j", early};
        err = mt_ipt_insert(ipt, "mangle", "PREROUTING", 1, jump, 6);
        if (err != MT_OK) { return err; }
        const char *capture[] = {"-i", r->ingress[i], "-p", "tcp", "-j", route};
        err = mt_ipt_append_ordered(ipt, "mangle", r->route_parent, TP_ROUTE_PRIORITY, capture, 6);
        if (err != MT_OK) { return err; }
        for (size_t p = 0; p < TP_PORTS; p++) {
            char port[8]; snprintf(port, sizeof(port), "%u", (unsigned)tp_ports[p]);
            TP_APPEND("filter", r->chains[TP_INPUT], "-i", r->ingress[i], "-p", "tcp", "--dport", port,
                "-m", "conntrack", "--ctdir", "ORIGINAL", "-m", "mark", "--mark", local,
                "-m", "connmark", "--mark", local, "-j", "ACCEPT");
            TP_APPEND("filter", r->chains[TP_FORWARD], "-i", r->ingress[i], "-o", r->interface,
                "-p", "tcp", "--dport", port, "-m", "conntrack", "--ctdir", "ORIGINAL",
                "-m", "mark", "--mark", alternate, "-m", "connmark", "--mark", alternate, "-j", "ACCEPT");
        }
    }
    TP_APPEND("nat", r->chains[TP_NAT], "-o", r->interface, "-p", "tcp", "-m", "conntrack", "--ctdir", "ORIGINAL",
        "-m", "mark", "--mark", alternate, "-j", "MASQUERADE");
    const char *input[] = {"-j", r->chains[TP_INPUT]};
    err = mt_ipt_insert(ipt, "filter", "INPUT", 1, input, 2);
    if (err != MT_OK) { return err; }
    const char *forward[] = {"-j", r->chains[TP_FORWARD]};
    err = mt_ipt_insert(ipt, "filter", "FORWARD", 1, forward, 2);
    if (err != MT_OK) { return err; }
    TP_APPEND("nat", "POSTROUTING", "-j", r->chains[TP_NAT]);
    return MT_OK;
}
#undef TP_APPEND

mt_err_t mt_tproxy_routes_prepare(mt_tproxy_routes_t *r)
{
    if (!r || !r->enabled) { return MT_OK; }
    mt_err_t e4 = stage_family(r, 0);
    mt_err_t e6 = stage_family(r, 1);
    if (e4 != MT_OK || e6 != MT_OK) { return e4 != MT_OK ? e4 : e6; }
    return local_signature(r, &r->local_signature);
}

static void first_error(mt_err_t *first, mt_err_t next)
{
    if (*first == MT_OK && next != MT_OK && next != MT_ERR_NOENT) { *first = next; }
}

static mt_err_t remove_family_chains(mt_tproxy_routes_t *r, size_t f)
{
    mt_ipt_t *ipt = family_ipt(r, f);
    if (!ipt || !r->staged) { return MT_OK; }
    mt_err_t err = MT_OK;
    for (size_t i = 0; i < r->n_ingress; i++) {
        const char *early[] = {"-i", r->ingress[i], "-p", "tcp", "-j", r->chains[TP_EARLY]};
        first_error(&err, mt_ipt_delete(ipt, "mangle", "PREROUTING", early, 6));
        const char *capture[] = {"-i", r->ingress[i], "-p", "tcp", "-j", r->chains[TP_ROUTE]};
        if (*r->route_parent) { first_error(&err, mt_ipt_delete(ipt, "mangle", r->route_parent, capture, 6)); }
    }
    const char *input[] = {"-j", r->chains[TP_INPUT]};
    mt_err_t input_err = mt_ipt_delete(ipt, "filter", "INPUT", input, 2);
    if (input_err == MT_ERR_STATE) {
        /* A failed initial staging/rebuild may not have registered INPUT. */
        input_err = mt_ipt_register_chain_patch(ipt, "filter", "INPUT");
        if (input_err == MT_OK) { input_err = mt_ipt_delete(ipt, "filter", "INPUT", input, 2); }
    }
    first_error(&err, input_err);
    const char *forward[] = {"-j", r->chains[TP_FORWARD]};
    first_error(&err, mt_ipt_delete(ipt, "filter", "FORWARD", forward, 2));
    const char *nat[] = {"-j", r->chains[TP_NAT]};
    first_error(&err, mt_ipt_delete(ipt, "nat", "POSTROUTING", nat, 2));
    for (size_t i = 0; i < TP_CHAINS; i++) {
        first_error(&err, mt_ipt_register_chain_delete(ipt, tp_tables[i], r->chains[i]));
    }
    first_error(&err, mt_ipt_commit(ipt));
    return err;
}

static mt_err_t remove_entry(mt_tproxy_routes_t *r, const mt_tproxy_route_entry_t *entry)
{
    size_t p = entry->port == 443 ? 1u : 0u;
    size_t s = p + (entry->alternate ? TP_PORTS : 0u);
    size_t f = entry->family == AF_INET6 ? 1u : 0u;
    return r->nl->ops->del(r->nl, r->sets[s][f], entry->address, f ? 16u : 4u, f ? 128u : 32u);
}

static mt_err_t clear_selected(mt_tproxy_routes_t *r, bool alternate_only)
{
    mt_err_t err = MT_OK;
    for (size_t i = 0; i < r->n_cache;) {
        if (alternate_only && !r->cache[i].entry.alternate) { i++; continue; }
        mt_err_t e = remove_entry(r, &r->cache[i].entry);
        if (e == MT_OK) { r->cache[i] = r->cache[--r->n_cache]; }
        else { first_error(&err, e); i++; }
    }
    return err;
}

static mt_err_t ensure_base_routes(mt_tproxy_routes_t *r, size_t f)
{
    if (!family_ipt(r, f)) { return MT_OK; }
    int family = family_id(f);
    tp_family_t *state = &r->family[f];
    mt_rtnl_default_route_t local;
    mt_err_t err = mt_rtnl_rule_ensure_at(r->deps.rtnl, family, r->local_mark,
                                         r->local_mark, TP_LOCAL_RULE_PRIORITY);
    if (err != MT_OK) { return err; }
    state->local_rule = true;
    err = mt_rtnl_rule_ensure_at(r->deps.rtnl, family, r->alternate_mark,
                                 r->alternate_mark, TP_ALTERNATE_RULE_PRIORITY);
    if (err != MT_OK) { return err; }
    state->alternate_rule = true;
    err = mt_rtnl_get_default_route(r->deps.rtnl, family, r->local_mark, 10, &local);
    if (err != MT_OK) { return err; }
    if (local.found && (local.type != RTN_LOCAL || local.ifindex != r->loopback || local.multipath)) {
        return MT_ERR_STATE;
    }
    if (!local.found) {
        err = mt_rtnl_route_add_local(r->deps.rtnl, family, r->local_mark, 10, r->loopback);
        if (err != MT_OK) { return err; }
    }
    state->local_route = true;
    mt_rtnl_default_route_t terminal;
    err = mt_rtnl_get_default_route(r->deps.rtnl, family, r->alternate_mark, 20, &terminal);
    if (err != MT_OK) { return err; }
    if (terminal.found && terminal.type != RTN_BLACKHOLE) { return MT_ERR_STATE; }
    if (!terminal.found) {
        err = mt_rtnl_route_add_blackhole(r->deps.rtnl, family, r->alternate_mark, 20);
        if (err != MT_OK) { return err; }
    }
    state->blackhole = true;
    return MT_OK;
}

mt_err_t mt_tproxy_routes_repair(mt_tproxy_routes_t *r)
{
    if (!r || !r->enabled) { return MT_OK; }
    for (size_t f = 0; f < 2; f++) {
        if (!family_ipt(r, f)) { continue; }
        mt_err_t err = ensure_base_routes(r, f);
        if (err != MT_OK) { return err; }
        const tp_family_t *state = &r->family[f];
        if (!state->active) { continue; }
        mt_rtnl_default_route_t actual;
        err = mt_rtnl_get_default_route(r->deps.rtnl, family_id(f), r->alternate_mark, 10, &actual);
        if (err != MT_OK) { return err; }
        if (actual.found) {
            if (actual.type != RTN_UNICAST || actual.multipath || actual.ifindex != state->route.ifindex ||
                actual.gateway_len != state->route.gateway_len ||
                memcmp(actual.gateway, state->route.gateway, actual.gateway_len) != 0) { return MT_ERR_STATE; }
        } else {
            bool unavailable = false;
            err = mt_rtnl_route_replace_iface(r->deps.rtnl, family_id(f), r->alternate_mark, 10,
                state->route.ifindex, state->route.gateway, state->route.gateway_len, &unavailable);
            if (err != MT_OK) { return err; }
            /* Keep the terminal blackhole during a link readiness race. */
        }
    }
    return MT_OK;
}

static mt_err_t reconcile_family(mt_tproxy_routes_t *r, size_t f)
{
    if (!family_ipt(r, f)) { return MT_OK; }
    int family = family_id(f);
    tp_family_t *state = &r->family[f];
    mt_err_t err = ensure_base_routes(r, f);
    if (err != MT_OK) { return err; }
    mt_link_info_t link = {0}; bool available = false;
    err = mt_rtnl_link_by_name(r->deps.rtnl, r->interface, &link, &available);
    if (err != MT_OK) { return err; }
    available = available && link.up && link.operational;
#ifdef MT_ENTWARE_KN
    if (mt_kn_ifstate_blocks(r->deps.interface_states, r->interface, family)) { available = false; }
#endif
    mt_rtnl_default_route_t wanted = {.found = true, .type = RTN_UNICAST, .ifindex = link.ifindex};
    if (available) {
        err = mt_rtnl_iface_has_address(r->deps.rtnl, family, link.ifindex, &available);
        if (err != MT_OK) { return err; }
    }
    if (available && !link.point_to_point) {
        err = mt_rtnl_gateway_for_profile(r->deps.rtnl, family, link.ifindex, &available,
                                          wanted.gateway, &wanted.gateway_len);
        if (err != MT_OK) { return err; }
    }
    mt_rtnl_default_route_t actual;
    err = mt_rtnl_get_default_route(r->deps.rtnl, family, r->alternate_mark, 10, &actual);
    if (err != MT_OK) { return err; }
    if (actual.found && (actual.type != RTN_UNICAST || actual.multipath)) { return MT_ERR_STATE; }
    bool same = actual.found && available && actual.ifindex == wanted.ifindex &&
        actual.gateway_len == wanted.gateway_len && !memcmp(actual.gateway, wanted.gateway, wanted.gateway_len);
    if (!available) {
        if (actual.found) {
            err = mt_rtnl_route_del_iface(r->deps.rtnl, family, r->alternate_mark, 10,
                                         actual.ifindex, actual.gateway, actual.gateway_len);
            if (err != MT_OK) { return err; }
        }
    } else if (!same) {
        bool unavailable = false;
        err = mt_rtnl_route_replace_iface(r->deps.rtnl, family, r->alternate_mark, 10,
                                         wanted.ifindex, wanted.gateway, wanted.gateway_len, &unavailable);
        if (err != MT_OK) { return err; }
        if (unavailable) { return MT_ERR_AGAIN; }
    }
    bool changed = state->active != available || (available &&
        (state->route.ifindex != wanted.ifindex || state->route.gateway_len != wanted.gateway_len ||
         memcmp(state->route.gateway, wanted.gateway, wanted.gateway_len) != 0));
    state->active = available;
    state->route = available ? wanted : (mt_rtnl_default_route_t){0};
    if (changed && r->nl) { return clear_selected(r, true); }
    return MT_OK;
}

mt_err_t mt_tproxy_routes_reconcile(mt_tproxy_routes_t *r, const char *interface_name)
{
    if (!r || !r->enabled) { return MT_OK; }
    mt_err_t e4 = MT_OK, e6 = MT_OK;
    if (!interface_name || !strcmp(interface_name, r->interface)) {
        e4 = reconcile_family(r, 0);
        e6 = reconcile_family(r, 1);
    }
    uint64_t signature;
    mt_err_t err = local_signature(r, &signature);
    if (err == MT_OK && signature != r->local_signature) {
        uint64_t previous = r->local_signature;
        err = mt_tproxy_routes_prepare(r);
        if (err == MT_OK && r->deps.ipt4) { err = mt_ipt_commit(r->deps.ipt4); }
        if (err == MT_OK && r->deps.ipt6) { err = mt_ipt_commit(r->deps.ipt6); }
        if (err != MT_OK) { r->local_signature = previous; }
    }
    return e4 != MT_OK ? e4 : e6 != MT_OK ? e6 : err;
}

mt_err_t mt_tproxy_routes_enable(mt_tproxy_routes_t *r)
{
    if (!r) { return MT_ERR_INVAL; }
    if (r->enabled) { return MT_OK; }
    mt_err_t err = mt_rtnl_alloc_mark_table(r->deps.rtnl, r->deps.start_idx, &r->local_mark);
    if (err == MT_OK) { err = mt_rtnl_reserve_table(r->deps.rtnl, r->local_mark); r->local_reserved = err == MT_OK; }
    if (err == MT_OK) { err = mt_rtnl_alloc_mark_table(r->deps.rtnl, r->deps.start_idx, &r->alternate_mark); }
    if (err == MT_OK) { err = mt_rtnl_reserve_table(r->deps.rtnl, r->alternate_mark); r->alternate_reserved = err == MT_OK; }
    if (err == MT_OK) { err = make_names(r); }
    if (err == MT_OK) {
        r->loopback = (int)if_nametoindex("lo");
        if (r->loopback <= 0) { err = MT_ERR_NOENT; }
        if (err == MT_OK && !if_nametoindex(r->interface)) { err = MT_ERR_NOENT; }
        for (size_t i = 0; i < r->n_ingress && err == MT_OK; i++) {
            if (!if_nametoindex(r->ingress[i])) { err = MT_ERR_NOENT; }
        }
    }
    if (err == MT_OK) {
        r->nl = mt_ipset_nl_real_new();
        if (!r->nl) { err = MT_ERR_SYS; }
    }
    for (size_t f = 0; f < 2 && err == MT_OK; f++) {
        if (!family_ipt(r, f)) { continue; }
        int family = family_id(f);
        err = mt_rtnl_rule_add_at(r->deps.rtnl, family, r->local_mark,
                                  r->local_mark, TP_LOCAL_RULE_PRIORITY);
        r->family[f].local_rule = err == MT_OK;
        if (err == MT_OK) {
            err = mt_rtnl_rule_add_at(r->deps.rtnl, family, r->alternate_mark,
                                      r->alternate_mark, TP_ALTERNATE_RULE_PRIORITY);
            r->family[f].alternate_rule = err == MT_OK;
        }
        if (err == MT_OK) { err = reconcile_family(r, f); }
        for (size_t s = 0; s < TP_SETS && err == MT_OK; s++) {
            /* Exclusive create, no pre-emptive destroy of a foreign set. */
            err = r->nl->ops->create(r->nl, r->sets[s][f], family, r->ttl_seconds);
            r->set_created[s][f] = err == MT_OK;
        }
    }
    if (err == MT_OK) {
        r->enabled = true;
        err = mt_tproxy_routes_prepare(r);
        if (err == MT_OK && r->deps.ipt4) { err = mt_ipt_commit(r->deps.ipt4); }
        if (err == MT_OK && r->deps.ipt6) { err = mt_ipt_commit(r->deps.ipt6); }
    }
    if (err != MT_OK) {
        mt_err_t rollback = mt_tproxy_routes_disable(r);
        if (rollback != MT_OK) { MT_ERROR("adaptive routing rollback failed: %s", mt_err_str(rollback)); }
        return err;
    }
    MT_INFO("adaptive TCP routing enabled: interface=%s ingress=%zu local_mark=0x%x alternate_mark=0x%x",
            r->interface, r->n_ingress, r->local_mark, r->alternate_mark);
    return MT_OK;
}

mt_err_t mt_tproxy_routes_disable(mt_tproxy_routes_t *r)
{
    if (!r) { return MT_OK; }
    r->enabled = false;
    /* Remove interception before local routes/listeners disappear. */
    mt_err_t first = remove_family_chains(r, 0);
    first_error(&first, remove_family_chains(r, 1));
    if (first != MT_OK) { return first; }
    r->staged = false;
    for (size_t f = 0; f < 2; f++) {
        int family = family_id(f);
        tp_family_t *s = &r->family[f];
        mt_err_t err;
        if (s->active) {
            err = mt_rtnl_route_del_iface(r->deps.rtnl, family, r->alternate_mark, 10,
                                          s->route.ifindex, s->route.gateway, s->route.gateway_len);
            first_error(&first, err); if (err == MT_OK) { s->active = false; }
        }
        if (s->blackhole) {
            err = mt_rtnl_route_del_blackhole(r->deps.rtnl, family, r->alternate_mark, 20);
            first_error(&first, err); if (err == MT_OK) { s->blackhole = false; }
        }
        if (s->local_route) {
            err = mt_rtnl_route_del_local(r->deps.rtnl, family, r->local_mark, 10, r->loopback);
            first_error(&first, err); if (err == MT_OK) { s->local_route = false; }
        }
        if (s->alternate_rule) {
            err = mt_rtnl_rule_del_at(r->deps.rtnl, family, r->alternate_mark,
                                      r->alternate_mark, TP_ALTERNATE_RULE_PRIORITY);
            first_error(&first, err); if (err == MT_OK) { s->alternate_rule = false; }
        }
        if (s->local_rule) {
            err = mt_rtnl_rule_del_at(r->deps.rtnl, family, r->local_mark,
                                      r->local_mark, TP_LOCAL_RULE_PRIORITY);
            first_error(&first, err); if (err == MT_OK) { s->local_rule = false; }
        }
        for (size_t set = 0; set < TP_SETS; set++) {
            if (!r->set_created[set][f]) { continue; }
            err = r->nl->ops->destroy(r->nl, r->sets[set][f]);
            first_error(&first, err); if (err == MT_OK) { r->set_created[set][f] = false; }
        }
    }
    if (first == MT_OK) {
        if (r->local_reserved) { mt_rtnl_release_table(r->deps.rtnl, r->local_mark); r->local_reserved = false; }
        if (r->alternate_reserved) { mt_rtnl_release_table(r->deps.rtnl, r->alternate_mark); r->alternate_reserved = false; }
        r->n_cache = 0;
    }
    return first;
}

void mt_tproxy_routes_free(mt_tproxy_routes_t *r)
{
    if (!r) { return; }
    /* Lifecycle owner must call disable and handle a cleanup failure before
     * freeing. Destructor performs no hidden kernel mutation. */
    mt_ipset_nl_free(r->nl);
    free(r->interface);
    if (r->ingress) {
        for (size_t i = 0; i < r->n_ingress; i++) { free(r->ingress[i]); }
    }
    free(r->ingress);
    free(r->cache);
    free(r);
}

static bool valid_destination(int family, const uint8_t *a)
{
    if (!a) { return false; }
    if (family == AF_INET) {
        return a[0] != 0 && a[0] != 10 && a[0] != 127 && a[0] < 224 &&
            !(a[0] == 100 && (a[1] & 0xc0u) == 0x40u) &&
            !(a[0] == 169 && a[1] == 254) && !(a[0] == 172 && (a[1] & 0xf0u) == 16) &&
            !(a[0] == 192 && a[1] == 168);
    }
    if (family != AF_INET6 || (a[0] & 0xe0u) != 0x20u || a[0] == 0xff || (a[0] & 0xfeu) == 0xfcu ||
        (a[0] == 0xfe && (a[1] & 0xc0u) == 0x80u)) { return false; }
    bool zero_prefix = true;
    for (size_t i = 0; i < 10; i++) { if (a[i]) { zero_prefix = false; break; } }
    if (zero_prefix && ((a[10] == 0xff && a[11] == 0xff) ||
        (!a[10] && !a[11] && !a[12] && !a[13] && !a[14] && a[15] <= 1))) { return false; }
    return true;
}

mt_err_t mt_tproxy_routes_learn(mt_tproxy_routes_t *r, int family,
                                const uint8_t *address, uint16_t port, bool alternate)
{
    if (!r || !r->enabled) { return MT_ERR_STATE; }
    if (!valid_destination(family, address) || (port != 80 && port != 443)) { return MT_ERR_INVAL; }
    size_t f = family == AF_INET6 ? 1u : 0u;
    if (!family_ipt(r, f)) { return MT_ERR_INVAL; }
    if (alternate && !r->family[f].active) { return MT_ERR_AGAIN; }
    int64_t now = monotonic_seconds();
    for (size_t i = 0; i < r->n_cache;) {
        if (r->cache[i].expires <= now) {
            mt_err_t err = remove_entry(r, &r->cache[i].entry);
            if (err != MT_OK) { return err; }
            r->cache[i] = r->cache[--r->n_cache];
        }
        else { i++; }
    }
    size_t width = f ? 16u : 4u, index = r->n_cache;
    for (size_t i = 0; i < r->n_cache; i++) {
        const mt_tproxy_route_entry_t *e = &r->cache[i].entry;
        if (e->family == family && e->port == port && !memcmp(e->address, address, width)) { index = i; break; }
    }
    if (index < r->n_cache && r->cache[index].entry.alternate != alternate) {
        mt_err_t err = remove_entry(r, &r->cache[index].entry);
        if (err != MT_OK) { return err; }
        r->cache[index] = r->cache[--r->n_cache];
        index = r->n_cache;
    }
    if (index == r->n_cache && r->n_cache == r->max_entries) {
        index = 0;
        for (size_t i = 1; i < r->n_cache; i++) {
            if (r->cache[i].expires < r->cache[index].expires) { index = i; }
        }
        mt_err_t err = remove_entry(r, &r->cache[index].entry);
        if (err != MT_OK) { return err; }
        r->cache[index] = r->cache[--r->n_cache];
        index = r->n_cache;
    }
    size_t set = (port == 443 ? 1u : 0u) + (alternate ? TP_PORTS : 0u);
    mt_err_t err = r->nl->ops->add(r->nl, r->sets[set][f], address,
                                  f ? 16u : 4u, f ? 128u : 32u, true, r->ttl_seconds, true);
    if (err != MT_OK) { return err; }
    if (index == r->n_cache) { r->n_cache++; }
    tp_cache_t *entry = &r->cache[index];
    *entry = (tp_cache_t){.entry = {.family = family, .port = port, .alternate = alternate},
                          .expires = now + r->ttl_seconds};
    memcpy(entry->entry.address, address, width);
    return MT_OK;
}

mt_err_t mt_tproxy_routes_clear(mt_tproxy_routes_t *r)
{
    return r ? clear_selected(r, false) : MT_OK;
}

mt_err_t mt_tproxy_routes_snapshot(const mt_tproxy_routes_t *r,
                                   mt_tproxy_route_entry_t **out, size_t *count)
{
    if (!out || !count) { return MT_ERR_INVAL; }
    *out = NULL; *count = 0;
    if (!r || r->n_cache == 0) { return MT_OK; }
    mt_tproxy_route_entry_t *entries = calloc(r->n_cache, sizeof(*entries));
    if (!entries) { return MT_ERR_NOMEM; }
    int64_t now = monotonic_seconds();
    for (size_t i = 0; i < r->n_cache; i++) {
        if (r->cache[i].expires <= now) { continue; }
        entries[*count] = r->cache[i].entry;
        entries[*count].expires_in_seconds = (uint32_t)(r->cache[i].expires - now);
        (*count)++;
    }
    *out = entries;
    return MT_OK;
}

uint32_t mt_tproxy_routes_mark(const mt_tproxy_routes_t *r, bool alternate)
{
    return !r || !r->enabled ? UINT32_MAX : alternate ? r->alternate_mark : 0;
}
#ifdef MT_ENTWARE_KN
void mt_tproxy_routes_set_interface_states(mt_tproxy_routes_t *r, const mt_kn_ifstate_t *states)
{
    if (r) { r->deps.interface_states = states; }
}
#endif
