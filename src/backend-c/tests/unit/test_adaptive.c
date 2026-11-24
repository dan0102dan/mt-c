/* Exercise the production controller/configuration with deterministic kernel
 * and listener faults. Transport and real firewall behavior have separate tests. */
#include "greatest.h"
#include "magitrickle/adaptive.h"
#include "magitrickle/auth.h"
#include "magitrickle/tproxy.h"
#include "magitrickle/tproxy_routes.h"
#include "magitrickle/yamlio.h"
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static unsigned listeners, starts, stops;
static unsigned fail_listener, fail_start, fail_stop;
static bool kernel_active;
static unsigned saves, clears, fail_save;

static mt_err_t fake_create(mt_loop_t *loop, const mt_tproxy_config_t *config,
                             const mt_tproxy_ops_t *ops, void *ctx, mt_tproxy_t **out)
{
    (void)loop; (void)config; (void)ops; (void)ctx;
    *out = NULL;
    if (fail_listener) { fail_listener--; return MT_ERR_IO; }
    *out = (mt_tproxy_t *)malloc(1);
    if (!*out) { return MT_ERR_NOMEM; }
    listeners++;
    return MT_OK;
}
static void fake_destroy(mt_tproxy_t *proxy) { if (proxy) { listeners--; free(proxy); } }
static uint16_t fake_port(const mt_tproxy_t *proxy) { (void)proxy; return 28080; }
static void fake_stats(const mt_tproxy_t *proxy, mt_tproxy_stats_t *out) { (void)proxy; memset(out, 0, sizeof(*out)); }
static mt_err_t fake_start(mt_app_t *app, const mt_tproxy_route_config_t *config)
{
    (void)app; (void)config; starts++;
    if (fail_start) { fail_start--; return MT_ERR_IO; }
    kernel_active = true; return MT_OK;
}
static mt_err_t fake_stop(mt_app_t *app)
{
    (void)app; stops++;
    if (fail_stop) { fail_stop--; return MT_ERR_IO; }
    kernel_active = false; return MT_OK;
}
static mt_err_t fake_interfaces(mt_app_t *app, mt_iface_info_t **out, size_t *count)
{ (void)app; *out = NULL; *count = 0; return MT_OK; }
static mt_err_t fake_snapshot(mt_app_t *app, mt_tproxy_route_entry_t **out, size_t *count)
{ (void)app; *out = NULL; *count = 0; return MT_OK; }
static mt_err_t fake_clear(mt_app_t *app) { (void)app; clears++; return MT_OK; }
static mt_err_t fake_save(mt_app_t *app, const char *path, const char *version)
{
    (void)app; (void)path; (void)version; saves++;
    if (fail_save) { fail_save--; return MT_ERR_IO; }
    return MT_OK;
}

#define mt_tproxy_create fake_create
#define mt_tproxy_destroy fake_destroy
#define mt_tproxy_port fake_port
#define mt_tproxy_stats fake_stats
#define mt_app_tproxy_start fake_start
#define mt_app_tproxy_stop fake_stop
#define mt_app_list_all_interfaces fake_interfaces
#define mt_app_tproxy_snapshot fake_snapshot
#define mt_app_tproxy_clear fake_clear
#define mt_app_save_config fake_save
#include "../../src/api/adaptive.c"
#undef mt_tproxy_create
#undef mt_tproxy_destroy
#undef mt_tproxy_port
#undef mt_tproxy_stats
#undef mt_app_tproxy_start
#undef mt_app_tproxy_stop
#undef mt_app_list_all_interfaces
#undef mt_app_tproxy_snapshot
#undef mt_app_tproxy_clear
#undef mt_app_save_config

static void reset(void)
{
    listeners = starts = stops = 0;
    fail_listener = fail_start = fail_stop = 0;
    kernel_active = false;
    saves = clears = fail_save = 0;
}

static mt_err_t candidate(mt_adaptive_config_t *out)
{
    mt_err_t err = mt_adaptive_config_defaults(out);
    if (err != MT_OK) { return err; }
    out->enabled = true; out->present = true;
    err = mt_strset(&out->interface, "vpn0");
    if (err != MT_OK) { return err; }
    out->ingress_interfaces = calloc(1, sizeof(*out->ingress_interfaces));
    if (!out->ingress_interfaces) { return MT_ERR_NOMEM; }
    out->ingress_interfaces[0] = strdup("br-lan");
    if (!out->ingress_interfaces[0]) { return MT_ERR_NOMEM; }
    out->n_ingress = 1;
    return MT_OK;
}

