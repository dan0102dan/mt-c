/* Independent settings/runtime controller for opt-in adaptive TCP routing. */
#ifndef MAGITRICKLE_ADAPTIVE_H
#define MAGITRICKLE_ADAPTIVE_H

#include "magitrickle/app.h"
#include "magitrickle/httpd.h"
#include "magitrickle/settings.h"

typedef struct mt_adaptive_service mt_adaptive_service_t;

cJSON *mt_adaptive_config_json(const mt_adaptive_config_t *config);
mt_err_t mt_adaptive_config_parse(const cJSON *json, mt_adaptive_config_t *out);

/* Startup failures remain visible through this controller's API and do not
 * take down DNS or existing rules. All methods run on the loop owner. */
mt_err_t mt_adaptive_service_create(mt_loop_t *loop, mt_app_t *app, mt_config_t *config,
                                    mt_settings_t *settings, const char *config_path,
                                    mt_adaptive_service_t **out);
mt_err_t mt_adaptive_service_destroy(mt_adaptive_service_t *service);
mt_err_t mt_adaptive_service_reload(mt_adaptive_service_t *service);
mt_err_t mt_adaptive_register_routes(mt_httpd_t *http, mt_adaptive_service_t *service);

#endif
