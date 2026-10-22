/* Keenetic neighbour.d events invalidate optional display metadata only. */
#include "magitrickle/dns_keenetic_hook.h"
#ifdef MT_ENTWARE_KN
static void neighbour_changed(mt_http_req_t *req, mt_http_res_t *res, void *ud)
{
    (void)req;
    mt_dns_clients_invalidate(ud);
    /* Acknowledge receipt without waiting for the debounced RCI snapshot. */
    mt_http_res_write(res, 202, NULL, NULL, 0);
}

mt_err_t mt_dns_keenetic_register_hook(mt_httpd_t *unix_http, mt_dns_clients_t *clients)
{
    if (!unix_http || !clients) { return MT_ERR_INVAL; }
    return mt_httpd_route(unix_http, "POST", MT_DNS_KEENETIC_HOOK_PATH, neighbour_changed, clients);
}
#endif
