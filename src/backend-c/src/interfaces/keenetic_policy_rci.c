#include "magitrickle/keenetic_policy_rci.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <cjson/cJSON.h>
#include "magitrickle/keenetic_rci.h"
#include "magitrickle/json.h"

static bool parse_mark(const cJSON *value, uint32_t *out)
{
    if (cJSON_IsNumber(value)) {
        double n = value->valuedouble;
        if (!isfinite(n) || n < 0 || n > MT_KN_POLICY_MASK || n != floor(n)) { return false; }
        *out = (uint32_t)n;
        return true;
    }
    if (!cJSON_IsString(value)) { return false; }
    const char *s = value->valuestring;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; }
    size_t n = strlen(s);
    if (!n || n > 8) { return false; }
    uint32_t mark = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        uint32_t d;
        if (c >= '0' && c <= '9') { d = (uint32_t)(c - '0'); }
        else if (c >= 'a' && c <= 'f') { d = (uint32_t)(c - 'a') + 10u; }
        else if (c >= 'A' && c <= 'F') { d = (uint32_t)(c - 'A') + 10u; }
        else { return false; }
        mark = mark * 16u + d;
    }
    if (mark > MT_KN_POLICY_MASK) { return false; }
    *out = mark;
    return true;
}

mt_err_t mt_kn_policy_parse(const char *json, size_t len, mt_kn_policy_entry_t **entries, size_t *count)
{
    *entries = NULL; *count = 0;
    if (!json || !len) { return MT_ERR_PROTO; }
    if (len > MT_KN_POLICY_BODY_LIMIT) { return MT_ERR_LIMIT; }
    cJSON *root = mt_json_parse_document(json, len);
    if (!root || !cJSON_IsObject(root)) { cJSON_Delete(root); return MT_ERR_PROTO; }
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
    if (status) { cJSON_Delete(root); return MT_ERR_UPSTREAM; }
    int number = cJSON_GetArraySize(root);
    if (number < 0 || (unsigned)number > MT_KN_POLICY_LIST_LIMIT) { cJSON_Delete(root); return MT_ERR_LIMIT; }
    /* An empty inventory owns no allocation. Keep it out of the population
     * path so that every entry below has non-NULL storage. */
    if (number == 0) { cJSON_Delete(root); return MT_OK; }
    mt_kn_policy_entry_t *out = calloc((size_t)number, sizeof(*out));
    if (!out) { cJSON_Delete(root); return MT_ERR_NOMEM; }
    size_t n = 0;
    mt_err_t err = MT_OK;
    const cJSON *item;
    cJSON_ArrayForEach(item, root) {
        if (!cJSON_IsObject(item) || !item->string || !*item->string ||
            strlen(item->string) >= sizeof(out[n].id)) { err = MT_ERR_PROTO; break; }
        mt_kn_policy_config_t test = {.enabled = true};
        strcpy(test.policy_id, item->string);
        if (!mt_kn_policy_config_valid(&test)) { err = MT_ERR_PROTO; break; }
        for (size_t i = 0; i < n; i++) {
            if (strcmp(out[i].id, item->string) == 0) { err = MT_ERR_PROTO; break; }
        }
        if (err != MT_OK) { break; }
        strcpy(out[n].id, item->string);
        const cJSON *description = cJSON_GetObjectItemCaseSensitive(item, "description");
        if (description && !cJSON_IsString(description)) { err = MT_ERR_PROTO; break; }
        const char *label = description ? description->valuestring : item->string;
        if (strlen(label) >= sizeof(out[n].description)) { err = MT_ERR_LIMIT; break; }
        strcpy(out[n].description, label);
        const cJSON *mark = cJSON_GetObjectItemCaseSensitive(item, "mark");
        if (mark && !cJSON_IsNull(mark)) {
            if (!parse_mark(mark, &out[n].mark)) { err = MT_ERR_PROTO; break; }
            out[n].available = true;
        }
        n++;
    }
    cJSON_Delete(root);
    if (err != MT_OK) { free(out); return err; }
    *entries = out; *count = n;
    return MT_OK;
}

mt_err_t mt_kn_policy_fetch(mt_kn_policy_entry_t **entries, size_t *count, const atomic_bool *stop)
{
    *entries = NULL; *count = 0;
    char *body = NULL; size_t length = 0;
    mt_err_t err = mt_kn_rci_request(MT_KN_RCI_BASE_URL "/rci/show/ip/policy", NULL,
                                     MT_KN_POLICY_BODY_LIMIT, stop, &body, &length);
    if (err == MT_OK) { err = mt_kn_policy_parse(body, length, entries, count); }
    free(body);
    return err;
}
