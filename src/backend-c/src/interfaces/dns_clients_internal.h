#ifndef MT_DNS_CLIENTS_INTERNAL_H
#define MT_DNS_CLIENTS_INTERNAL_H
#include "magitrickle/dns_clients.h"
#define MT_DNS_CLIENT_SCOPE_LEN 64u
typedef struct {
    mt_dns_client_t client;
    char owner[18];
    char scope[MT_DNS_CLIENT_SCOPE_LEN];
} mt_dns_client_observation_t;
typedef struct {
    mt_dns_client_observation_t *items;
    size_t count, capacity;
} mt_dns_client_table_t;
bool mt_dns_client_mac(const char *text, char out[18]);
mt_err_t mt_dns_client_table_add(mt_dns_client_table_t *table, const char *ip, const char *name);
/* Keep unnamed owners as well: they can make another device's label unsafe.
 * Empty owner/scope means unknown; conflicting known values suppress the IP. */
mt_err_t mt_dns_client_table_add_owned(mt_dns_client_table_t *table, const char *ip,
    const char *name, const char *owner, const char *scope);
void mt_dns_client_table_finish(mt_dns_client_table_t *table, mt_dns_client_t **out, size_t *count);
#endif
