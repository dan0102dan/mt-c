#include "magitrickle/adaptive.h"
#include "magitrickle/json.h"
#include "magitrickle/log.h"
#include "magitrickle/tproxy.h"
#include "magitrickle/tproxy_routes.h"
#include "magitrickle/udp_tproxy.h"
#include "magitrickle/version.h"

#include <arpa/inet.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mt_adaptive_service {
    mt_loop_t *loop;
    mt_app_t *app;
    mt_config_t *config;
    mt_settings_t *settings;
    const char *config_path;
    mt_tproxy_t *proxy;
    mt_udp_tproxy_t *udp_proxy;
    bool active;
    mt_adaptive_config_t runtime;
    char error[256];
};

cJSON *mt_adaptive_config_json(const mt_adaptive_config_t *config)
{
    cJSON *out = cJSON_CreateObject();
    cJSON *ingress = out ? cJSON_AddArrayToObject(out, "ingressInterfaces") : NULL;
    if (!ingress ||
        !cJSON_AddBoolToObject(out, "enabled", config->enabled) ||
        !cJSON_AddBoolToObject(out, "tcpAllPorts", config->tcp_all_ports) ||
        !cJSON_AddBoolToObject(out, "quicEnabled", config->quic_enabled) ||
        !cJSON_AddStringToObject(out, "interface", config->interface ? config->interface : "") ||
        !cJSON_AddNumberToObject(out, "connectTimeoutMs", config->connect_timeout_ms) ||
        !cJSON_AddNumberToObject(out, "tlsTimeoutMs", config->tls_timeout_ms) ||
        !cJSON_AddNumberToObject(out, "idleTimeoutMs", config->idle_timeout_ms) ||
        !cJSON_AddNumberToObject(out, "ttlSeconds", config->ttl_seconds) ||
        !cJSON_AddNumberToObject(out, "maxConnections", config->max_connections) ||
        !cJSON_AddNumberToObject(out, "maxEntries", config->max_entries)) { cJSON_Delete(out); return NULL; }
    for (size_t i = 0; i < config->n_ingress; i++) {
        cJSON *item = cJSON_CreateString(config->ingress_interfaces[i]);
        if (!item || !cJSON_AddItemToArray(ingress, item)) { cJSON_Delete(item); cJSON_Delete(out); return NULL; }
    }
    return out;
}

mt_err_t mt_adaptive_config_parse(const cJSON *json, mt_adaptive_config_t *out)
{
    memset(out, 0, sizeof(*out));
    static const char *const keys[] = {"enabled", "interface", "ingressInterfaces",
        "connectTimeoutMs", "tlsTimeoutMs", "idleTimeoutMs", "ttlSeconds", "maxConnections", "maxEntries",
        "tcpAllPorts", "quicEnabled"};
    if (!json || !cJSON_IsObject(json)) { return MT_ERR_INVAL; }
    const cJSON *values[sizeof(keys) / sizeof(keys[0])] = {0};
    const cJSON *item;
    cJSON_ArrayForEach(item, json) {
        size_t i = 0;
        while (i < sizeof(keys) / sizeof(keys[0]) && (!item->string || strcmp(item->string, keys[i]) != 0)) { i++; }
        if (i == sizeof(keys) / sizeof(keys[0]) || values[i]) { return MT_ERR_INVAL; }
        values[i] = item;
    }
    if (!values[0] || !values[1] || !values[2] ||
        !cJSON_IsBool(values[0]) || !cJSON_IsString(values[1]) || !cJSON_IsArray(values[2]) ||
        cJSON_GetArraySize(values[2]) > 16) { return MT_ERR_INVAL; }
    mt_err_t err = mt_adaptive_config_defaults(out);
    if (err != MT_OK) { return err; }
    out->present = true;
    out->enabled = cJSON_IsTrue(values[0]);
    /* Old clients/configurations keep the original TCP web-only scope. */
    if ((values[9] && !cJSON_IsBool(values[9])) ||
        (values[10] && !cJSON_IsBool(values[10]))) { err = MT_ERR_INVAL; goto fail; }
    out->tcp_all_ports = cJSON_IsTrue(values[9]);
    out->quic_enabled = cJSON_IsTrue(values[10]);
    err = mt_strset(&out->interface, values[1]->valuestring);
    if (err != MT_OK) { goto fail; }
    size_t count = (size_t)cJSON_GetArraySize(values[2]);
    if (count) {
        out->ingress_interfaces = calloc(count, sizeof(*out->ingress_interfaces));
        if (!out->ingress_interfaces) { err = MT_ERR_NOMEM; goto fail; }
    }
    cJSON_ArrayForEach(item, values[2]) {
        if (!cJSON_IsString(item)) { err = MT_ERR_INVAL; goto fail; }
        err = mt_strset(&out->ingress_interfaces[out->n_ingress], item->valuestring);
        if (err != MT_OK) { goto fail; }
        out->n_ingress++;
    }
    uint32_t *numbers[] = {&out->connect_timeout_ms, &out->tls_timeout_ms, &out->idle_timeout_ms,
        &out->ttl_seconds, &out->max_connections, &out->max_entries};
    for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++) {
        const cJSON *value = values[i + 3];
        if (!value || !cJSON_IsNumber(value) || !isfinite(value->valuedouble) || value->valuedouble < 0 ||
            value->valuedouble > UINT32_MAX || (double)(uint32_t)value->valuedouble != value->valuedouble) {
            err = MT_ERR_INVAL; goto fail;
        }
        *numbers[i] = (uint32_t)value->valuedouble;
    }
    if (!mt_adaptive_config_valid(out)) { err = MT_ERR_INVAL; goto fail; }
    return MT_OK;
