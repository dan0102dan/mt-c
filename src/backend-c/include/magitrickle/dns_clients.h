/* Optional current device labels. Loop-owned cache; router I/O is worker-only.
 * Names are presentation metadata, never identity or routing decisions. */
#ifndef MAGITRICKLE_DNS_CLIENTS_H
#define MAGITRICKLE_DNS_CLIENTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include "magitrickle/err.h"
#include "magitrickle/loop.h"

#define MT_DNS_CLIENT_LIMIT 2048u
#define MT_DNS_CLIENT_NAME_LEN 128u
#define MT_DNS_CLIENT_IP_LEN 48u
#define MT_DNS_CLIENT_BODY_LIMIT ((size_t)1024 * 1024)
#define MT_DNS_CLIENT_SUBSCRIBERS 8u
#define MT_DNS_CLIENT_REFRESH_MS 300000u

typedef struct mt_dns_client {
    char ip[MT_DNS_CLIENT_IP_LEN];
    char name[MT_DNS_CLIENT_NAME_LEN];
} mt_dns_client_t;

typedef struct mt_dns_clients mt_dns_clients_t;
typedef void (*mt_dns_clients_notify_fn)(void *ud);
/* Own *clients on success. Outputs must be NULL/0 on failure. Runs off-loop. */
typedef mt_err_t (*mt_dns_clients_fetch_fn)(mt_dns_client_t **clients, size_t *count,
                                          const atomic_bool *stop, void *ud);

/* Canonical text; only IPv4-mapped IPv6 is collapsed to IPv4. No DNS lookup. */
bool mt_dns_client_address(const char *address, char *out, size_t size);
/* Copy a bounded, printable UTF-8 name without truncating a code point. */
bool mt_dns_client_label(const char *name, char out[MT_DNS_CLIENT_NAME_LEN]);

#ifdef MT_ENTWARE_KN
mt_err_t mt_dns_clients_parse_hotspot(const char *json, size_t len,
                                     mt_dns_client_t **clients, size_t *count);
mt_err_t mt_dns_clients_fetch_keenetic(mt_dns_client_t **clients, size_t *count,
                                      const atomic_bool *stop, void *ud);
#endif

/* NULL fetch selects the compiled provider. Generic builds succeed with
 * *out == NULL: no worker, router requests, or implicit platform detection.
 * Create after blocking daemon signals; destroy after HTTP streams close. */
mt_err_t mt_dns_clients_create(mt_loop_t *loop, mt_dns_clients_fetch_fn fetch,
                               void *fetch_ud, mt_dns_clients_t **out);
void mt_dns_clients_destroy(mt_dns_clients_t *clients);
/* Everything below runs on the loop thread. Subscribers share one fetch. */
int mt_dns_clients_subscribe(mt_dns_clients_t *clients, mt_dns_clients_notify_fn fn, void *ud);
void mt_dns_clients_unsubscribe(mt_dns_clients_t *clients, int subscription);
void mt_dns_clients_refresh(mt_dns_clients_t *clients);
/* Loop-local inventory view for bounded metadata events. Never retain these
 * pointers across callbacks; check revision again before the next batch. */
size_t mt_dns_clients_count(const mt_dns_clients_t *clients);
const mt_dns_client_t *mt_dns_clients_at(const mt_dns_clients_t *clients, size_t index);
uint64_t mt_dns_clients_revision(const mt_dns_clients_t *clients);
const char *mt_dns_clients_lookup(const mt_dns_clients_t *clients, const char *address);

#endif