TEST config_bounds_and_scope(void)
{
    mt_adaptive_config_t config;
    ASSERT_EQ(MT_OK, mt_adaptive_config_defaults(&config));
    ASSERT(mt_adaptive_config_valid(&config));
    config.enabled = true;
    ASSERT_FALSE(mt_adaptive_config_valid(&config));
    mt_adaptive_config_clear(&config);
    ASSERT_EQ(MT_OK, candidate(&config));
    ASSERT(mt_adaptive_config_valid(&config));
    ASSERT_EQ(MT_OK, mt_strset(&config.interface, "br-lan"));
    ASSERT_FALSE(mt_adaptive_config_valid(&config));
    ASSERT_EQ(MT_OK, mt_strset(&config.interface, "vpn0"));
    config.ttl_seconds = 29; ASSERT_FALSE(mt_adaptive_config_valid(&config));
    config.ttl_seconds = 30; ASSERT(mt_adaptive_config_valid(&config));
    config.max_connections = 1025; ASSERT_FALSE(mt_adaptive_config_valid(&config));
    config.max_connections = 1024; ASSERT(mt_adaptive_config_valid(&config));
    ASSERT_FALSE(mt_adaptive_interface_valid("br+"));
    ASSERT_FALSE(mt_adaptive_interface_valid("lo"));
    ASSERT_FALSE(mt_adaptive_interface_valid("-vpn0"));
    ASSERT_FALSE(mt_adaptive_interface_valid("vpn0 -j ACCEPT"));
    mt_adaptive_config_clear(&config);
    PASS();
}

TEST json_strict_roundtrip(void)
{
    mt_adaptive_config_t original, parsed;
    ASSERT_EQ(MT_OK, candidate(&original));
    cJSON *json = mt_adaptive_config_json(&original);
    ASSERT(json);
    ASSERT_EQ(MT_OK, mt_adaptive_config_parse(json, &parsed));
    ASSERT(mt_adaptive_config_equal(&original, &parsed));
    ASSERT(original.interface != parsed.interface);
    ASSERT(original.ingress_interfaces[0] != parsed.ingress_interfaces[0]);
    mt_adaptive_config_clear(&parsed);
    cJSON_AddBoolToObject(json, "enabled", true);
    ASSERT_EQ(MT_ERR_INVAL, mt_adaptive_config_parse(json, &parsed));
    cJSON_Delete(json);
    json = mt_adaptive_config_json(&original);
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(json, "connectTimeoutMs"), 1500.5);
    ASSERT_EQ(MT_ERR_INVAL, mt_adaptive_config_parse(json, &parsed));
    cJSON_Delete(json);
    json = mt_adaptive_config_json(&original);
    cJSON_AddStringToObject(json, "unrecognized", "field");
    ASSERT_EQ(MT_ERR_INVAL, mt_adaptive_config_parse(json, &parsed));
    cJSON_Delete(json);
    json = mt_adaptive_config_json(&original);
    cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(json, "ingressInterfaces"), cJSON_CreateString("br-lan"));
    ASSERT_EQ(MT_ERR_INVAL, mt_adaptive_config_parse(json, &parsed));
    cJSON_Delete(json);
    mt_adaptive_config_clear(&original);
    PASS();
}

TEST yaml_legacy_omission_and_owned_clone(void)
{
    mt_config_t config, loaded;
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&config));
    char *text = NULL; size_t length = 0;
    ASSERT_EQ(MT_OK, mt_config_save_buffer(&config, "0.99.0", &text, &length));
    ASSERT_FALSE(strstr(text, "adaptiveRouting")); free(text);
    mt_adaptive_config_clear(&config.app.adaptive);
    ASSERT_EQ(MT_OK, candidate(&config.app.adaptive));
    ASSERT_EQ(MT_OK, mt_config_save_buffer(&config, "0.99.0", &text, &length));
    ASSERT(strstr(text, "adaptiveRouting"));
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&loaded));
    ASSERT_EQ(MT_OK, mt_config_load_buffer(&loaded, text, length));
    ASSERT(mt_adaptive_config_equal(&loaded.app.adaptive, &config.app.adaptive));
    free(text);
    mt_app_config_t clone;
    ASSERT_EQ(MT_OK, mt_app_config_clone(&clone, &config.app));
    mt_config_clear(&config);
    ASSERT(mt_adaptive_config_equal(&loaded.app.adaptive, &clone.adaptive));
    mt_app_config_clear(&clone); mt_config_clear(&loaded);
    PASS();
}