fail:
    mt_adaptive_config_clear(out);
    return err;
}

/* Listeners bind loopback explicitly, and TPROXY selects them via --on-ip.
 * Direct listener connections retain a loopback destination and fail this
 * guard even when every TCP destination port is enabled. */
static bool public_destination(const mt_tproxy_endpoint_t *client,
                                const mt_tproxy_endpoint_t *destination)
{
    if (!client || !destination || client->family != destination->family ||
        !destination->port) { return false; }
    const uint8_t *a = destination->addr;
    if (destination->family == AF_INET) {
        return a[0] != 0 && a[0] != 10 && a[0] != 127 && a[0] < 224 &&
            !(a[0] == 169 && a[1] == 254) && !(a[0] == 172 && a[1] >= 16 && a[1] <= 31) &&
            !(a[0] == 192 && a[1] == 168) && !(a[0] == 100 && a[1] >= 64 && a[1] <= 127);
    }
    if (destination->family == AF_INET6) {
        /* Global unicast only; excludes mapped IPv4, unspecified, link-local,
         * ULA and multicast. Matches the kernel scope for the first version. */
        return (a[0] & 0xe0u) == 0x20u;
    }
    return false;
}

static void combined_stats(const mt_adaptive_service_t *service, mt_tproxy_stats_t *out)
{
    mt_tproxy_stats_t udp;
    mt_tproxy_stats(service->proxy, out);
    mt_udp_tproxy_stats(service->udp_proxy, &udp);
    out->active += udp.active;
    out->accepted += udp.accepted;
    out->direct_success += udp.direct_success;
    out->alternate_success += udp.alternate_success;
    out->failures += udp.failures;
    out->over_limit += udp.over_limit;
}

static bool capacity_available(const mt_adaptive_service_t *service)
{
    if (!service) { return false; }
    mt_tproxy_stats_t stats;
    combined_stats(service, &stats);
    return stats.active < service->runtime.max_connections;
}

static bool authorize(void *ctx, const mt_tproxy_endpoint_t *client,
                       const mt_tproxy_endpoint_t *destination)
{
    const mt_adaptive_service_t *service = ctx;
    return capacity_available(service) && public_destination(client, destination) &&
        (service->runtime.tcp_all_ports || destination->port == 80 || destination->port == 443);
}

static bool authorize_udp(void *ctx, const mt_tproxy_endpoint_t *client,
                           const mt_tproxy_endpoint_t *destination)
{
    const mt_adaptive_service_t *service = ctx;
    return capacity_available(service) && public_destination(client, destination) &&
        service->runtime.quic_enabled && destination->port == 443;
}

