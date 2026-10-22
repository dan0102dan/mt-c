/* Optional Keenetic DNS labels from a read-only IPv6 neighbor snapshot.
 * Worker-only; never used for routing or as historical device identity. */
#ifndef MAGITRICKLE_DNS_NEIGHBORS_H
#define MAGITRICKLE_DNS_NEIGHBORS_H
#include "magitrickle/dns_clients.h"

#ifdef MT_ENTWARE_KN
#define MT_DNS_NEIGHBOR_LIMIT 2048u

/* Worker-owned RCI cache; never accessed by the event loop. */
typedef struct {
    char *json;
    size_t len;
    uint64_t retry_after;
    unsigned char (*observed)[6];
    size_t observed_count;
    bool retry_pending;
} mt_dns_hotspot_cache_t;
void mt_dns_hotspot_cache_clear(mt_dns_hotspot_cache_t *cache);
mt_err_t mt_dns_clients_fetch_keenetic_cached(mt_dns_client_t **clients, size_t *count,
    const atomic_bool *stop, mt_dns_hotspot_cache_t *cache);
/* Event socket only invalidates snapshots; it never mutates the label map. */
int mt_dns_neighbors_watch_open(void);
mt_err_t mt_dns_neighbors_watch_drain(int fd, bool *changed);

typedef struct {
    char ip[MT_DNS_CLIENT_IP_LEN];
    unsigned char mac[6];
    int ifindex;
} mt_dns_neighbor_t;

typedef struct {
    mt_dns_neighbor_t *items;
    size_t count, capacity;
} mt_dns_neighbors_t;

/* Append one netlink datagram. Native-endian headers are copied, never cast.
 * On any failure the caller must discard the ENTIRE accumulated snapshot. */
mt_err_t mt_dns_neighbors_parse(mt_dns_neighbors_t *table, const void *data,
                                size_t len, uint32_t sequence, bool *done);
/* Remove duplicate/conflicting IPs (including cross-interface link-local
 * collisions); sort the remaining entries by MAC for bounded joins. */
void mt_dns_neighbors_finish(mt_dns_neighbors_t *table);
/* RTM_GETNEIGH only: no probes, subprocesses, subscriptions or kernel writes.
 * NULL/0 on failure, caller frees *neighbors on success. */
mt_err_t mt_dns_neighbors_read(mt_dns_neighbor_t **neighbors, size_t *count,
                               const atomic_bool *stop);
/* neighbors must be a finished snapshot. Limit/allocation failure while
 * enriching falls back to the explicit hotspot IP labels. */
mt_err_t mt_dns_clients_parse_hotspot_neighbors(const char *json, size_t len,
                                                const mt_dns_neighbor_t *neighbors,
                                                size_t neighbor_count,
                                                mt_dns_client_t **clients, size_t *count);
#endif
#endif