TEST yaml_rejects_invalid_without_replacing_adaptive(void)
{
    mt_config_t config;
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&config));
    const char *inputs[] = {
        "configVersion: '0.99.0'\napp: {adaptiveRouting: {enabled: true}}",
        "configVersion: '0.99.0'\napp: {adaptiveRouting: {enabled: false, enabled: true}}",
        "configVersion: '0.99.0'\napp: {adaptiveRouting: {ttlSeconds: 2}}",
        "configVersion: '0.99.0'\napp: {adaptiveRouting: {tlsTimeoutMs: '1000'}}",
        "configVersion: '0.99.0'\napp: {adaptiveRouting: {enabled: false, mystery: 1}}",
        "configVersion: '0.99.0'\napp: {adaptiveRouting: {interface: 'br+'}}",
    };
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        ASSERT_EQ(MT_ERR_INVAL, mt_config_load_buffer(&config, inputs[i], strlen(inputs[i])));
        ASSERT_FALSE(config.app.adaptive.present);
        ASSERT_FALSE(config.app.adaptive.enabled);
    }
    mt_config_clear(&config);
    PASS();
}

TEST controller_enable_retry_and_disable(void)
{
    reset();
    mt_config_t config;
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&config));
    mt_adaptive_service_t *service;
    ASSERT_EQ(MT_OK, mt_adaptive_service_create(NULL, NULL, &config, NULL, NULL, &service));
    ASSERT_EQ(0, starts); ASSERT_EQ(0, listeners);
    mt_adaptive_config_t next;
    ASSERT_EQ(MT_OK, candidate(&next));
    ASSERT_EQ(MT_OK, apply(service, &next));
    ASSERT(service->active); ASSERT(kernel_active); ASSERT_EQ(1, listeners);
    unsigned previous_starts = starts, previous_stops = stops;
    ASSERT_EQ(MT_OK, apply(service, &next));
    ASSERT_EQ(previous_starts, starts); ASSERT_EQ(previous_stops, stops);
    next.enabled = false;
    ASSERT_EQ(MT_OK, apply(service, &next));
    ASSERT_FALSE(service->active); ASSERT_FALSE(kernel_active); ASSERT_EQ(0, listeners);
    ASSERT_EQ(MT_OK, mt_adaptive_service_destroy(service));
    mt_adaptive_config_clear(&next); mt_config_clear(&config);
    PASS();
}

TEST activation_failure_retains_previous_settings(void)
{
    reset();
    mt_config_t config;
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&config));
    mt_adaptive_service_t *service;
    ASSERT_EQ(MT_OK, mt_adaptive_service_create(NULL, NULL, &config, NULL, NULL, &service));
    mt_adaptive_config_t next;
    ASSERT_EQ(MT_OK, candidate(&next));
    fail_listener = 1;
    ASSERT_EQ(MT_ERR_IO, apply(service, &next));
    ASSERT_FALSE(config.app.adaptive.enabled); ASSERT_FALSE(service->active);
    ASSERT_EQ(0, starts); ASSERT_EQ(0, listeners);
    fail_start = 1;
    ASSERT_EQ(MT_ERR_IO, apply(service, &next));
    ASSERT_FALSE(config.app.adaptive.enabled); ASSERT_FALSE(service->active);
    ASSERT_EQ(0, listeners);
    ASSERT_EQ(MT_OK, apply(service, &next));
    ASSERT(service->active); ASSERT_EQ(1, listeners);
    ASSERT_EQ(MT_OK, mt_strset(&next.interface, "vpn1"));
    fail_start = 1;
    ASSERT_EQ(MT_ERR_IO, apply(service, &next));
    ASSERT(service->active); ASSERT(kernel_active); ASSERT_EQ(1, listeners);
    ASSERT_STR_EQ("vpn0", config.app.adaptive.interface);
    ASSERT_EQ(MT_OK, mt_adaptive_service_destroy(service));
    mt_adaptive_config_clear(&next); mt_config_clear(&config);
    PASS();
}

TEST cleanup_failure_keeps_listener_until_retry(void)
{
    reset();
    mt_config_t config;
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&config));
    mt_adaptive_service_t *service;
    ASSERT_EQ(MT_OK, mt_adaptive_service_create(NULL, NULL, &config, NULL, NULL, &service));
    mt_adaptive_config_t next;
    ASSERT_EQ(MT_OK, candidate(&next));
    ASSERT_EQ(MT_OK, apply(service, &next));
    next.enabled = false; fail_stop = 1;
    ASSERT_EQ(MT_ERR_IO, apply(service, &next));
    ASSERT_EQ(1, listeners); ASSERT(config.app.adaptive.enabled);
    ASSERT_FALSE(service->active); ASSERT(service->error[0]);
    ASSERT_EQ(MT_OK, apply(service, &next));
    ASSERT_EQ(0, listeners); ASSERT_FALSE(config.app.adaptive.enabled);
    ASSERT_EQ(MT_OK, mt_adaptive_service_destroy(service));
    mt_adaptive_config_clear(&next); mt_config_clear(&config);
    PASS();
}