static mt_err_t prepare(void *ctx, int fd, const mt_tproxy_endpoint_t *client,
                         const mt_tproxy_endpoint_t *destination, bool alternate)
{
    (void)client; (void)destination;
    mt_adaptive_service_t *service = ctx;
    uint32_t mark = mt_app_tproxy_mark(service->app, alternate);
    if (mark == UINT32_MAX) { return MT_ERR_STATE; }
    return mt_tproxy_set_mark(fd, mark);
}

static void learned(void *ctx, const mt_tproxy_endpoint_t *client,
                     const mt_tproxy_endpoint_t *destination, bool alternate)
{
    (void)client;
    mt_adaptive_service_t *service = ctx;
    mt_err_t err = mt_app_tproxy_learn(service->app, destination->family, IPPROTO_TCP, destination->addr,
                                       destination->port, alternate);
    if (err != MT_OK && err != MT_ERR_AGAIN && err != MT_ERR_LIMIT) {
        MT_DEBUG("adaptive route not cached: %s", mt_err_str(err));
    }
}

static void learned_udp(void *ctx, const mt_tproxy_endpoint_t *client,
                         const mt_tproxy_endpoint_t *destination, bool alternate)
{
    (void)client;
    mt_adaptive_service_t *service = ctx;
    mt_err_t err = mt_app_tproxy_learn(service->app, destination->family, IPPROTO_UDP,
                                       destination->addr, destination->port, alternate);
    if (err != MT_OK && err != MT_ERR_AGAIN && err != MT_ERR_LIMIT) {
        MT_DEBUG("adaptive UDP route not cached: %s", mt_err_str(err));
    }
}

static mt_err_t pin_udp(void *ctx, const mt_tproxy_endpoint_t *client,
                         const mt_tproxy_endpoint_t *destination, uint32_t *token)
{
    mt_adaptive_service_t *service = ctx;
    mt_tproxy_udp_tuple_t tuple = {.family = destination->family,
        .client_port = client->port, .destination_port = destination->port};
    memcpy(tuple.client_address, client->addr, sizeof(tuple.client_address));
    memcpy(tuple.destination_address, destination->addr, sizeof(tuple.destination_address));
    return mt_app_tproxy_pin_udp(service->app, &tuple, token);
}

static void unpin_udp(void *ctx, uint32_t token)
{
    mt_adaptive_service_t *service = ctx;
    mt_app_tproxy_unpin_udp(service->app, token);
}

static mt_err_t stop_runtime(mt_adaptive_service_t *service)
{
    /* Stop interception before closing accepted sockets/listeners. */
    mt_err_t err = mt_app_tproxy_stop(service->app);
    if (err != MT_OK) { service->active = false; return err; }
    mt_tproxy_destroy(service->proxy);
    mt_udp_tproxy_destroy(service->udp_proxy);
    service->proxy = NULL;
    service->udp_proxy = NULL;
    service->active = false;
    return err;
}

static mt_err_t start_runtime(mt_adaptive_service_t *service, const mt_adaptive_config_t *config)
{
    if (service->proxy || service->udp_proxy) { return MT_ERR_STATE; }
    if (!config->enabled) { return MT_OK; }
    if (!mt_adaptive_config_valid(config) ||
        (service->config->app.netfilter.disable_ipv4 && service->config->app.netfilter.disable_ipv6)) { return MT_ERR_INVAL; }
    mt_tproxy_config_t transport = {
        .port = 0,
        .ipv4 = !service->config->app.netfilter.disable_ipv4,
        .ipv6 = !service->config->app.netfilter.disable_ipv6,
        .connect_timeout_ms = config->connect_timeout_ms,
        .tls_timeout_ms = config->tls_timeout_ms,
        .idle_timeout_ms = config->idle_timeout_ms,
        .max_connections = config->max_connections,
    };
    const mt_tproxy_ops_t ops = {.authorize = authorize, .prepare = prepare, .learned = learned};
    mt_err_t err = mt_tproxy_create(service->loop, &transport, &ops, service, &service->proxy);
    if (err != MT_OK) { return err; }
    if (config->quic_enabled) {
        const mt_udp_tproxy_config_t udp = {.ipv4 = transport.ipv4, .ipv6 = transport.ipv6,
            .response_timeout_ms = config->connect_timeout_ms,
            .idle_timeout_ms = config->idle_timeout_ms, .max_connections = config->max_connections};
        const mt_udp_tproxy_ops_t udp_ops = {.authorize = authorize_udp, .prepare = prepare,
            .learned = learned_udp, .pin = pin_udp, .unpin = unpin_udp};
        err = mt_udp_tproxy_create(service->loop, &udp, &udp_ops, service, &service->udp_proxy);
        if (err != MT_OK) {
            mt_tproxy_destroy(service->proxy); service->proxy = NULL;
            return err;
        }
    }
    mt_tproxy_route_config_t routes = {
        .interface = config->interface,
        .ingress_interfaces = (const char *const *)config->ingress_interfaces,
        .n_ingress = config->n_ingress,
        .port = mt_tproxy_port(service->proxy),
        .udp_port = mt_udp_tproxy_port(service->udp_proxy),
        .tcp_all_ports = config->tcp_all_ports,
        .max_connections = config->max_connections,
        .ttl_seconds = config->ttl_seconds,
        .max_entries = config->max_entries,
    };
    err = mt_app_tproxy_start(service->app, &routes);
    if (err != MT_OK) {
        /* Failed cleanup may leave a live interception jump. Retain the
         * listener until stop succeeds; never report that runtime as active. */
        if (mt_app_tproxy_stop(service->app) == MT_OK) {
            mt_tproxy_destroy(service->proxy);
            mt_udp_tproxy_destroy(service->udp_proxy);
            service->proxy = NULL;
            service->udp_proxy = NULL;
        }
    } else {
        service->active = true;
    }
    return err;
}

