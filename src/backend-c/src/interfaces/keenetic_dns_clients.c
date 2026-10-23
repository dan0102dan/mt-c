/* Compiled only for Entware _kn. Read-only local RCI; never called on the DNS loop. */
#include "dns_clients_internal.h"
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>
#include "magitrickle/keenetic_rci.h"
#include "magitrickle/json.h"

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
    cJSON *root = mt_json_parse_document(json, len);
    if (!root) { return MT_ERR_PROTO; }
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

typedef struct { char *data; size_t len; } client_response_t;
mt_err_t mt_dns_clients_fetch_keenetic(mt_dns_client_t **clients, size_t *count,
                                      const atomic_bool *stop, void *ud)
{
    (void)ud;
    if (!clients || !count) { return MT_ERR_INVAL; }
    *clients = NULL; *count = 0;
    client_response_t hotspot = {0}, neighbors = {0};
    mt_err_t err = mt_kn_rci_request(MT_KN_RCI_BASE_URL "/rci/show/ip/hotspot", NULL,
                                      MT_DNS_CLIENT_BODY_LIMIT, stop, &hotspot.data, &hotspot.len);
    if (err != MT_OK) { return err; }
    /* Explicit hotspot addresses remain useful when neighbour discovery fails.
     * Both documents are fresh on every refresh; no cached joins survive it. */
    mt_err_t discovery = mt_kn_rci_request(MT_KN_RCI_BASE_URL "/rci/show/ip/neighbour", NULL,
                                            MT_DNS_CLIENT_BODY_LIMIT, stop, &neighbors.data, &neighbors.len);
    if (discovery == MT_ERR_CANCELED || (stop && atomic_load(stop))) { err = MT_ERR_CANCELED; }
    else { err = mt_dns_clients_parse_keenetic(hotspot.data, hotspot.len, neighbors.data, neighbors.len, clients, count); }
    free(hotspot.data); free(neighbors.data);
    return err;
}
