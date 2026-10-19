/* See system.h. Port of handlers.go's ListInterfaces/SaveConfig/
 * NetfilterDHook. */
#include "magitrickle/system.h"
#include "magitrickle/profiles_api.h"
#include "magitrickle/update.h"
#include "magitrickle/http_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "magitrickle/log.h"

static void must_route(mt_httpd_t *h, const char *method, const char *pattern, mt_http_handler_fn fn,
                       void *ud) {
    if (mt_httpd_route(h, method, pattern, fn, ud) != MT_OK) {
        MT_ERROR("failed to register route %s %s", method, pattern);
    }
}

static void handle_list_interfaces(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req;
    mt_system_ctx_t *ctx = ud;

    mt_iface_info_t *ifaces = NULL;
    size_t n = 0;
    mt_err_t err = mt_app_list_interfaces(ctx->app, &ifaces, &n);
    if (err != MT_OK) {
        mt_http_res_write_error(res, 500, mt_err_str(err));
        return;
    }

    cJSON *out = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(out, "interfaces");
    cJSON *blackhole = cJSON_CreateObject();
    cJSON_AddStringToObject(blackhole, "id", "blackhole");
    cJSON_AddItemToArray(arr, blackhole);
    for (size_t i = 0; i < n; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", ifaces[i].id);
#ifdef MT_ENTWARE_KN
        /* Keep the kernel ID; the optional RCI alias only enriches KN responses. */
        if (ifaces[i].name[0] != '\0') { cJSON_AddStringToObject(item, "name", ifaces[i].name); }
#endif
        cJSON_AddItemToArray(arr, item);
    }
    free(ifaces);
    mt_http_res_write_json(res, 200, out);
}

static void handle_save_config(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req;
    mt_system_ctx_t *ctx = ud;
    if (!ctx->config_path) {
        mt_http_res_write(res, 200, NULL, NULL, 0);
        return;
    }
    mt_err_t err =
        mt_app_save_config(ctx->app, ctx->config_path, ctx->config_version ? ctx->config_version : "");
    if (err != MT_OK) {
        mt_http_res_write_error(res, 500, mt_err_str(err));
        return;
    }
    mt_http_res_write(res, 200, NULL, NULL, 0);
}

static void handle_netfilterd_hook(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_system_ctx_t *ctx = ud;
    size_t body_len;
    const uint8_t *body = mt_http_req_body(req, &body_len);
    cJSON *json = body_len > 0 ? cJSON_ParseWithLength((const char *)body, body_len) : NULL;
    if (!json) {
        mt_http_res_write_error(res, 400, "failed to parse request");
        return;
    }
    cJSON *type_j = cJSON_GetObjectItemCaseSensitive(json, "type");
    cJSON *table_j = cJSON_GetObjectItemCaseSensitive(json, "table");
    MT_DEBUG("received netfilter.d event: type=%s table=%s",
            cJSON_IsString(type_j) ? type_j->valuestring : "",
            cJSON_IsString(table_j) ? table_j->valuestring : "");
    cJSON_Delete(json);

    /* Never turned into an HTTP error response: the firmware fires this
     * hook while it is still rewriting tables and has nothing to do with
     * a failure. Where a committer runs (Keenetic `_kn`), the call only
     * queues a rebuild and cannot fail at all -- see
     * mt_app_force_commit_iptables. */
    mt_err_t err = mt_app_force_commit_iptables(ctx->app);
    if (err != MT_OK) { MT_ERROR("error fixing iptables after netfilter.d: %s", mt_err_str(err)); }
    mt_http_res_write(res, 200, NULL, NULL, 0);
}

static cJSON *dns_query_json(const mt_dns_query_entry_t *entry, const mt_dns_clients_t *clients) {
    char address[MT_DNS_CLIENT_IP_LEN];
    const char *ip = mt_dns_client_address(entry->client, address, sizeof(address)) ? address : entry->client;
    const char *name = mt_dns_clients_lookup(clients, ip);
    cJSON *row = cJSON_CreateObject();
    if (!row) { return NULL; }
    if (name && !cJSON_AddStringToObject(row, "client_name", name)) {
        cJSON_Delete(row); return NULL;
    }
    if (!cJSON_AddNumberToObject(row, "id", (double)entry->id) ||
        !cJSON_AddNumberToObject(row, "time_ms", (double)entry->time_ms) ||
        !cJSON_AddStringToObject(row, "client", ip) ||
        !cJSON_AddStringToObject(row, "domain", entry->domain) ||
        !cJSON_AddNumberToObject(row, "qtype", entry->qtype) ||
        !cJSON_AddStringToObject(row, "protocol", entry->tcp ? "tcp" : "udp")) {
        cJSON_Delete(row);
        return NULL;
    }
    return row;
}

static cJSON *dns_snapshot(mt_dns_query_log_t *log, const mt_dns_clients_t *clients) {
    cJSON *result = cJSON_CreateObject();
    if (!result) { return NULL; }
    cJSON *rows = cJSON_AddArrayToObject(result, "queries");
    if (!rows || !cJSON_AddNumberToObject(result, "capacity", (double)mt_dns_query_log_capacity(log))) {
        cJSON_Delete(result);
        return NULL;
    }
    size_t count = mt_dns_query_log_size(log);
    for (size_t i = 0; i < count; i++) {
        mt_dns_query_entry_t entry;
        if (!mt_dns_query_log_get(log, i, &entry)) { break; }
        cJSON *row = dns_query_json(&entry, clients);
        if (!row || !cJSON_AddItemToArray(rows, row)) {
            cJSON_Delete(row);
            cJSON_Delete(result);
            return NULL;
        }
    }
    return result;
}

/* Both snapshot and stream run on the same event loop as append/clear.
 * There is no snapshot/subscribe race or per-subscriber event queue. */
#include "dns_stream.inc"

static void handle_dns_queries(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req;
    mt_system_ctx_t *ctx = ud;
    cJSON *result = dns_snapshot(ctx->query_log, ctx->dns_clients);
    if (!result) { mt_http_res_write_error(res, 500, "out of memory"); return; }
    mt_http_res_set_header(res, "Cache-Control", "no-store");
    mt_http_res_write_json(res, 200, result);
}

static void handle_dns_clear(mt_http_req_t *req, mt_http_res_t *res, void *ud)
{
    (void)req;
    mt_system_ctx_t *ctx = ud;
    mt_dns_query_log_clear(ctx->query_log);
    cJSON *result = cJSON_CreateObject();
    if (!result) { mt_http_res_write_error(res, 500, "out of memory"); return; }
    cJSON_AddBoolToObject(result, "cleared", true);
    mt_http_res_set_header(res, "Cache-Control", "no-store");
    mt_http_res_write_json(res, 200, result);
}

void mt_system_register_routes(mt_httpd_t *h, mt_system_ctx_t *ctx) {
    mt_profiles_register_routes(h, ctx);
    mt_update_register_routes(h, ctx);
    must_route(h, "GET", "/api/v1/system/interfaces", handle_list_interfaces, ctx);
    must_route(h, "GET", "/api/v1/system/dns/queries", handle_dns_queries, ctx);
    must_route(h, "GET", "/api/v1/system/dns/stream", handle_dns_stream, ctx);
    must_route(h, "DELETE", "/api/v1/system/dns/queries", handle_dns_clear, ctx);
    must_route(h, "POST", "/api/v1/system/config/save", handle_save_config, ctx);
    must_route(h, "POST", "/api/v1/system/hooks/netfilterd", handle_netfilterd_hook, ctx);
}
