/* General settings: a strict editable projection, independent of routing
 * collections. A controller outlives the daemon runtime it asks to recreate. */
#ifndef MAGITRICKLE_SETTINGS_H
#define MAGITRICKLE_SETTINGS_H

#include "magitrickle/dnsproxy.h"
#include "magitrickle/httpd.h"
#include "magitrickle/models.h"

typedef struct mt_settings mt_settings_t;
typedef mt_err_t (*mt_settings_live_apply_fn)(mt_app_config_t *candidate, void *userdata);

cJSON *mt_settings_json(const mt_app_config_t *app);
mt_err_t mt_settings_revision(const mt_app_config_t *app, char revision[65]);
/* On success out owns a clone of base with only the 20 public fields replaced.
 * On error out is empty; field identifies the invalid public field. */
mt_err_t mt_settings_parse(const cJSON *json, const mt_app_config_t *base,
                           mt_app_config_t *out, char field[96]);
/* Borrow live fields (including WebUI enable state) from source, retaining all other values/pointers.
 * The caller must transfer or retain ownership of source->log_level. */
void mt_settings_overlay_live(mt_app_config_t *target, const mt_app_config_t *source);

mt_settings_t *mt_settings_create(mt_config_t *config);
/* Call before destroying the attached loop; joins any preparation worker. */
void mt_settings_destroy(mt_settings_t *settings);
/* NULL detaches preparation before the old loop is destroyed. The controller
 * itself must remain alive until its HTTP responses have been torn down. */
void mt_settings_attach_loop(mt_settings_t *settings, mt_loop_t *loop);
/* Called on a later loop dispatch after ACK and revision/impact checks.
 * The callback may replace the WebUI listener and transfer candidate->log_level;
 * all other candidate data remains controller-owned. Failure must leave the
 * active configuration unchanged. */
void mt_settings_set_live_apply(mt_settings_t *settings, mt_settings_live_apply_fn apply,
                                void *userdata);
void mt_settings_bound_ports(mt_settings_t *settings, uint16_t http, uint16_t dns);
/* Recorded only after every runtime component started successfully. Legacy
 * reloads may change cfg without changing this actual-runtime baseline. */
void mt_settings_runtime_started(mt_settings_t *settings);
void mt_settings_runtime_stopped(mt_settings_t *settings);
void mt_settings_register_routes(mt_httpd_t *http, mt_settings_t *settings);
bool mt_settings_busy(const mt_settings_t *settings);
/* Ready only after a completely written acknowledgement, successful off-loop
 * DNS resolution and a second revision check against the latest live config. */
bool mt_settings_ready(const mt_settings_t *settings);
bool mt_settings_take(mt_settings_t *settings, mt_app_config_t *candidate,
                       mt_dns_upstream_t *upstream);
void mt_settings_finish(mt_settings_t *settings, const char *error, bool rollback_failed);

#endif
