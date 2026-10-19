/* Compiled only for Entware _kn. Read-only local RCI; never called on the DNS loop. */
#include "dns_clients_internal.h"
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>
#include <curl/curl.h>

mt_err_t mt_dns_clients_parse_hotspot(const char *json, size_t len,
                                     mt_dns_client_t **clients, size_t *count)
{
    if (!clients || !count) { return MT_ERR_INVAL; }
    *clients = NULL; *count = 0;
    if (len > MT_DNS_CLIENT_BODY_LIMIT) { return MT_ERR_LIMIT; }
    if (!json || !len || memchr(json, 0, len)) { return MT_ERR_PROTO; }
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, len, &end, false);
    while (end && end < json + len && (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t')) { end++; }
    if (!root || end != json + len) { cJSON_Delete(root); return MT_ERR_PROTO; }
    if (cJSON_GetObjectItemCaseSensitive(root, "status")) { cJSON_Delete(root); return MT_ERR_UPSTREAM; }
    const cJSON *hosts = cJSON_IsArray(root) ? root : cJSON_GetObjectItemCaseSensitive(root, "host");
    /* Some firmware versions return {} when the host inventory is empty. */
    if (!hosts && cJSON_IsObject(root) && cJSON_GetArraySize(root) == 0) { cJSON_Delete(root); return MT_OK; }
    bool single = cJSON_IsObject(hosts) && cJSON_GetObjectItemCaseSensitive(hosts, "ip");
    if (!cJSON_IsArray(hosts) && !single) { cJSON_Delete(root); return MT_ERR_PROTO; }
    if (!single && (unsigned)cJSON_GetArraySize(hosts) > MT_DNS_CLIENT_LIMIT) {
        cJSON_Delete(root); return MT_ERR_LIMIT;
    }
    mt_dns_client_table_t table = {0};
    mt_err_t err = MT_OK;
    const cJSON *host = single ? hosts : hosts->child;
    for (; host; host = single ? NULL : host->next) {
        if (!cJSON_IsObject(host)) { continue; }
        const cJSON *active = cJSON_GetObjectItemCaseSensitive(host, "active");
        if (cJSON_IsFalse(active)) { continue; }
        const cJSON *ip = cJSON_GetObjectItemCaseSensitive(host, "ip");
        if (!cJSON_IsString(ip)) { continue; }
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(host, "name");
        const cJSON *hostname = cJSON_GetObjectItemCaseSensitive(host, "hostname");
        char label[MT_DNS_CLIENT_NAME_LEN];
        /* Keenetic's user-defined Client Lists name is distinct from hostname. */
        if (!(cJSON_IsString(name) && mt_dns_client_label(name->valuestring, label)) &&
            !(cJSON_IsString(hostname) && mt_dns_client_label(hostname->valuestring, label))) { continue; }
        err = mt_dns_client_table_add(&table, ip->valuestring, label);
        if (err != MT_OK) { break; }
    }
    cJSON_Delete(root);
    if (err != MT_OK) { free(table.items); return err; }
    mt_dns_client_table_finish(&table, clients, count);
    return MT_OK;
}

typedef struct { char *data; size_t len; mt_err_t error; } client_response_t;
static size_t receive_clients(char *data, size_t size, size_t count, void *ud)
{
    client_response_t *r = ud;
    if (count && size > SIZE_MAX / count) { r->error = MT_ERR_LIMIT; return 0; }
    size_t n = size * count;
    if (n > MT_DNS_CLIENT_BODY_LIMIT - r->len) { r->error = MT_ERR_LIMIT; return 0; }
    char *next = realloc(r->data, r->len + n + 1);
    if (!next) { r->error = MT_ERR_NOMEM; return 0; }
    r->data = next;
    memcpy(next + r->len, data, n); r->len += n; next[r->len] = '\0';
    return n;
}
/* libcurl owns only an opaque pointer, not the read-only cancellation flag.
 * This stack context outlives the synchronous transfer and its callbacks. */
typedef struct { const atomic_bool *stop; } client_progress_t;
static int fetch_progress(void *ud, curl_off_t total, curl_off_t now, curl_off_t up_total, curl_off_t up_now)
{
    (void)total; (void)now; (void)up_total; (void)up_now;
    const client_progress_t *ctx = ud;
    return atomic_load(ctx->stop) ? 1 : 0;
}
mt_err_t mt_dns_clients_fetch_keenetic(mt_dns_client_t **clients, size_t *count,
                                      const atomic_bool *stop, void *ud)
{
    (void)ud;
    *clients = NULL; *count = 0;
    if (stop && atomic_load(stop)) { return MT_ERR_CANCELED; }
    CURL *curl = curl_easy_init();
    if (!curl) { return MT_ERR_NOMEM; }
    client_response_t response = {0};
    const client_progress_t progress = {.stop = stop};
    CURLcode setup = CURLE_OK;
#define SET(option, value) do { if (setup == CURLE_OK) { setup = curl_easy_setopt(curl, option, value); } } while (0)
    SET(CURLOPT_URL, "http://127.0.0.1:79/rci/show/ip/hotspot");
    SET(CURLOPT_PROXY, "");
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_CONNECTTIMEOUT_MS, 500L);
    SET(CURLOPT_TIMEOUT_MS, 2000L);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_WRITEFUNCTION, receive_clients);
    SET(CURLOPT_WRITEDATA, &response);
    /* Install progress reporting only with a live cancellation flag. A NULL
     * flag keeps libcurl's default (progress disabled) for uncancelled reads. */
    if (stop) {
        SET(CURLOPT_XFERINFOFUNCTION, fetch_progress);
        SET(CURLOPT_XFERINFODATA, &progress);
        SET(CURLOPT_NOPROGRESS, 0L);
    }
#undef SET
    CURLcode result = setup == CURLE_OK ? curl_easy_perform(curl) : setup;
    long status = 0;
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    mt_err_t err = response.error;
    if (err == MT_OK && result != CURLE_OK) {
        err = result == CURLE_ABORTED_BY_CALLBACK ? MT_ERR_CANCELED :
              result == CURLE_OPERATION_TIMEDOUT ? MT_ERR_TIMEOUT : MT_ERR_UPSTREAM;
    }
    if (err == MT_OK && status != 200) { err = MT_ERR_UPSTREAM; }
    if (err == MT_OK) { err = mt_dns_clients_parse_hotspot(response.data, response.len, clients, count); }
    free(response.data);
    return err;
}
