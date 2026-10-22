/* Compiled only for Entware _kn. Read-only local RCI; never called on the DNS loop. */
#include "dns_clients_internal.h"
#include "magitrickle/dns_neighbors.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <cjson/cJSON.h>
#include <curl/curl.h>

static int hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}
static bool parse_mac(const cJSON *value, unsigned char mac[6])
{
    if (!cJSON_IsString(value) || strnlen(value->valuestring, 18) != 17) { return false; }
    const unsigned char *text = (const unsigned char *)value->valuestring;
    unsigned nonzero = 0;
    for (size_t i = 0; i < 6; i++) {
        int hi = hex_digit(text[i * 3]), lo = hex_digit(text[i * 3 + 1]);
        if (hi < 0 || lo < 0 || (i < 5 && text[i * 3 + 2] != ':')) { return false; }
        mac[i] = (unsigned char)((unsigned)hi * 16u + (unsigned)lo);
        nonzero |= mac[i];
    }
    return nonzero && !(mac[0] & 1u);
}
static mt_err_t parse_hotspot(const char *json, size_t len,
                              const mt_dns_neighbor_t *neighbors, size_t neighbor_count,
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
    /* Some firmware versions return {} when the host inventory is empty.
     * Reject any other response without 'host' before accessing its children. */
    if (!hosts) {
        bool empty = cJSON_IsObject(root) && cJSON_GetArraySize(root) == 0;
        cJSON_Delete(root);
        return empty ? MT_OK : MT_ERR_PROTO;
    }
    bool single = cJSON_IsObject(hosts) && (cJSON_GetObjectItemCaseSensitive(hosts, "ip") ||
                                           cJSON_GetObjectItemCaseSensitive(hosts, "mac"));
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
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(host, "name");
        const cJSON *hostname = cJSON_GetObjectItemCaseSensitive(host, "hostname");
        char label[MT_DNS_CLIENT_NAME_LEN];
        /* Keenetic's user-defined Client Lists name is distinct from hostname. */
        if (!(cJSON_IsString(name) && mt_dns_client_label(name->valuestring, label)) &&
            !(cJSON_IsString(hostname) && mt_dns_client_label(hostname->valuestring, label))) { continue; }
        if (cJSON_IsString(ip)) { err = mt_dns_client_table_add(&table, ip->valuestring, label); }
        if (err != MT_OK) { break; }
        unsigned char mac[6];
        if (!neighbor_count || !parse_mac(cJSON_GetObjectItemCaseSensitive(host, "mac"), mac)) { continue; }
        /* Only exact kernel-observed MACs join addresses; no EUI-64/vendor/PTR guesses.
         * The finished snapshot is MAC-sorted and excludes ambiguous IP scopes. */
        size_t lo = 0, hi = neighbor_count;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (memcmp(neighbors[mid].mac, mac, sizeof(mac)) < 0) { lo = mid + 1; }
            else { hi = mid; }
        }
        for (; lo < neighbor_count && memcmp(neighbors[lo].mac, mac, sizeof(mac)) == 0; lo++) {
            err = mt_dns_client_table_add(&table, neighbors[lo].ip, label);
            if (err != MT_OK) { break; }
        }
        if (err != MT_OK) { break; }
    }
    cJSON_Delete(root);
    if (err != MT_OK) { free(table.items); return err; }
    mt_dns_client_table_finish(&table, clients, count);
    return MT_OK;
}

