/* Optional TCP/QUIC learning routes. Kernel mutation is serialized with the
 * app's netfilter owner. Transparent listeners must exist before enable. */
#ifndef MAGITRICKLE_TPROXY_ROUTES_H
#define MAGITRICKLE_TPROXY_ROUTES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "magitrickle/err.h"
#include "magitrickle/iptables.h"
#include "magitrickle/rtnl.h"
#ifdef MT_ENTWARE_KN
#include "magitrickle/keenetic_policy.h"
#include "magitrickle/keenetic_ifstate.h"
#endif

typedef struct mt_tproxy_route_config {
    const char *interface;
    const char *const *ingress_interfaces;
    size_t n_ingress;
    uint16_t port;              /* TCP listener bound to loopback */
    bool tcp_all_ports;         /* false retains TCP 80/443 scope */
    uint16_t udp_port;          /* zero disables UDP/443 interception */
    uint32_t max_connections;   /* active UDP pin capacity; required for UDP */
    uint32_t ttl_seconds;
    uint32_t max_entries;
} mt_tproxy_route_config_t;

typedef struct mt_tproxy_route_entry {
    int family;
    uint8_t protocol;          /* IPPROTO_TCP or IPPROTO_UDP */
    uint8_t address[16];
    uint16_t port;
    bool alternate;
    uint32_t expires_in_seconds;
} mt_tproxy_route_entry_t;

typedef struct mt_tproxy_udp_tuple {
    int family;
    uint8_t client_address[16], destination_address[16];
    uint16_t client_port, destination_port; /* host byte order */
} mt_tproxy_udp_tuple_t;

typedef struct mt_tproxy_route_deps {
    mt_ipt_t *ipt4, *ipt6;
    mt_rtnl_t *rtnl;
    const char *chain_prefix, *ipset_prefix;
    uint32_t start_idx;
#ifdef MT_ENTWARE_KN
    const mt_kn_policy_scope_t *policy;
    const mt_kn_ifstate_t *interface_states;
#endif
} mt_tproxy_route_deps_t;

typedef struct mt_tproxy_routes mt_tproxy_routes_t;

mt_tproxy_routes_t *mt_tproxy_routes_new(const mt_tproxy_route_config_t *config,
                                         const mt_tproxy_route_deps_t *deps);
/* Returns teardown errors rather than hiding an incomplete cleanup. */
mt_err_t mt_tproxy_routes_enable(mt_tproxy_routes_t *routes);
mt_err_t mt_tproxy_routes_disable(mt_tproxy_routes_t *routes);
void mt_tproxy_routes_free(mt_tproxy_routes_t *routes);
mt_err_t mt_tproxy_routes_prepare(mt_tproxy_routes_t *routes);
/* Restore owned policy rules/routes before a firewall rebuild. Does not
 * mutate the event-loop-owned learned cache. Caller holds netfilter lock. */
mt_err_t mt_tproxy_routes_repair(mt_tproxy_routes_t *routes);
mt_err_t mt_tproxy_routes_reconcile(mt_tproxy_routes_t *routes, const char *interface_name);
mt_err_t mt_tproxy_routes_learn(mt_tproxy_routes_t *routes, int family, uint8_t protocol,
                                const uint8_t *address, uint16_t port, bool alternate);
/* A successful pin returns a nonzero token. Close the connected transparent
 * UDP socket before unpinning. Retirement is lock-free; kernel deletion is
 * deferred until the next pin, prepare or reconciliation under the owner lock. */
mt_err_t mt_tproxy_routes_pin_udp(mt_tproxy_routes_t *routes,
                                  const mt_tproxy_udp_tuple_t *tuple, uint32_t *token);
void mt_tproxy_routes_unpin_udp(mt_tproxy_routes_t *routes, uint32_t token);
mt_err_t mt_tproxy_routes_clear(mt_tproxy_routes_t *routes);
mt_err_t mt_tproxy_routes_snapshot(const mt_tproxy_routes_t *routes,
                                   mt_tproxy_route_entry_t **out, size_t *count);
/* UINT32_MAX means inactive; zero is a valid DIRECT mark. */
uint32_t mt_tproxy_routes_mark(const mt_tproxy_routes_t *routes, bool alternate);
#ifdef MT_ENTWARE_KN
void mt_tproxy_routes_set_interface_states(mt_tproxy_routes_t *routes,
                                           const mt_kn_ifstate_t *states);
#endif

#endif
