/* Compiled only for Entware _kn. Read-only local RCI; never called on the DNS loop. */
#include "dns_clients_internal.h"
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>
#include <curl/curl.h>

typedef struct { const cJSON *first; bool single; } client_rows_t;

static const char *string_field(const cJSON *object, const char *key)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}
static mt_err_t parse_document(const char *json, size_t len, cJSON **out)
{
    *out = NULL;
    if (len > MT_DNS_CLIENT_BODY_LIMIT) { return MT_ERR_LIMIT; }
    if (!json || !len || memchr(json, 0, len)) { return MT_ERR_PROTO; }
    /* cJSON exposes NUL-terminated strings without their decoded lengths.
     * Reject encoded NULs before they can truncate a name, address or owner.
     * Skip escaped characters so a literal backslash followed by u0000 works. */
    for (size_t i = 0; i < len; i++) {
        if (json[i] != '\\') { continue; }
        if (len - i >= 6 && memcmp(json + i, "\\u0000", 6) == 0) { return MT_ERR_PROTO; }
        if (i + 1 < len) { i++; }
    }
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, len, &end, false);
    while (end && end < json + len && (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t')) { end++; }
    if (!root || end != json + len) { cJSON_Delete(root); return MT_ERR_PROTO; }
    if (cJSON_GetObjectItemCaseSensitive(root, "status")) { cJSON_Delete(root); return MT_ERR_UPSTREAM; }
    *out = root;
    return MT_OK;
}
static mt_err_t hotspot_rows(const cJSON *root, client_rows_t *rows)
{
    if (!root) { return MT_ERR_PROTO; }
    const cJSON *hosts = cJSON_IsArray(root) ? root : cJSON_GetObjectItemCaseSensitive(root, "host");
    if (!hosts) {
        return cJSON_IsObject(root) && !root->child ? MT_OK : MT_ERR_PROTO;
    }
    rows->single = cJSON_IsObject(hosts) &&
        (cJSON_GetObjectItemCaseSensitive(hosts, "ip") || cJSON_GetObjectItemCaseSensitive(hosts, "mac"));
    if (!rows->single && !cJSON_IsArray(hosts)) { return MT_ERR_PROTO; }
    if (!rows->single && (unsigned)cJSON_GetArraySize(hosts) > MT_DNS_CLIENT_LIMIT) { return MT_ERR_LIMIT; }
    rows->first = rows->single ? hosts : hosts->child;
    return MT_OK;
}
static mt_err_t neighbor_rows(const cJSON *root, client_rows_t *rows)
{
    if (!root || (!cJSON_IsArray(root) && !cJSON_IsObject(root))) { return MT_ERR_PROTO; }
    rows->single = cJSON_IsObject(root) && cJSON_GetObjectItemCaseSensitive(root, "address-family");
    if (rows->single) { rows->first = root; return MT_OK; }
    if ((unsigned)cJSON_GetArraySize(root) > MT_DNS_CLIENT_LIMIT) { return MT_ERR_LIMIT; }
    /* RCI show/ip/neighbour is normally an object indexed by neighbour ID.
     * Array and single-record responses carry the same record fields. */
    for (const cJSON *row = root->child; row; row = row->next) {
        if (!cJSON_IsObject(row)) { return MT_ERR_PROTO; }
        if (cJSON_IsObject(root)) {
            const char *key = row->string;
            if (!key || !*key) { return MT_ERR_PROTO; }
        }
    }
    rows->first = root->child;
    return MT_OK;
}
static bool host_active(const cJSON *host)
{
    const cJSON *active = cJSON_GetObjectItemCaseSensitive(host, "active");
    return !active || cJSON_IsTrue(active);
}
static const char *host_label(const cJSON *host, char label[MT_DNS_CLIENT_NAME_LEN])
{
    /* The Client Lists alias is distinct from the DHCP hostname. */
    return mt_dns_client_label(string_field(host, "name"), label) ||
           mt_dns_client_label(string_field(host, "hostname"), label) ? label : NULL;
}
static const char *host_scope(const cJSON *host)
{
    const cJSON *interface = cJSON_GetObjectItemCaseSensitive(host, "interface");
    const char *scope = cJSON_IsObject(interface) ? string_field(interface, "id") : NULL;
    return scope && *scope && strnlen(scope, MT_DNS_CLIENT_SCOPE_LEN) < MT_DNS_CLIENT_SCOPE_LEN ? scope : NULL;
}
static mt_err_t add_hotspot(mt_dns_client_table_t *table, const client_rows_t *hosts)
{
    for (const cJSON *host = hosts->first; host; host = hosts->single ? NULL : host->next) {
        if (!cJSON_IsObject(host)) { return MT_ERR_PROTO; }
        const cJSON *active = cJSON_GetObjectItemCaseSensitive(host, "active");
        if (active && !cJSON_IsBool(active)) { return MT_ERR_PROTO; }
        if (!host_active(host)) { continue; }
        const char *scope = host_scope(host);
        char label[MT_DNS_CLIENT_NAME_LEN], mac[18];
        bool owned = mt_dns_client_mac(string_field(host, "mac"), mac);
        mt_err_t err = mt_dns_client_table_add_owned(table, string_field(host, "ip"),
            host_label(host, label), owned ? mac : NULL, scope);
        if (err != MT_OK) { return err; }
    }
    return MT_OK;
}
static mt_err_t add_neighbor_address(mt_dns_client_table_t *table, const client_rows_t *hosts,
    const char *ip, int family, const char *mac, const char *scope)
{
    unsigned char address[sizeof(struct in6_addr)];
    if (!ip || inet_pton(family, ip, address) != 1) { return MT_ERR_PROTO; }
    bool matched = false;
    for (const cJSON *host = hosts->first; mac && host; host = hosts->single ? NULL : host->next) {
        if (!host_active(host)) { continue; }
        char owner[18], label[MT_DNS_CLIENT_NAME_LEN];
        const char *host_if = host_scope(host);
        /* An unknown optional scope does not invalidate explicit IP labels,
         * but a present, unrecognized interface must not widen a MAC join. */
        if ((cJSON_GetObjectItemCaseSensitive(host, "interface") && !host_if) ||
            !mt_dns_client_mac(string_field(host, "mac"), owner) ||
            strcmp(owner, mac) || (scope && host_if && strcmp(scope, host_if))) { continue; }
        matched = true;
        mt_err_t err = mt_dns_client_table_add_owned(table, ip, host_label(host, label),
            mac, scope ? scope : host_if);
        if (err != MT_OK) { return err; }
    }
    /* An unnamed observed owner must still block a conflicting named owner. */
    return matched ? MT_OK : mt_dns_client_table_add_owned(table, ip, NULL, mac, scope);
}
static mt_err_t add_neighbors(mt_dns_client_table_t *table, const client_rows_t *hosts,
                              const client_rows_t *neighbors)
{
    size_t addresses = 0;
    for (const cJSON *row = neighbors->first; row; row = neighbors->single ? NULL : row->next) {
        const cJSON *expired = cJSON_GetObjectItemCaseSensitive(row, "expired");
        const char *expired_text = string_field(row, "expired");
        const cJSON *interface = cJSON_GetObjectItemCaseSensitive(row, "interface");
        const char *scope = string_field(row, "interface");
        /* Preserve the firmware's exact yes/no meaning when supplied as a
         * string; other representations must not silently become false. */
        bool known_expiry = !expired || cJSON_IsBool(expired) ||
            (expired_text && (!strcmp(expired_text, "yes") || !strcmp(expired_text, "no")));
        if (!known_expiry ||
            (interface && (!scope || !*scope || strnlen(scope, MT_DNS_CLIENT_SCOPE_LEN) >= MT_DNS_CLIENT_SCOPE_LEN))) { return MT_ERR_PROTO; }
        if (cJSON_IsTrue(expired) || (expired_text && !strcmp(expired_text, "yes"))) { continue; }
        char owner[18];
        const char *mac = mt_dns_client_mac(string_field(row, "mac"), owner) ? owner : NULL;
        const char *family = string_field(row, "address-family");
        if (family && strcmp(family, "ipv4") == 0) {
            if (++addresses > MT_DNS_CLIENT_LIMIT) { return MT_ERR_LIMIT; }
            mt_err_t err = add_neighbor_address(table, hosts, string_field(row, "address"), AF_INET, mac, scope);
            if (err != MT_OK) { return err; }
        } else if (family && strcmp(family, "ipv6") == 0) {
            const cJSON *list = cJSON_GetObjectItemCaseSensitive(
                cJSON_GetObjectItemCaseSensitive(row, "addresses"), "address");
            if (!cJSON_IsArray(list) && !cJSON_IsObject(list)) { return MT_ERR_PROTO; }
            bool single = cJSON_IsObject(list);
            for (const cJSON *item = single ? list : list->child; item; item = single ? NULL : item->next) {
                if (++addresses > MT_DNS_CLIENT_LIMIT) { return MT_ERR_LIMIT; }
                if (!cJSON_IsObject(item)) { return MT_ERR_PROTO; }
                const char *status = string_field(item, "status");
                if (cJSON_GetObjectItemCaseSensitive(item, "status") && !status) { return MT_ERR_PROTO; }
                /* A live neighbour may still contain expired IPv6 addresses. */
                if (!status || strcmp(status, "active")) { continue; }
                mt_err_t err = add_neighbor_address(table, hosts, string_field(item, "address"), AF_INET6, mac, scope);
                if (err != MT_OK) { return err; }
            }
        } else { return MT_ERR_PROTO; }
    }
    return MT_OK;
}

mt_err_t mt_dns_clients_parse_keenetic(const char *hotspot, size_t hotspot_len,
    const char *neighbors, size_t neighbors_len, mt_dns_client_t **clients, size_t *count)
{
    if (!clients || !count) { return MT_ERR_INVAL; }
    *clients = NULL; *count = 0;
    cJSON *root = NULL;
    client_rows_t hosts = {0};
    mt_dns_client_table_t table = {0};
    mt_err_t err = parse_document(hotspot, hotspot_len, &root);
    if (err == MT_OK) { err = hotspot_rows(root, &hosts); }
    if (err == MT_OK) { err = add_hotspot(&table, &hosts); }
    if (err != MT_OK) { cJSON_Delete(root); free(table.items); return err; }
    size_t explicit_count = table.count;
    if (neighbors && neighbors_len) {
        cJSON *neighbor_root = NULL;
        client_rows_t rows = {0};
        err = parse_document(neighbors, neighbors_len, &neighbor_root);
        if (err == MT_OK) { err = neighbor_rows(neighbor_root, &rows); }
        if (err == MT_OK) { err = add_neighbors(&table, &hosts, &rows); }
        /* Optional enrichment is atomic. Malformed/oversized snapshots cannot
         * hide a conflicting owner after some addresses have been joined. */
        if (err != MT_OK) { table.count = explicit_count; }
        cJSON_Delete(neighbor_root);
    }
    cJSON_Delete(root);
    mt_dns_client_table_finish(&table, clients, count);
    return MT_OK;
}
mt_err_t mt_dns_clients_parse_hotspot(const char *json, size_t len,
                                     mt_dns_client_t **clients, size_t *count)
{
    return mt_dns_clients_parse_keenetic(json, len, NULL, 0, clients, count);
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
typedef struct { const atomic_bool *stop; } client_progress_t;
static int fetch_progress(void *ud, curl_off_t total, curl_off_t now, curl_off_t up_total, curl_off_t up_now)
{
    (void)total; (void)now; (void)up_total; (void)up_now;
    const client_progress_t *ctx = ud;
    return atomic_load(ctx->stop) ? 1 : 0;
}
static mt_err_t fetch_response(const char *url, client_response_t *out, const atomic_bool *stop)
{
    if (stop && atomic_load(stop)) { return MT_ERR_CANCELED; }
    CURL *curl = curl_easy_init();
    if (!curl) { return MT_ERR_NOMEM; }
    client_response_t response = {0};
    const client_progress_t progress = {.stop = stop};
    CURLcode setup = CURLE_OK;
#define SET(option, value) do { if (setup == CURLE_OK) { setup = curl_easy_setopt(curl, option, value); } } while (0)
    SET(CURLOPT_URL, url);
    SET(CURLOPT_PROXY, "");
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_CONNECTTIMEOUT_MS, 500L);
    SET(CURLOPT_TIMEOUT_MS, 2000L);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_WRITEFUNCTION, receive_clients);
    SET(CURLOPT_WRITEDATA, &response);
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
mt_err_t mt_dns_clients_fetch_keenetic(mt_dns_client_t **clients, size_t *count,
                                      const atomic_bool *stop, void *ud)
{
    (void)ud;
    if (!clients || !count) { return MT_ERR_INVAL; }
    *clients = NULL; *count = 0;
    client_response_t hotspot = {0}, neighbors = {0};
    mt_err_t err = fetch_response("http://127.0.0.1:79/rci/show/ip/hotspot", &hotspot, stop);
    if (err != MT_OK) { return err; }
    /* Explicit hotspot addresses remain useful when neighbour discovery fails.
     * Both documents are fresh on every refresh; no cached joins survive it. */
    mt_err_t discovery = fetch_response("http://127.0.0.1:79/rci/show/ip/neighbour", &neighbors, stop);
    if (discovery == MT_ERR_CANCELED || (stop && atomic_load(stop))) { err = MT_ERR_CANCELED; }
    else { err = mt_dns_clients_parse_keenetic(hotspot.data, hotspot.len, neighbors.data, neighbors.len, clients, count); }
    free(hotspot.data); free(neighbors.data);
    return err;
}