/* Candidate model and rollback baseline are allocated before kernel mutation.
 * A persistence retry of an already applied configuration keeps live flows. */
static mt_err_t apply(mt_adaptive_service_t *service, const mt_adaptive_config_t *candidate)
{
    if (!mt_adaptive_config_valid(candidate)) { return MT_ERR_INVAL; }
    mt_adaptive_config_t stored = {0}, runtime = {0};
    mt_err_t err = mt_adaptive_config_clone(&stored, candidate);
    if (err == MT_OK) { err = mt_adaptive_config_clone(&runtime, candidate); }
    if (err != MT_OK) { mt_adaptive_config_clear(&stored); return err; }
    bool change = !mt_adaptive_config_equal(&service->runtime, candidate) ||
                    (candidate->enabled != service->active) ||
                    (!service->active && (service->proxy || service->udp_proxy));
    if (change) {
        err = stop_runtime(service);
        if (err != MT_OK) {
            snprintf(service->error, sizeof(service->error), "Cannot remove previous adaptive routing (%s); retry required", mt_err_str(err));
            mt_adaptive_config_clear(&stored); mt_adaptive_config_clear(&runtime);
            return err;
        }
        err = start_runtime(service, candidate);
        if (err != MT_OK) {
            mt_err_t rollback = start_runtime(service, &service->runtime);
            snprintf(service->error, sizeof(service->error),
                     "Cannot activate adaptive routing (%s)%s", mt_err_str(err),
                     rollback == MT_OK ? "; previous settings restored" : "; restoring previous routing also failed");
            mt_adaptive_config_clear(&stored); mt_adaptive_config_clear(&runtime);
            return err;
        }
    }
    service->error[0] = '\0';
    mt_adaptive_config_clear(&service->config->app.adaptive);
    service->config->app.adaptive = stored;
    mt_adaptive_config_clear(&service->runtime);
    service->runtime = runtime;
    return MT_OK;
}

mt_err_t mt_adaptive_service_create(mt_loop_t *loop, mt_app_t *app, mt_config_t *config,
                                    mt_settings_t *settings, const char *config_path,
                                    mt_adaptive_service_t **out)
{
    *out = NULL;
    mt_adaptive_service_t *service = calloc(1, sizeof(*service));
    if (!service) { return MT_ERR_NOMEM; }
    service->loop = loop; service->app = app; service->config = config;
    service->settings = settings; service->config_path = config_path;
    mt_err_t err = mt_adaptive_config_clone(&service->runtime, &config->app.adaptive);
    if (err != MT_OK) { free(service); return err; }
    err = start_runtime(service, &config->app.adaptive);
    if (err != MT_OK) {
        snprintf(service->error, sizeof(service->error), "Adaptive routing unavailable (%s); check TPROXY support and selected interfaces", mt_err_str(err));
        MT_WARN("%s", service->error);
    }
    *out = service;
    return MT_OK;
}