TEST unsupported_startup_keeps_settings_available(void)
{
    reset();
    mt_config_t config;
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&config));
    mt_adaptive_config_clear(&config.app.adaptive);
    ASSERT_EQ(MT_OK, candidate(&config.app.adaptive));
    mt_adaptive_service_t *service;
    fail_listener = 1;
    ASSERT_EQ(MT_OK, mt_adaptive_service_create(NULL, NULL, &config, NULL, NULL, &service));
    ASSERT_FALSE(service->active); ASSERT(service->error[0]);
    ASSERT(config.app.adaptive.enabled); ASSERT_EQ(0, listeners);
    ASSERT_EQ(MT_OK, mt_adaptive_service_reload(service));
    ASSERT(service->active); ASSERT_EQ(1, listeners);
    ASSERT_EQ(MT_OK, mt_adaptive_service_destroy(service));
    mt_config_clear(&config);
    PASS();
}

TEST listener_authorization_excludes_direct_and_private_targets(void)
{
    mt_tproxy_endpoint_t client = {.family = AF_INET, .port = 51000};
    mt_tproxy_endpoint_t destination = {.family = AF_INET, .port = 443};
    const char *denied[] = {"127.0.0.1", "192.168.1.1", "10.0.0.1", "172.16.0.1", "169.254.1.1", "224.0.0.1", "100.64.0.1"};
    for (size_t i = 0; i < sizeof(denied) / sizeof(denied[0]); i++) {
        ASSERT_EQ(1, inet_pton(AF_INET, denied[i], destination.addr));
        ASSERT_FALSE(authorize(NULL, &client, &destination));
    }
    ASSERT_EQ(1, inet_pton(AF_INET, "203.0.113.1", destination.addr));
    ASSERT(authorize(NULL, &client, &destination));
    destination.port = 28080; ASSERT_FALSE(authorize(NULL, &client, &destination));
    PASS();
}

/* Run the real parser/router/serializer on the calling thread. Each request
 * has a fresh listener, while the controller and its live flows survive. */
