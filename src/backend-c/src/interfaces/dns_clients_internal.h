#ifndef MT_DNS_CLIENTS_INTERNAL_H
#define MT_DNS_CLIENTS_INTERNAL_H
#include "magitrickle/dns_clients.h"
typedef struct {
    mt_dns_client_t *items;
    size_t count, capacity;
} mt_dns_client_table_t;
mt_err_t mt_dns_client_table_add(mt_dns_client_table_t *table, const char *ip, const char *name);
void mt_dns_client_table_finish(mt_dns_client_table_t *table, mt_dns_client_t **out, size_t *count);
#endif