mt_err_t mt_adaptive_service_destroy(mt_adaptive_service_t *service)
{
    if (!service) { return MT_OK; }
    mt_err_t err = stop_runtime(service);
    /* The daemon is terminating; callbacks cannot outlive the controller. */
    mt_tproxy_destroy(service->proxy);
    mt_udp_tproxy_destroy(service->udp_proxy);
    mt_adaptive_config_clear(&service->runtime);
    free(service);
    return err;
}

mt_err_t mt_adaptive_service_reload(mt_adaptive_service_t *service)
{
    if (!service) { return MT_OK; }
    mt_err_t err = apply(service, &service->config->app.adaptive);
    if (err != MT_OK) {
        /* File reload already published the model. Restore this section's
         * active model too, so a later unrelated save does not persist failure. */
        mt_adaptive_config_t restored;
        if (mt_adaptive_config_clone(&restored, &service->runtime) == MT_OK) {
            mt_adaptive_config_clear(&service->config->app.adaptive);
            service->config->app.adaptive = restored;
        }
    }
    return err;
}

static cJSON *snapshot(mt_adaptive_service_t *service)
{
    cJSON *out = cJSON_CreateObject();
    cJSON *settings = mt_adaptive_config_json(&service->config->app.adaptive);
    if (!out || !settings || !cJSON_AddItemToObject(out, "settings", settings)) {
        cJSON_Delete(settings); cJSON_Delete(out); return NULL;
    }
    cJSON *interfaces = cJSON_AddArrayToObject(out, "interfaces");
    cJSON *entries = cJSON_AddArrayToObject(out, "entries");
    cJSON *stats = cJSON_AddObjectToObject(out, "stats");
    if (!interfaces || !entries || !stats || !cJSON_AddBoolToObject(out, "active", service->active) ||
        !(service->error[0] ? cJSON_AddStringToObject(out, "error", service->error) : cJSON_AddNullToObject(out, "error"))) { goto fail; }
    mt_tproxy_stats_t counters;
    combined_stats(service, &counters);
    if (!cJSON_AddNumberToObject(stats, "active", counters.active) ||
        !cJSON_AddNumberToObject(stats, "accepted", (double)counters.accepted) ||
        !cJSON_AddNumberToObject(stats, "directSuccess", (double)counters.direct_success) ||
        !cJSON_AddNumberToObject(stats, "alternateSuccess", (double)counters.alternate_success) ||
        !cJSON_AddNumberToObject(stats, "failures", (double)counters.failures) ||
        !cJSON_AddNumberToObject(stats, "overLimit", (double)counters.over_limit)) { goto fail; }
    mt_iface_info_t *ifaces = NULL; size_t count = 0;
    if (mt_app_list_all_interfaces(service->app, &ifaces, &count) != MT_OK) { goto fail; }
    for (size_t i = 0; i < count; i++) {
        if (!mt_adaptive_interface_valid(ifaces[i].id)) { continue; }
        cJSON *item = cJSON_CreateObject();
        const char *label = "";
#ifdef MT_ENTWARE_KN
        label = ifaces[i].name;
#endif
        if (!item || !cJSON_AddStringToObject(item, "id", ifaces[i].id) ||
            !cJSON_AddStringToObject(item, "name", label) || !cJSON_AddItemToArray(interfaces, item)) {
            cJSON_Delete(item); free(ifaces); goto fail;
        }
    }
    free(ifaces);
    mt_tproxy_route_entry_t *routes = NULL; count = 0;
    if (mt_app_tproxy_snapshot(service->app, &routes, &count) != MT_OK) { goto fail; }
    for (size_t i = 0; i < count; i++) {
        char address[INET6_ADDRSTRLEN];
        if (!inet_ntop(routes[i].family, routes[i].address, address, sizeof(address))) { continue; }
        cJSON *item = cJSON_CreateObject();
        if (!item || !cJSON_AddStringToObject(item, "address", address) ||
            !cJSON_AddNumberToObject(item, "family", routes[i].family == AF_INET ? 4 : 6) ||
            !cJSON_AddNumberToObject(item, "port", routes[i].port) ||
            !cJSON_AddStringToObject(item, "protocol", routes[i].protocol == IPPROTO_UDP ? "udp" : "tcp") ||
            !cJSON_AddBoolToObject(item, "alternate", routes[i].alternate) ||
            !cJSON_AddNumberToObject(item, "expiresInSeconds", routes[i].expires_in_seconds) ||
            !cJSON_AddItemToArray(entries, item)) { cJSON_Delete(item); free(routes); goto fail; }
    }
    free(routes);
    return out;
fail:
    cJSON_Delete(out);
    return NULL;
}