static void stop_http(mt_loop_t *loop, void *ctx) { (void)ctx; mt_loop_stop(loop); }
static bool require_auth(void *ctx) { (void)ctx; return true; }
static int http_request(mt_adaptive_service_t *service, const char *method,
                          const char *path, const char *body, bool auth, cJSON **out)
{
    mt_loop_t *loop = NULL;
    mt_httpd_t *http = NULL;
    int fd = -1, status = -1, timer = 0;
    *out = NULL;
    if (mt_loop_create(&loop) != MT_OK || mt_httpd_create(loop, &http) != MT_OK ||
        mt_adaptive_register_routes(http, service) != MT_OK ||
        mt_httpd_listen_tcp(http, "127.0.0.1", 0) != MT_OK) { goto done; }
    mt_auth_ctx_t auth_ctx = {.enabled = require_auth};
    if (auth) { mt_httpd_set_middleware(http, mt_auth_middleware, &auth_ctx); }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { goto done; }
    struct sockaddr_in address = {.sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = htons(mt_httpd_tcp_port(http))};
    if (connect(fd, (struct sockaddr *)&address, sizeof(address))) { goto done; }
    struct timeval timeout = {2, 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    char request[10000], received[12000];
    int n = snprintf(request, sizeof(request),
        "%s %s HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
        method, path, body ? strlen(body) : 0, body ? body : "");
    if (n < 0 || (size_t)n >= sizeof(request)) { goto done; }
    size_t sent = 0;
    while (sent < (size_t)n) {
        ssize_t chunk = send(fd, request + sent, (size_t)n - sent, 0);
        if (chunk <= 0) { goto done; }
        sent += (size_t)chunk;
    }
    if (mt_loop_add_timer(loop, 25, 0, stop_http, NULL, &timer) != MT_OK ||
        mt_loop_run(loop) != MT_OK) { goto done; }
    size_t used = 0;
    while (used < sizeof(received) - 1) {
        ssize_t chunk = recv(fd, received + used, sizeof(received) - 1 - used, 0);
        if (chunk <= 0) { break; }
        used += (size_t)chunk;
    }
    received[used] = '\0';
    if (sscanf(received, "HTTP/1.1 %d", &status) != 1) { status = -1; goto done; }
    const char *start = strstr(received, "\r\n\r\n");
    if (start) { *out = cJSON_Parse(start + 4); }
    if (!auth && !strstr(received, "Cache-Control: no-store")) { status = -2; }
done:
    if (fd >= 0) { close(fd); }
    mt_httpd_destroy(http); mt_loop_destroy(loop);
    return status;
}

TEST http_persistence_retry_keeps_runtime_and_validates_input(void)
{
    reset();
    mt_config_t config;
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&config));
    mt_adaptive_service_t *service;
    ASSERT_EQ(MT_OK, mt_adaptive_service_create(NULL, NULL, &config, NULL, "/test/config.yaml", &service));
    mt_adaptive_config_t next;
    ASSERT_EQ(MT_OK, candidate(&next));
    cJSON *input = cJSON_CreateObject(), *out = NULL;
    ASSERT(input);
    ASSERT(cJSON_AddItemToObject(input, "settings", mt_adaptive_config_json(&next)));
    char *body = cJSON_PrintUnformatted(input); cJSON_Delete(input); ASSERT(body);
    fail_save = 1;
    ASSERT_EQ(500, http_request(service, "PUT", "/api/v1/adaptive", body, false, &out));
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(out, "applied")));
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(out, "active")));
    ASSERT_STR_EQ("PERSISTENCE_FAILED", cJSON_GetObjectItemCaseSensitive(out, "code")->valuestring);
    cJSON_Delete(out);
    ASSERT_EQ(1, starts); ASSERT_EQ(1, saves); ASSERT_EQ(1, listeners);
    ASSERT_EQ(200, http_request(service, "PUT", "/api/v1/adaptive", body, false, &out));
    cJSON_Delete(out);
    ASSERT_EQ(1, starts); ASSERT_EQ(2, saves); ASSERT_EQ(1, listeners);
    ASSERT_EQ(400, http_request(service, "PUT", "/api/v1/adaptive", "{\"settings\":{}}", false, &out));
    cJSON_Delete(out); ASSERT_EQ(1, starts); ASSERT_EQ(2, saves);
    ASSERT_EQ(400, http_request(service, "PUT", "/api/v1/adaptive", "{} trailing", false, &out));
    cJSON_Delete(out); ASSERT_EQ(1, starts);
    ASSERT_EQ(200, http_request(service, "DELETE", "/api/v1/adaptive/cache", NULL, false, &out));
    cJSON_Delete(out); ASSERT_EQ(1, clears); ASSERT_EQ(1, listeners); ASSERT_EQ(1, starts);
    ASSERT_EQ(200, http_request(service, "GET", "/api/v1/adaptive", NULL, false, &out));
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(out, "active")));
    cJSON_Delete(out);
    free(body); mt_adaptive_config_clear(&next);
    ASSERT_EQ(MT_OK, mt_adaptive_service_destroy(service));
    mt_config_clear(&config);
    PASS();
}

TEST http_auth_blocks_reads_and_writes_before_mutation(void)
{
    reset();
    mt_config_t config;
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&config));
    mt_adaptive_service_t *service;
    ASSERT_EQ(MT_OK, mt_adaptive_service_create(NULL, NULL, &config, NULL, NULL, &service));
    cJSON *out = NULL;
    ASSERT_EQ(401, http_request(service, "GET", "/api/v1/adaptive", NULL, true, &out)); cJSON_Delete(out);
    ASSERT_EQ(401, http_request(service, "PUT", "/api/v1/adaptive", "{}", true, &out)); cJSON_Delete(out);
    ASSERT_EQ(401, http_request(service, "DELETE", "/api/v1/adaptive/cache", NULL, true, &out)); cJSON_Delete(out);
    ASSERT_EQ(0, starts); ASSERT_EQ(0, saves); ASSERT_EQ(0, clears);
    ASSERT_EQ(MT_OK, mt_adaptive_service_destroy(service)); mt_config_clear(&config);
    PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(config_bounds_and_scope);
    RUN_TEST(json_strict_roundtrip);
    RUN_TEST(yaml_legacy_omission_and_owned_clone);
    RUN_TEST(yaml_rejects_invalid_without_replacing_adaptive);
    RUN_TEST(controller_enable_retry_and_disable);
    RUN_TEST(activation_failure_retains_previous_settings);
    RUN_TEST(cleanup_failure_keeps_listener_until_retry);
    RUN_TEST(unsupported_startup_keeps_settings_available);
    RUN_TEST(listener_authorization_excludes_direct_and_private_targets);
    RUN_TEST(http_persistence_retry_keeps_runtime_and_validates_input);
    RUN_TEST(http_auth_blocks_reads_and_writes_before_mutation);
    GREATEST_MAIN_END();
}