mt_err_t mt_dns_clients_parse_hotspot(const char *json, size_t len,
                                     mt_dns_client_t **clients, size_t *count)
{
    return parse_hotspot(json, len, NULL, 0, clients, count);
}
mt_err_t mt_dns_clients_parse_hotspot_neighbors(const char *json, size_t len,
                                                const mt_dns_neighbor_t *neighbors,
                                                size_t neighbor_count,
                                                mt_dns_client_t **clients, size_t *count)
{
    if (!clients || !count) { return MT_ERR_INVAL; }
    *clients = NULL; *count = 0;
    if (neighbor_count && !neighbors) { return MT_ERR_INVAL; }
    if (neighbor_count > MT_DNS_NEIGHBOR_LIMIT) { return MT_ERR_LIMIT; }
    mt_err_t err = parse_hotspot(json, len, neighbors, neighbor_count, clients, count);
    /* Never publish a partial join: a skipped conflicting host could attach
     * the wrong name. Optional enrichment must not erase valid IPv4 labels. */
    if (neighbor_count && (err == MT_ERR_LIMIT || err == MT_ERR_NOMEM)) {
        err = mt_dns_clients_parse_hotspot(json, len, clients, count);
    }
    return err;
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
static mt_err_t fetch_hotspot(client_response_t *out, const atomic_bool *stop)
{
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
    if (err == MT_OK) { *out = response; }
    else { free(response.data); }
    return err;
}
void mt_dns_hotspot_cache_clear(mt_dns_hotspot_cache_t *cache)
{
    free(cache->json);
    free(cache->observed);
    *cache = (mt_dns_hotspot_cache_t){0};
}
static bool unknown_mac(const mt_dns_hotspot_cache_t *cache,
                        const mt_dns_neighbor_t *neighbors, size_t count)
{
    cJSON *root = cJSON_ParseWithLength(cache->json, cache->len);
    if (!root) { return true; }
    const cJSON *hosts = cJSON_IsArray(root) ? root : cJSON_GetObjectItemCaseSensitive(root, "host");
    bool single = cJSON_IsObject(hosts);
    bool known[MT_DNS_NEIGHBOR_LIMIT] = {false};
    for (const cJSON *host = single ? hosts : hosts ? hosts->child : NULL;
         host; host = single ? NULL : host->next) {
        unsigned char mac[6];
        if (!parse_mac(cJSON_GetObjectItemCaseSensitive(host, "mac"), mac)) { continue; }
        size_t lo = 0, hi = count;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (memcmp(neighbors[mid].mac, mac, 6) < 0) { lo = mid + 1; }
            else { hi = mid; }
        }
        for (; lo < count && memcmp(neighbors[lo].mac, mac, 6) == 0; lo++) { known[lo] = true; }
    }
    cJSON_Delete(root);
    for (size_t i = 0; i < count; i++) {
        if (known[i]) { continue; }
        size_t lo = 0, hi = cache->observed_count;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (memcmp(cache->observed[mid], neighbors[i].mac, 6) < 0) { lo = mid + 1; }
            else { hi = mid; }
        }
        if (lo == cache->observed_count || memcmp(cache->observed[lo], neighbors[i].mac, 6)) { return true; }
    }
    return false;
}
mt_err_t mt_dns_clients_fetch_keenetic_cached(mt_dns_client_t **clients, size_t *count,
    const atomic_bool *stop, mt_dns_hotspot_cache_t *cache)
{
    *clients = NULL; *count = 0;
    mt_dns_neighbor_t *neighbors = NULL;
    size_t neighbor_count = 0;
    mt_err_t discovery = mt_dns_neighbors_read(&neighbors, &neighbor_count, stop);
    if (discovery == MT_ERR_CANCELED) { return discovery; }
    struct timespec ts;
    uint64_t now = clock_gettime(CLOCK_MONOTONIC, &ts) == 0 ?
        (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u : UINT64_MAX;
    mt_err_t err = MT_OK;
    bool unknown = cache->json && unknown_mac(cache, neighbors, neighbor_count);
    cache->retry_pending = (!cache->json || unknown) && now < cache->retry_after;
    if (!cache->json && cache->retry_pending) { free(neighbors); return MT_ERR_AGAIN; }
    if (!cache->json || (unknown && !cache->retry_pending)) {
        client_response_t response = {0};
        err = fetch_hotspot(&response, stop);
        mt_dns_hotspot_cache_clear(cache);
        if (err == MT_OK) {
            cache->json = response.data; cache->len = response.len;
            if (neighbor_count) {
                cache->observed = malloc(neighbor_count * sizeof(*cache->observed));
                if (cache->observed) {
                    for (size_t i = 0; i < neighbor_count; i++) { memcpy(cache->observed[i], neighbors[i].mac, 6); }
                    cache->observed_count = neighbor_count;
                }
            }
        }
        /* Unknown/unnamed neighbors must not cause an RCI request storm. */
        cache->retry_after = now == UINT64_MAX ? UINT64_MAX : now + 5000u;
    }
    if (err == MT_OK) {
        err = mt_dns_clients_parse_hotspot_neighbors(cache->json, cache->len,
            neighbors, neighbor_count, clients, count);
    }
    if (err != MT_OK) {
        mt_dns_hotspot_cache_clear(cache);
        cache->retry_after = now == UINT64_MAX ? UINT64_MAX : now + 5000u;
    }
    free(neighbors);
    return err;
}
mt_err_t mt_dns_clients_fetch_keenetic(mt_dns_client_t **clients, size_t *count,
                                      const atomic_bool *stop, void *ud)
{
    (void)ud;
    mt_dns_hotspot_cache_t cache = {0};
    mt_err_t err = mt_dns_clients_fetch_keenetic_cached(clients, count, stop, &cache);
    mt_dns_hotspot_cache_clear(&cache);
    return err;
}
