/* NDM interface readiness, not the include/exclude device-policy filter.
 * Loop-owned state. Only profile candidates consult it; missing metadata
 * defers to the existing kernel checks. No code/storage in non-KN builds. */
#ifndef MAGITRICKLE_KEENETIC_IFSTATE_H
#define MAGITRICKLE_KEENETIC_IFSTATE_H
#ifdef MT_ENTWARE_KN
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "magitrickle/err.h"

#define MT_KN_IFSTATE_LIMIT 256u
#define MT_KN_IFSTATE_BODY_LIMIT (1024u * 1024u)
#define MT_KN_IFSTATE_HOOK_PATH "/api/v1/system/hooks/ndm-interface"
#define MT_KN_IFSTATE_LOST_PATH "/opt/var/run/magitrickle-ndm-lost"

typedef enum {
    MT_KN_IF_UNSET, MT_KN_IF_RUNNING, MT_KN_IF_BLOCKED, MT_KN_IF_UNKNOWN
} mt_kn_iflevel_t;
typedef enum {
    MT_KN_IF_CONF, MT_KN_IF_LINK, MT_KN_IF_IPV4, MT_KN_IF_IPV6, MT_KN_IF_CTRL,
    MT_KN_IF_UP, MT_KN_IF_LEGACY_LINK, MT_KN_IF_CONNECTED, MT_KN_IF_FIELDS
} mt_kn_iffield_t;

typedef struct {
    char id[64];
    char name[16]; /* system-name, never derived from a description/NDM ID */
    char alias[64]; /* optional display label from the same RCI snapshot */
    mt_kn_iflevel_t fields[MT_KN_IF_FIELDS];
    bool layered;
} mt_kn_ifinfo_t;

typedef enum {
    MT_KN_IF_LAYER, MT_KN_IF_LEGACY, MT_KN_IF_CREATED, MT_KN_IF_DESTROYED
} mt_kn_ifevent_kind_t;
typedef struct {
    mt_kn_ifinfo_t info;
    mt_kn_ifevent_kind_t kind;
    bool resync; /* a hook reported a prior delivery failure/unknown field */
} mt_kn_ifevent_t;

typedef struct mt_kn_ifstate mt_kn_ifstate_t;
struct mt_app;
/* Bind once during daemon startup, before enabling groups. The cache remains
 * borrowed until app destruction. These entry points do not exist on generic. */
void mt_app_set_interface_states(struct mt_app *app, const mt_kn_ifstate_t *state);
mt_err_t mt_app_reconcile_profiles(struct mt_app *app, const char *name);
/* Called on the loop only, and only when effective availability or identity
 * changes. name is borrowed for this call. Consumers must coalesce/retry. */
typedef void (*mt_kn_ifchanged_fn)(void *ud, const char *name);
mt_kn_ifstate_t *mt_kn_ifstate_new(mt_kn_ifchanged_fn changed, void *ud);
void mt_kn_ifstate_free(mt_kn_ifstate_t *state);
bool mt_kn_ifstate_blocks(const mt_kn_ifstate_t *state, const char *name, int family);
/* Borrowed loop-owned label; NULL for absent/removed/redundant labels. */
const char *mt_kn_ifstate_alias(const mt_kn_ifstate_t *state, const char *name);
uint64_t mt_kn_ifstate_revision(const mt_kn_ifstate_t *state);
/* Event is already parsed/validated. resync tells the owner to request a
 * one-off snapshot (new interface without a baseline, unknown fields, loss). */
mt_err_t mt_kn_ifstate_event(mt_kn_ifstate_t *state, const mt_kn_ifevent_t *event,
                            bool *resync);
/* token must be captured BEFORE starting the RCI read: newer per-field events
 * win over its response. A failed read must not call this function. */
mt_err_t mt_kn_ifstate_snapshot(mt_kn_ifstate_t *state, const mt_kn_ifinfo_t *items,
                               size_t count, uint64_t token);
mt_err_t mt_kn_ifevent_parse(const char *body, size_t len, mt_kn_ifevent_t *out);
/* Returns the complete detached NDM inventory, possibly without kernel names
 * on old firmware. Caller frees it. Unknown field values are not "down". */
mt_err_t mt_kn_ifinfo_parse(const char *body, size_t len, mt_kn_ifinfo_t **out, size_t *count);
#endif
#endif
