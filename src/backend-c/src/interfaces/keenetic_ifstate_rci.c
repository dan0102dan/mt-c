/* Only snapshots, never the normal event path. Does not create/configure
 * interfaces. POST is the read-only show/interface system-name batch. */
#include "magitrickle/keenetic_ifstate_service.h"
#include "magitrickle/keenetic_rci.h"
#include <stdlib.h>
#include <string.h>
#include "magitrickle/json.h"
#include <cjson/cJSON.h>

typedef struct { char *data; size_t size; } body_t;
static bool system_name_valid(const char *s)
{
    if (!s || !*s || strlen(s) >= 16) { return false; }
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.' || *p == ':') { continue; }
        return false;
    }
    return true;
}
static mt_err_t fill_names(const body_t *body, mt_kn_ifinfo_t *items, size_t count)
{
    cJSON *root = mt_json_parse_document(body->data, body->size);
    if (!cJSON_IsArray(root) || (size_t)cJSON_GetArraySize(root) != count) { cJSON_Delete(root); return MT_ERR_PROTO; }
    size_t i = 0;
    const cJSON *element;
    cJSON_ArrayForEach(element, root) {
        const cJSON *show = cJSON_GetObjectItemCaseSensitive(element, "show");
        const cJSON *iface = cJSON_GetObjectItemCaseSensitive(show, "interface");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(iface, "system-name");
        if (name) {
            if (!cJSON_IsString(name) || !system_name_valid(name->valuestring)) { cJSON_Delete(root); return MT_ERR_PROTO; }
            strcpy(items[i].name, name->valuestring);
        }
        /* Missing system-name/unsupported show subcommand: retain the NDM
         * record. Hooks supply the mapping on old firmware, without guessing. */
        i++;
    }
    cJSON_Delete(root);
    return MT_OK;
}
mt_err_t mt_kn_ifstate_fetch(mt_kn_ifinfo_t **items, size_t *count, const atomic_bool *stop)
{
    *items = NULL; *count = 0;
    body_t list = {0};
    mt_err_t err = mt_kn_rci_request(MT_KN_RCI_BASE_URL "/rci/show/interface", NULL,
                                      MT_KN_IFSTATE_BODY_LIMIT, stop, &list.data, &list.size);
    mt_kn_ifinfo_t *data = NULL;
    size_t n = 0;
    if (err == MT_OK) { err = mt_kn_ifinfo_parse(list.data, list.size, &data, &n); }
    free(list.data);
    if (err != MT_OK) { return err; }
    bool need_names = false;
    for (size_t i = 0; i < n; i++) { if (!data[i].name[0]) { need_names = true; break; } }
    if (need_names) {
        mt_kn_iface_meta_t *metas = calloc(n, sizeof(*metas));
        if (!metas) { free(data); return MT_ERR_NOMEM; }
        for (size_t i = 0; i < n; i++) { strcpy(metas[i].id, data[i].id); }
        char *request = mt_kn_build_system_name_request(metas, n);
        free(metas);
        if (!request) { free(data); return MT_ERR_NOMEM; }
        body_t names = {0};
        err = mt_kn_rci_request(MT_KN_RCI_BASE_URL "/rci/", request,
                                MT_KN_IFSTATE_BODY_LIMIT, stop, &names.data, &names.size);
        free(request);
        if (err == MT_OK) { err = fill_names(&names, data, n); }
        free(names.data);
    }
    if (err != MT_OK) { free(data); return err; }
    *items = data; *count = n;
    return MT_OK;
}
