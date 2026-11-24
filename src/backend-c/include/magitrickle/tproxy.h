/* Bounded TCP transparent relay. All calls and callbacks belong to mt_loop's
 * owner thread. This transport owns sockets, never firewall rules/routes.
 * Retry after application data is deliberately restricted to an inspected
 * TLS ClientHello, before receiving any server bytes. */
#ifndef MAGITRICKLE_TPROXY_H
#define MAGITRICKLE_TPROXY_H

#include <stdbool.h>
#include <stdint.h>

#include "magitrickle/err.h"
#include "magitrickle/loop.h"

typedef struct mt_tproxy mt_tproxy_t;

typedef struct mt_tproxy_endpoint {
    int family;                 /* AF_INET or AF_INET6 */
    uint8_t addr[16];            /* network byte order; first 4 bytes for IPv4 */
    uint16_t port;              /* host byte order */
} mt_tproxy_endpoint_t;

typedef struct mt_tproxy_config {
    uint16_t port;              /* zero asks the kernel for a free port */
    bool ipv4;
    bool ipv6;
    uint32_t connect_timeout_ms;
    uint32_t tls_timeout_ms;
    uint32_t idle_timeout_ms;
    uint32_t max_connections;
} mt_tproxy_config_t;

typedef struct mt_tproxy_ops {
    /* Mandatory. Reject direct accesses to the listener and destinations or
     * clients outside the explicitly configured interception scope. */
    bool (*authorize)(void *ctx, const mt_tproxy_endpoint_t *client,
                      const mt_tproxy_endpoint_t *destination);
    /* Mandatory. Called for each new outgoing socket, before connect().
     * Apply the direct/alternative policy mark here; errors fail this attempt.
     * Do not change the socket's blocking mode, ownership or destination. */
    mt_err_t (*prepare)(void *ctx, int fd, const mt_tproxy_endpoint_t *client,
                        const mt_tproxy_endpoint_t *destination, bool alternate);
    /* Optional. First server response for this flow (for TLS, a ServerHello
     * header is required). This establishes path reachability, not successful
     * certificate validation or page rendering. Never destroy the relay from
     * a callback. A failed learned-rule install must not close this flow. */
    void (*learned)(void *ctx, const mt_tproxy_endpoint_t *client,
                    const mt_tproxy_endpoint_t *destination, bool alternate);
} mt_tproxy_ops_t;

typedef struct mt_tproxy_stats {
    uint32_t active;
    uint64_t accepted;
    uint64_t direct_success;
    uint64_t alternate_success;
    uint64_t failures;
    uint64_t over_limit;         /* listener pauses at the configured limit */
} mt_tproxy_stats_t;

/* Binds transparent listeners for every requested family, all-or-nothing.
 * IP_TRANSPARENT requires the kernel capability; failure leaves no listeners.
 * Config and ops are copied; ctx and loop must outlive the relay. */
mt_err_t mt_tproxy_create(mt_loop_t *loop, const mt_tproxy_config_t *config,
                          const mt_tproxy_ops_t *ops, void *ctx, mt_tproxy_t **out);
void mt_tproxy_destroy(mt_tproxy_t *proxy);
uint16_t mt_tproxy_port(const mt_tproxy_t *proxy);
void mt_tproxy_stats(const mt_tproxy_t *proxy, mt_tproxy_stats_t *out);
/* Apply Linux's socket policy mark before connect(), keeping SO_MARK inside
 * the platform layer. Failure is reported, never silently ignored. */
mt_err_t mt_tproxy_set_mark(int fd, uint32_t mark);

#endif
