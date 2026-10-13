#ifndef MAGITRICKLE_KEENETIC_IFSTATE_SERVICE_H
#define MAGITRICKLE_KEENETIC_IFSTATE_SERVICE_H
#ifdef MT_ENTWARE_KN
#include <stdatomic.h>
#include "magitrickle/keenetic_ifstate.h"
#include "magitrickle/httpd.h"
#include "magitrickle/loop.h"

typedef struct mt_kn_ifstate_service mt_kn_ifstate_service_t;
typedef mt_err_t (*mt_kn_ifstate_fetch_fn)(mt_kn_ifinfo_t **items, size_t *count,
                                          const atomic_bool *stop);
mt_err_t mt_kn_ifstate_fetch(mt_kn_ifinfo_t **items, size_t *count, const atomic_bool *stop);
/* cache and loop are borrowed, must outlive the service. A NULL fetch selects
 * the production read-only RCI fetch. NULL marker disables filesystem watching
 * for embeddings/tests. Production passes MT_KN_IFSTATE_LOST_PATH. */
mt_err_t mt_kn_ifstate_service_create(mt_loop_t *loop, mt_kn_ifstate_t *cache,
                                     const char *marker, mt_kn_ifstate_fetch_fn fetch,
                                     mt_kn_ifstate_service_t **out);
void mt_kn_ifstate_service_destroy(mt_kn_ifstate_service_t *service);
void mt_kn_ifstate_service_resync(mt_kn_ifstate_service_t *service);
/* Register on the trusted Unix listener ONLY, never the TCP/WebUI listener. */
mt_err_t mt_kn_ifstate_register_hook(mt_httpd_t *unix_http, mt_kn_ifstate_service_t *service);
#endif
#endif
