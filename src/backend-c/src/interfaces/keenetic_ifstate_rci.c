/* Only snapshots, never the normal event path. Does not create/configure
 * interfaces. POST is the read-only show/interface system-name batch. */
#include "magitrickle/keenetic_ifstate_service.h"
#include "magitrickle/keenetic_rci.h"
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

typedef struct { char *data; size_t size; mt_err_t error; } body_t;
static size_t receive_body(char *data, size_t size, size_t count, void *ud)
{
    body_t *body = ud;
    if (count && size > SIZE_MAX / count) { body->error = MT_ERR_LIMIT; return 0; }
    size_t n = size * count;
    if (n > MT_KN_IFSTATE_BODY_LIMIT - body->size) { body->error = MT_ERR_LIMIT; return 0; }
    char *next = realloc(body->data, body->size + n + 1);
    if (!next) { body->error = MT_ERR_NOMEM; return 0; }
    body->data = next;
    memcpy(next + body->size, data, n);
    body->size += n; next[body->size] = '\0';
    return n;
}
static int progress(void *ud, curl_off_t total, curl_off_t now, curl_off_t up_total, curl_off_t up_now)
{
    (void)total; (void)now; (void)up_total; (void)up_now;
    const atomic_bool *stop = ud;
    return stop && atomic_load(stop) ? 1 : 0;
}
static mt_err_t read_rci(const char *url, const char *post, const atomic_bool *stop, body_t *body)
{
    if (stop && atomic_load(stop)) { return MT_ERR_CANCELED; }
    CURL *curl = curl_easy_init();
    if (!curl) { return MT_ERR_NOMEM; }
    struct curl_slist *headers = NULL;
    if (post) {
        headers = curl_slist_append(NULL, "Content-Type: application/json");
        if (!headers) { curl_easy_cleanup(curl); return MT_ERR_NOMEM; }
    }
    CURLcode setup = CURLE_OK;
#define SET(option, value) do { if (setup == CURLE_OK) { setup = curl_easy_setopt(curl, option, value); } } while (0)
    SET(CURLOPT_URL, url);
    SET(CURLOPT_PROXY, "");
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_CONNECTTIMEOUT_MS, 500L);
    SET(CURLOPT_TIMEOUT_MS, 2000L);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_WRITEFUNCTION, receive_body);
    SET(CURLOPT_WRITEDATA, body);
    SET(CURLOPT_XFERINFOFUNCTION, progress);
    SET(CURLOPT_XFERINFODATA, stop);
    SET(CURLOPT_NOPROGRESS, 0L);
    if (post) {
        SET(CURLOPT_HTTPHEADER, headers);
        SET(CURLOPT_POSTFIELDS, post);
        SET(CURLOPT_POSTFIELDSIZE, (long)strlen(post));
    }
#undef SET
    CURLcode result = setup == CURLE_OK ? curl_easy_perform(curl) : setup;
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    if (body->error != MT_OK) { return body->error; }
    if (stop && atomic_load(stop)) { return MT_ERR_CANCELED; }
    if (result == CURLE_OPERATION_TIMEDOUT) { return MT_ERR_TIMEOUT; }
    if (result != CURLE_OK || status != 200) { return MT_ERR_UPSTREAM; }
    return MT_OK;
}
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
    if (!body->data || memchr(body->data, '\0', body->size)) { return MT_ERR_PROTO; }
    for (size_t i = 0; i < body->size; i++) {
        if (body->data[i] != '\\') { continue; }
        if (body->size - i >= 6 && !memcmp(body->data + i, "\\u0000", 6)) { return MT_ERR_PROTO; }
        if (i + 1 < body->size) { i++; }
    }
    cJSON *root = cJSON_ParseWithLengthOpts(body->data, body->size + 1, NULL, true);
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
    mt_err_t err = read_rci("http://127.0.0.1:79/rci/show/interface", NULL, stop, &list);
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
        err = read_rci("http://127.0.0.1:79/rci/", request, stop, &names);
        free(request);
        if (err == MT_OK) { err = fill_names(&names, data, n); }
        free(names.data);
    }
    if (err != MT_OK) { free(data); return err; }
    *items = data; *count = n;
    return MT_OK;
}
