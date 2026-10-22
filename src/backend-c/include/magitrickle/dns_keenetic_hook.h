#ifndef MAGITRICKLE_DNS_KEENETIC_HOOK_H
#define MAGITRICKLE_DNS_KEENETIC_HOOK_H
#ifdef MT_ENTWARE_KN
#include "magitrickle/dns_clients.h"
#include "magitrickle/httpd.h"

#define MT_DNS_KEENETIC_HOOK_PATH "/api/v1/system/hooks/ndm-neighbour"

/* Register only on the trusted Unix listener. clients is borrowed and must
 * outlive that listener; notifications carry no device data or identity. */
mt_err_t mt_dns_keenetic_register_hook(mt_httpd_t *unix_http, mt_dns_clients_t *clients);
#endif
#endif