static void response(mt_adaptive_service_t *service, mt_http_res_t *res, int status,
                       const char *code, const char *error, bool applied)
{
    mt_http_res_set_header(res, "Cache-Control", "no-store");
    cJSON *out = snapshot(service);
    if (!out) { mt_http_res_write_error(res, 503, "Cannot read adaptive routing state"); return; }
    if (code) { cJSON_AddStringToObject(out, "code", code); }
    if (error) { cJSON_DeleteItemFromObjectCaseSensitive(out, "error"); cJSON_AddStringToObject(out, "error", error); }
    if (applied) { cJSON_AddBoolToObject(out, "applied", true); }
    mt_http_res_write_json(res, status, out);
}

static void get_settings(mt_http_req_t *req, mt_http_res_t *res, void *ctx)
{
    (void)req;
    response(ctx, res, 200, NULL, NULL, false);
}

static void put_settings(mt_http_req_t *req, mt_http_res_t *res, void *ctx)
{
    mt_adaptive_service_t *service = ctx;
    if (mt_settings_busy(service->settings)) {
        response(service, res, 409, "SETTINGS_BUSY", "General settings are being applied", false); return;
    }
    const char *site = mt_http_req_header(req, "Sec-Fetch-Site");
    const char *type = mt_http_req_header(req, "Content-Type");
    if ((site && strcmp(site, "cross-site") == 0) || !type || strncmp(type, "application/json", 16) != 0 ||
        (type[16] && type[16] != ';')) {
        response(service, res, 400, "INVALID_SETTINGS", "Expected a same-origin JSON request", false); return;
    }
    size_t len = 0;
    const uint8_t *body = mt_http_req_body(req, &len);
    cJSON *json = len <= 8192 ? mt_json_parse_document((const char *)body, len) : NULL;
    mt_adaptive_config_t candidate = {0};
    mt_err_t err = MT_ERR_INVAL;
    if (json && cJSON_IsObject(json) && cJSON_GetArraySize(json) == 1) {
        err = mt_adaptive_config_parse(cJSON_GetObjectItemCaseSensitive(json, "settings"), &candidate);
    }
    cJSON_Delete(json);
    if (err != MT_OK) { response(service, res, 400, "INVALID_SETTINGS", "Invalid adaptive routing settings", false); return; }
    err = apply(service, &candidate);
    mt_adaptive_config_clear(&candidate);
    if (err != MT_OK) {
        response(service, res, 503, "TPROXY_UNAVAILABLE", service->error[0] ? service->error : mt_err_str(err), false); return;
    }
    if (service->config_path) {
        err = mt_app_save_config(service->app, service->config_path, MT_VERSION);
        if (err != MT_OK) {
            response(service, res, 500, "PERSISTENCE_FAILED", "Routing applied, but configuration could not be saved", true); return;
        }
    }
    response(service, res, 200, NULL, NULL, false);
}

static void clear_cache(mt_http_req_t *req, mt_http_res_t *res, void *ctx)
{
    (void)req;
    mt_adaptive_service_t *service = ctx;
    mt_err_t err = mt_app_tproxy_clear(service->app);
    response(service, res, err == MT_OK ? 200 : 503,
               err == MT_OK ? NULL : "CACHE_UNAVAILABLE", err == MT_OK ? NULL : mt_err_str(err), false);
}

mt_err_t mt_adaptive_register_routes(mt_httpd_t *http, mt_adaptive_service_t *service)
{
    mt_err_t err = mt_httpd_route(http, "GET", "/api/v1/adaptive", get_settings, service);
    if (err == MT_OK) { err = mt_httpd_route(http, "PUT", "/api/v1/adaptive", put_settings, service); }
    if (err == MT_OK) { err = mt_httpd_route(http, "DELETE", "/api/v1/adaptive/cache", clear_cache, service); }
    return err;
}
