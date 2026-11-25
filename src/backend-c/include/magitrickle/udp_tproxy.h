/* Bounded UDP/443 transparent relay. Loop-thread-only, with one speculative
 * replay of structurally classified QUIC Initial-only datagrams. It does not
 * terminate QUIC, authenticate packets, or migrate established connections. */
#ifndef MAGITRICKLE_UDP_TPROXY_H
#define MAGITRICKLE_UDP_TPROXY_H

#include "magitrickle/tproxy.h"

typedef struct mt_udp_tproxy mt_udp_tproxy_t;

typedef struct mt_udp_tproxy_config {
    uint16_t port;              /* loopback listener; zero selects a free port */
    bool ipv4;
    bool ipv6;
    uint32_t response_timeout_ms;
    uint32_t idle_timeout_ms;
    uint32_t max_connections;
} mt_udp_tproxy_config_t;

typedef struct mt_udp_tproxy_ops {
    bool (*authorize)(void *ctx, const mt_tproxy_endpoint_t *client,
                      const mt_tproxy_endpoint_t *destination);
    mt_err_t (*prepare)(void *ctx, int fd, const mt_tproxy_endpoint_t *client,
                        const mt_tproxy_endpoint_t *destination, bool alternate);
    void (*learned)(void *ctx, const mt_tproxy_endpoint_t *client,
                    const mt_tproxy_endpoint_t *destination, bool alternate);
    /* Required: install a kernel pin before sending any upstream datagram.
     * Failure drops this new flow. Success supplies a nonzero ownership
     * token; unpin retires it after the transparent socket has been closed.
     * The route owner retries failed deletion in its bounded registry; a
     * closed transparent socket makes a leftover set entry inert. Never
     * destroy the relay from a callback. Calls belong to the loop thread. */
    mt_err_t (*pin)(void *ctx, const mt_tproxy_endpoint_t *client,
                    const mt_tproxy_endpoint_t *destination, uint32_t *token);
    void (*unpin)(void *ctx, uint32_t token);
} mt_udp_tproxy_ops_t;

mt_err_t mt_udp_tproxy_create(mt_loop_t *loop, const mt_udp_tproxy_config_t *config,
                              const mt_udp_tproxy_ops_t *ops, void *ctx,
                              mt_udp_tproxy_t **out);
void mt_udp_tproxy_destroy(mt_udp_tproxy_t *proxy);
uint16_t mt_udp_tproxy_port(const mt_udp_tproxy_t *proxy);
void mt_udp_tproxy_stats(const mt_udp_tproxy_t *proxy, mt_tproxy_stats_t *out);

#endif
