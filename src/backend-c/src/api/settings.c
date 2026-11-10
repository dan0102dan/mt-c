#include "magitrickle/settings.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <net/if.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "magitrickle/hash.h"
#include "magitrickle/json.h"
#include "magitrickle/rand.h"
#include "magitrickle/update.h"

struct mt_settings {
    mt_config_t *config;
    mt_loop_t *loop;
    char id[33], revision[65], target_revision[65], active_revision[65], error[256];
    const char *state;
    mt_app_config_t requested, candidate;
    mt_dns_upstream_t upstream;
    mt_err_t resolve_error;
    pthread_t resolver;
    int resolve_pipe[2];
    bool resolving, response_pending, acknowledged, resolved, ready;
    bool dns_restart_required, allow_dns_restart;
    mt_settings_live_apply_fn live_apply;
    void *live_userdata;
    uint16_t http_port, dns_port;
};

static bool log_level(const char *value);

/* All serializer allocations are checked: a partial projection must never
 * become an accepted baseline or a successful acknowledgement. */
cJSON *mt_settings_json(const mt_app_config_t *a)
{
    uint32_t timeout = (uint32_t)(a->dns_proxy.timeout / MT_DURATION_MS);
    uint32_t concurrent = (uint32_t)a->dns_proxy.max_concurrent;
    if (!timeout) { timeout = 5000; }
    if (!concurrent) { concurrent = 100; }
    cJSON *root = cJSON_CreateObject();
    cJSON *http = root ? cJSON_AddObjectToObject(root, "httpWeb") : NULL;
    cJSON *auth = http ? cJSON_AddObjectToObject(http, "auth") : NULL;
    cJSON *web_host = http ? cJSON_AddObjectToObject(http, "host") : NULL;
    cJSON *dns = root ? cJSON_AddObjectToObject(root, "dnsProxy") : NULL;
    cJSON *dns_host = dns ? cJSON_AddObjectToObject(dns, "host") : NULL;
    cJSON *upstream = dns ? cJSON_AddObjectToObject(dns, "upstream") : NULL;
    cJSON *nf = root ? cJSON_AddObjectToObject(root, "netfilter") : NULL;
    cJSON *ipset = nf ? cJSON_AddObjectToObject(nf, "ipset") : NULL;
    cJSON *links = root ? cJSON_AddArrayToObject(root, "link") : NULL;
    if (!root || !http || !auth || !web_host || !dns || !dns_host || !upstream ||
        !nf || !ipset || !links ||
        !cJSON_AddBoolToObject(http, "enabled", a->http_web.enabled) ||
        !cJSON_AddBoolToObject(auth, "enabled", a->http_web.auth.enabled) ||
        !cJSON_AddStringToObject(web_host, "address", a->http_web.host.address) ||
        !cJSON_AddNumberToObject(web_host, "port", a->http_web.host.port) ||
        !cJSON_AddStringToObject(dns_host, "address", a->dns_proxy.host.address) ||
        !cJSON_AddNumberToObject(dns_host, "port", a->dns_proxy.host.port) ||
        !cJSON_AddStringToObject(upstream, "address", a->dns_proxy.upstream.address) ||
        !cJSON_AddNumberToObject(upstream, "port", a->dns_proxy.upstream.port) ||
        !cJSON_AddBoolToObject(dns, "disableRemap53", a->dns_proxy.disable_remap53) ||
        !cJSON_AddBoolToObject(dns, "disableFakePTR", a->dns_proxy.disable_fake_ptr) ||
        !cJSON_AddBoolToObject(dns, "disableDropAAAA", a->dns_proxy.disable_drop_aaaa) ||
        !cJSON_AddNumberToObject(dns, "maxIdleConns", (uint32_t)a->dns_proxy.max_idle_conns) ||
        !cJSON_AddNumberToObject(dns, "maxConcurrent", concurrent) ||
        !cJSON_AddNumberToObject(dns, "timeoutMs", timeout) ||
        !cJSON_AddBoolToObject(nf, "disableIPv4", a->netfilter.disable_ipv4) ||
        !cJSON_AddBoolToObject(nf, "disableIPv6", a->netfilter.disable_ipv6) ||
        !cJSON_AddNumberToObject(ipset, "additionalTTLSeconds", (uint32_t)(a->netfilter.ipset.additional_ttl / MT_DURATION_SEC)) ||
        !cJSON_AddBoolToObject(root, "showAllInterfaces", a->show_all_interfaces) ||
        !cJSON_AddStringToObject(root, "logLevel", a->log_level && log_level(a->log_level) ? a->log_level : "info")) {
        cJSON_Delete(root); return NULL;
    }
    for (size_t i = 0; i < a->n_link; i++) {
        cJSON *name = cJSON_CreateString(a->link[i]);
        if (!name || !cJSON_AddItemToArray(links, name)) {
            cJSON_Delete(name); cJSON_Delete(root); return NULL;
        }
    }
    return root;
}

mt_err_t mt_settings_revision(const mt_app_config_t *app, char revision[65])
{
    cJSON *json = mt_settings_json(app);
    char *text = json ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json);
    if (!text) { return MT_ERR_NOMEM; }
    uint8_t digest[MT_SHA256_DIGEST_LEN];
    mt_sha256((const uint8_t *)text, strlen(text), digest);
    free(text);
    for (size_t i = 0; i < sizeof(digest); i++) { snprintf(revision + 2 * i, 3, "%02x", digest[i]); }
    return MT_OK;
}

static bool fields(const cJSON *obj, const char *const *keys, size_t count,
                    const char *path, char field[96])
{
    if (!cJSON_IsObject(obj)) { snprintf(field, 96, "%s", path); return false; }
    size_t seen = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, obj) {
        size_t i = 0;
        while (i < count && (!item->string || strcmp(item->string, keys[i]))) { i++; }
        if (i == count || (seen & ((size_t)1 << i))) {
            snprintf(field, 96, "%s%s%s", path, *path ? "." : "", item->string ? item->string : "?");
            return false;
        }
        seen |= (size_t)1 << i;
    }
    for (size_t i = 0; i < count; i++) {
        if (!(seen & ((size_t)1 << i))) {
            snprintf(field, 96, "%s%s%s", path, *path ? "." : "", keys[i]); return false;
        }
    }
    return true;
}

static const cJSON *member(const cJSON *json, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(json, key);
}

static bool number(const cJSON *json, uint32_t min, uint32_t max, uint32_t *out)
{
    if (!cJSON_IsNumber(json) || !isfinite(json->valuedouble) ||
        json->valuedouble < min || json->valuedouble > max) { return false; }
    uint32_t value = (uint32_t)json->valuedouble;
    if ((double)value != json->valuedouble) { return false; }
    *out = value; return true;
}

static bool listen_address(const char *address)
{
    if (!address || !*address || strlen(address) >= INET6_ADDRSTRLEN + 2) { return false; }
    char host[INET6_ADDRSTRLEN + 2];
    snprintf(host, sizeof(host), "%s", address);
    char *p = host;
    size_t len = strlen(p);
    if (len >= 2 && p[0] == '[' && p[len - 1] == ']') { p[len - 1] = '\0'; p++; }
    uint8_t bytes[16];
    return inet_pton(AF_INET, p, bytes) == 1 || inet_pton(AF_INET6, p, bytes) == 1;
}

static bool upstream_address(const char *address)
{
    if (!address || !*address || strlen(address) >= 256) { return false; }
    if (listen_address(address)) { return true; }
    for (const unsigned char *p = (const unsigned char *)address; *p; p++) {
        if (!(isalnum(*p) || *p == '.' || *p == '-' || *p == '_')) { return false; }
    }
    return true;
}

static bool log_level(const char *value)
{
    static const char *const levels[] = {"trace", "debug", "info", "warn", "error", "fatal", "panic", "nolevel", "disabled"};
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        if (!strcmp(value, levels[i])) { return true; }
    }
    return false;
}

mt_err_t mt_settings_parse(const cJSON *json, const mt_app_config_t *base,
                           mt_app_config_t *out, char field[96])
{
    memset(out, 0, sizeof(*out)); field[0] = '\0';
    static const char *const root_keys[] = {"httpWeb", "dnsProxy", "netfilter", "link", "showAllInterfaces", "logLevel"};
    static const char *const http_keys[] = {"enabled", "auth", "host"};
    static const char *const auth_keys[] = {"enabled"};
    static const char *const host_keys[] = {"address", "port"};
    static const char *const dns_keys[] = {"host", "upstream", "disableRemap53", "disableFakePTR", "disableDropAAAA", "maxIdleConns", "maxConcurrent", "timeoutMs"};
    static const char *const nf_keys[] = {"disableIPv4", "disableIPv6", "ipset"};
    static const char *const ipset_keys[] = {"additionalTTLSeconds"};
    const cJSON *http = member(json, "httpWeb"), *auth = member(http, "auth"), *host = member(http, "host");
    const cJSON *dns = member(json, "dnsProxy"), *dns_host = member(dns, "host"), *upstream = member(dns, "upstream");
    const cJSON *nf = member(json, "netfilter"), *ipset = member(nf, "ipset");
#define FIELDS(object, keys, path) fields(object, keys, sizeof(keys) / sizeof((keys)[0]), path, field)
    if (!FIELDS(json, root_keys, "") || !FIELDS(http, http_keys, "httpWeb") ||
        !FIELDS(auth, auth_keys, "httpWeb.auth") || !FIELDS(host, host_keys, "httpWeb.host") ||
        !FIELDS(dns, dns_keys, "dnsProxy") || !FIELDS(dns_host, host_keys, "dnsProxy.host") ||
        !FIELDS(upstream, host_keys, "dnsProxy.upstream") || !FIELDS(nf, nf_keys, "netfilter") ||
        !FIELDS(ipset, ipset_keys, "netfilter.ipset")) { return MT_ERR_INVAL; }
#undef FIELDS
    mt_err_t err = mt_app_config_clone(out, base);
    if (err != MT_OK) { return err; }
    uint32_t value;
#define REQUIRE(check, path) do { if (!(check)) { snprintf(field, 96, "%s", path); err = MT_ERR_INVAL; goto fail; } } while (0)
#define BOOL(object, key, target, path) do { const cJSON *item = member(object, key); REQUIRE(cJSON_IsBool(item), path); target = cJSON_IsTrue(item); } while (0)
#define NUMBER(object, key, min, max, target, path) do { REQUIRE(number(member(object, key), min, max, &value), path); target = value; } while (0)
#define STRING(object, key, target, path, valid) do { const cJSON *item = member(object, key); REQUIRE(cJSON_IsString(item) && (valid), path); err = mt_strset(&(target), item->valuestring); if (err != MT_OK) { goto fail; } } while (0)
    BOOL(http, "enabled", out->http_web.enabled, "httpWeb.enabled");
    BOOL(auth, "enabled", out->http_web.auth.enabled, "httpWeb.auth.enabled");
    STRING(host, "address", out->http_web.host.address, "httpWeb.host.address", listen_address(item->valuestring));
    NUMBER(host, "port", 1, UINT16_MAX, value, "httpWeb.host.port"); out->http_web.host.port = (uint16_t)value;
    STRING(dns_host, "address", out->dns_proxy.host.address, "dnsProxy.host.address", listen_address(item->valuestring));
    NUMBER(dns_host, "port", 1, UINT16_MAX, value, "dnsProxy.host.port"); out->dns_proxy.host.port = (uint16_t)value;
    STRING(upstream, "address", out->dns_proxy.upstream.address, "dnsProxy.upstream.address", upstream_address(item->valuestring));
    NUMBER(upstream, "port", 1, UINT16_MAX, value, "dnsProxy.upstream.port"); out->dns_proxy.upstream.port = (uint16_t)value;
    BOOL(dns, "disableRemap53", out->dns_proxy.disable_remap53, "dnsProxy.disableRemap53");
    BOOL(dns, "disableFakePTR", out->dns_proxy.disable_fake_ptr, "dnsProxy.disableFakePTR");
    BOOL(dns, "disableDropAAAA", out->dns_proxy.disable_drop_aaaa, "dnsProxy.disableDropAAAA");
    NUMBER(dns, "maxIdleConns", 0, UINT32_MAX, out->dns_proxy.max_idle_conns, "dnsProxy.maxIdleConns");
    NUMBER(dns, "maxConcurrent", 1, UINT32_MAX, out->dns_proxy.max_concurrent, "dnsProxy.maxConcurrent");
    NUMBER(dns, "timeoutMs", 1, UINT32_MAX, value, "dnsProxy.timeoutMs"); out->dns_proxy.timeout = (mt_duration_t)value * MT_DURATION_MS;
    BOOL(nf, "disableIPv4", out->netfilter.disable_ipv4, "netfilter.disableIPv4");
    BOOL(nf, "disableIPv6", out->netfilter.disable_ipv6, "netfilter.disableIPv6");
    NUMBER(ipset, "additionalTTLSeconds", 0, UINT32_MAX, value, "netfilter.ipset.additionalTTLSeconds"); out->netfilter.ipset.additional_ttl = (mt_duration_t)value * MT_DURATION_SEC;
    BOOL(json, "showAllInterfaces", out->show_all_interfaces, "showAllInterfaces");
    STRING(json, "logLevel", out->log_level, "logLevel", log_level(item->valuestring));
    const cJSON *links = member(json, "link"), *link;
    REQUIRE(cJSON_IsArray(links), "link");
    for (size_t i = 0; i < out->n_link; i++) { free(out->link[i]); }
    free(out->link); out->link = NULL; out->n_link = 0;
    size_t count = (size_t)cJSON_GetArraySize(links);
    if (count) {
        out->link = calloc(count, sizeof(*out->link));
        if (!out->link) { err = MT_ERR_NOMEM; goto fail; }
        cJSON_ArrayForEach(link, links) {
            REQUIRE(cJSON_IsString(link) && *link->valuestring && strlen(link->valuestring) < IF_NAMESIZE, "link");
            for (const unsigned char *p = (const unsigned char *)link->valuestring; *p; p++) { REQUIRE(*p != '/' && !isspace(*p) && !iscntrl(*p), "link"); }
            for (size_t i = 0; i < out->n_link; i++) { REQUIRE(strcmp(out->link[i], link->valuestring), "link"); }
            err = mt_strset(&out->link[out->n_link], link->valuestring);
            if (err != MT_OK) { goto fail; }
            out->n_link++;
        }
    }
#undef STRING
#undef NUMBER
#undef BOOL
#undef REQUIRE
    return MT_OK;
fail:
    mt_app_config_clear(out); memset(out, 0, sizeof(*out)); return err;
}

static void fail_operation(mt_settings_t *s, const char *message)
{
    s->state = "failed";
    snprintf(s->error, sizeof(s->error), "%s", message);
    s->ready = false;
}

static mt_app_config_t effective_settings(const mt_settings_t *s)
{
    mt_app_config_t app = s->config->app; /* borrowed strings; projection only */
    if (!app.http_web.host.port) { app.http_web.host.port = s->http_port ? s->http_port : 8080; }
    if (!app.dns_proxy.host.port && s->dns_port) { app.dns_proxy.host.port = s->dns_port; }
    return app;
}

void mt_settings_overlay_live(mt_app_config_t *target, const mt_app_config_t *source)
{
    target->log_level = source->log_level;
    target->show_all_interfaces = source->show_all_interfaces;
    target->http_web.auth.enabled = source->http_web.auth.enabled;
    target->dns_proxy.disable_fake_ptr = source->dns_proxy.disable_fake_ptr;
    target->dns_proxy.disable_drop_aaaa = source->dns_proxy.disable_drop_aaaa;
    target->netfilter.ipset.additional_ttl = source->netfilter.ipset.additional_ttl;
}

/* Compare effective public values, including legacy zero/default projections.
 * The same overlay defines classification, persistence and live mutation. */
static mt_err_t needs_dns_restart(const mt_settings_t *s, const mt_app_config_t *current,
                                  const mt_app_config_t *candidate, const char *revision,
                                  const char *target_revision, bool *required)
{
    *required = true;
    if (!s->live_apply || strcmp(revision, s->active_revision)) { return MT_OK; }
    mt_app_config_t live = *current;
    mt_settings_overlay_live(&live, candidate);
    char live_revision[65];
    mt_err_t err = mt_settings_revision(&live, live_revision);
    if (err == MT_OK) { *required = strcmp(live_revision, target_revision) != 0; }
    return err;
}

/* Called only on the event-loop owner. No other request can mutate cfg
 * between this final comparison/snapshot and the loop's exit. */
static void maybe_apply(mt_settings_t *s)
{
    if (!s->loop || !s->acknowledged || !s->resolved || strcmp(s->state, "applying")) { return; }
    if (s->resolve_error != MT_OK) { fail_operation(s, "Cannot resolve the DNS upstream address"); return; }
    char revision[65], field[96];
    mt_app_config_t effective = effective_settings(s);
    if (mt_settings_revision(&effective, revision) != MT_OK) { fail_operation(s, "Cannot prepare settings"); return; }
    if (strcmp(revision, s->revision)) { fail_operation(s, "Settings changed before they could be applied"); return; }
    bool restart;
    if (needs_dns_restart(s, &effective, &s->requested, revision, s->target_revision, &restart) != MT_OK) {
        fail_operation(s, "Cannot prepare settings"); return;
    }
    if (restart && (!s->allow_dns_restart || !s->dns_restart_required)) {
        fail_operation(s, "Settings now require a DNS restart; save again to confirm"); return;
    }
    cJSON *json = mt_settings_json(&s->requested);
    mt_err_t err = json ? mt_settings_parse(json, &s->config->app, &s->candidate, field) : MT_ERR_NOMEM;
    cJSON_Delete(json);
    if (err != MT_OK) { fail_operation(s, "Cannot prepare settings"); return; }
    if (!restart) {
        err = s->live_apply(&s->candidate, s->live_userdata);
        mt_app_config_clear(&s->candidate); memset(&s->candidate, 0, sizeof(s->candidate));
        if (err == MT_OK) {
            mt_settings_runtime_started(s);
            mt_settings_finish(s, NULL, false);
        } else {
            char message[160];
            snprintf(message, sizeof(message), "Cannot save live settings (%s); settings were not changed", mt_err_str(err));
            mt_settings_finish(s, message, false);
        }
        return;
    }
    s->ready = true;
    mt_loop_stop(s->loop);
}

static void response_sent(void *userdata, bool sent)
{
    mt_settings_t *s = userdata;
    s->response_pending = false;
    s->acknowledged = sent;
    if (!sent) { fail_operation(s, "Settings acknowledgement was not delivered; nothing was changed"); return; }
    maybe_apply(s);
}

static void *resolve_upstream(void *userdata)
{
    mt_settings_t *s = userdata;
    s->resolve_error = mt_dnsproxy_resolve_upstream(s->requested.dns_proxy.upstream.address,
                                                   s->requested.dns_proxy.upstream.port, &s->upstream);
    const uint8_t done = 1;
    ssize_t written;
    do { written = write(s->resolve_pipe[1], &done, 1); } while (written < 0 && errno == EINTR);
    return NULL;
}

static void close_resolver_pipe(mt_settings_t *s)
{
    if (s->resolve_pipe[0] >= 0) {
        (void)mt_loop_del_fd(s->loop, s->resolve_pipe[0]);
        close(s->resolve_pipe[0]); s->resolve_pipe[0] = -1;
    }
    if (s->resolve_pipe[1] >= 0) { close(s->resolve_pipe[1]); s->resolve_pipe[1] = -1; }
}

static void resolved(mt_loop_t *loop, int fd, uint32_t events, void *userdata)
{
    (void)loop; (void)fd; (void)events;
    mt_settings_t *s = userdata;
    pthread_join(s->resolver, NULL);
    s->resolving = false; s->resolved = true;
    close_resolver_pipe(s);
    maybe_apply(s);
}

mt_settings_t *mt_settings_create(mt_config_t *config)
{
    mt_settings_t *s = calloc(1, sizeof(*s));
    if (s) { s->config = config; s->resolve_pipe[0] = s->resolve_pipe[1] = -1; }
    return s;
}

void mt_settings_destroy(mt_settings_t *s)
{
    if (!s) { return; }
    if (s->resolving) { pthread_join(s->resolver, NULL); }
    close_resolver_pipe(s);
    mt_app_config_clear(&s->requested);
    mt_app_config_clear(&s->candidate);
    free(s);
}

void mt_settings_attach_loop(mt_settings_t *s, mt_loop_t *loop) {
    if (!s) { return; }
    if (!loop) {
        if (s->resolving) { pthread_join(s->resolver, NULL); s->resolving = false; }
        close_resolver_pipe(s);
        s->live_apply = NULL; s->live_userdata = NULL;
    }
    s->loop = loop;
}
void mt_settings_set_live_apply(mt_settings_t *s, mt_settings_live_apply_fn apply, void *userdata)
{
    if (s) { s->live_apply = apply; s->live_userdata = userdata; }
}
bool mt_settings_busy(const mt_settings_t *s) { return s && (s->resolving || s->response_pending || (s->state && !strcmp(s->state, "applying"))); }
bool mt_settings_ready(const mt_settings_t *s) { return s && s->ready; }
void mt_settings_bound_ports(mt_settings_t *s, uint16_t http, uint16_t dns) { if (s) { s->http_port = http; s->dns_port = dns; } }
void mt_settings_runtime_started(mt_settings_t *s) {
    if (!s) { return; }
    mt_app_config_t effective = effective_settings(s);
    if (mt_settings_revision(&effective, s->active_revision) != MT_OK) { s->active_revision[0] = '\0'; }
}
void mt_settings_runtime_stopped(mt_settings_t *s) { if (s) { s->active_revision[0] = '\0'; } }

bool mt_settings_take(mt_settings_t *s, mt_app_config_t *candidate, mt_dns_upstream_t *upstream)
{
    if (!mt_settings_ready(s)) { return false; }
    *candidate = s->candidate; memset(&s->candidate, 0, sizeof(s->candidate));
    *upstream = s->upstream; s->ready = false;
    return true;
}

void mt_settings_finish(mt_settings_t *s, const char *error, bool rollback_failed)
{
    if (rollback_failed) { mt_settings_runtime_stopped(s); }
    s->state = error ? (rollback_failed ? "rollback_failed" : "failed") : "succeeded";
    snprintf(s->error, sizeof(s->error), "%s", error ? error : "");
    s->ready = false;
    mt_app_config_clear(&s->requested); memset(&s->requested, 0, sizeof(s->requested));
}

static cJSON *operation_json(const mt_settings_t *s)
{
    if (!s->state) { return cJSON_CreateNull(); }
    cJSON *json = cJSON_CreateObject();
    if (!json || !cJSON_AddStringToObject(json, "id", s->id) ||
        !cJSON_AddStringToObject(json, "state", s->state) ||
        !cJSON_AddStringToObject(json, "target_revision", s->target_revision) ||
        (*s->error && !cJSON_AddStringToObject(json, "error", s->error))) { cJSON_Delete(json); return NULL; }
    return json;
}

static void api_error(mt_http_res_t *res, int status, const char *code, const char *message, const char *field)
{
    cJSON *json = mt_json_error(message);
    if (json) { cJSON_AddStringToObject(json, "code", code); if (field && *field) { cJSON_AddStringToObject(json, "field", field); } }
    (void)mt_http_res_write_json(res, status, json);
}

static void get_settings(mt_http_req_t *req, mt_http_res_t *res, void *userdata)
{
    (void)req;
    mt_settings_t *s = userdata;
    mt_http_res_set_header(res, "Cache-Control", "no-store");
    char revision[65];
    mt_app_config_t effective = s ? effective_settings(s) : (mt_app_config_t){0};
    if (!s || mt_settings_revision(&effective, revision) != MT_OK) {
        api_error(res, 503, "SETTINGS_UNAVAILABLE", "Settings are unavailable", NULL); return;
    }
    cJSON *json = cJSON_CreateObject(), *settings = mt_settings_json(&effective), *operation = operation_json(s);
    if (!json || !settings || !operation || !cJSON_AddStringToObject(json, "revision", revision)) {
        cJSON_Delete(json); cJSON_Delete(settings); cJSON_Delete(operation);
        api_error(res, 503, "SETTINGS_UNAVAILABLE", "Cannot read settings", NULL); return;
    }
    if (!cJSON_AddItemToObject(json, "settings", settings)) {
        cJSON_Delete(settings); cJSON_Delete(operation); cJSON_Delete(json);
        api_error(res, 503, "SETTINGS_UNAVAILABLE", "Cannot read settings", NULL); return;
    }
    if (!cJSON_AddItemToObject(json, "operation", operation)) {
        cJSON_Delete(operation); cJSON_Delete(json);
        api_error(res, 503, "SETTINGS_UNAVAILABLE", "Cannot read settings", NULL); return;
    }
    (void)mt_http_res_write_json(res, 200, json);
}

static cJSON *acknowledgement(const mt_settings_t *s, const mt_app_config_t *app)
{
    cJSON *json = cJSON_CreateObject(), *operation = operation_json(s);
    cJSON *web = json ? cJSON_AddObjectToObject(json, "next_web") : NULL;
    if (!json || !operation || !web || !cJSON_AddStringToObject(json, "target_revision", s->target_revision) ||
        !cJSON_AddBoolToObject(web, "enabled", app->http_web.enabled) ||
        !cJSON_AddStringToObject(web, "address", app->http_web.host.address) ||
        !cJSON_AddNumberToObject(web, "port", app->http_web.host.port) ||
        !cJSON_AddBoolToObject(web, "auth_enabled", app->http_web.auth.enabled)) {
        cJSON_Delete(json); cJSON_Delete(operation); return NULL;
    }
    if (!cJSON_AddItemToObject(json, "operation", operation)) {
        cJSON_Delete(operation); cJSON_Delete(json); return NULL;
    }
    return json;
}

typedef struct settings_request {
    mt_app_config_t candidate;
    char revision[65], target_revision[65];
    bool allow_dns_restart, dns_restart_required;
} settings_request_t;

/* Preview and PUT must validate and classify exactly the same projection.
 * Preview performs no resolution, reservation, persistence or live mutation. */
static bool read_settings_request(mt_http_req_t *req, mt_http_res_t *res,
                                  mt_settings_t *s, bool preview, settings_request_t *out)
{
    mt_http_res_set_header(res, "Cache-Control", "no-store");
    if (!s || !s->loop) { api_error(res, 503, "SETTINGS_UNAVAILABLE", "Settings cannot be applied", NULL); return false; }
    if (mt_settings_busy(s)) { api_error(res, 409, "SETTINGS_BUSY", "Settings are already being applied", NULL); return false; }
    if (mt_update_busy()) { api_error(res, 409, "SETTINGS_BUSY", "Wait for the software update to finish", NULL); return false; }
    const char *site = mt_http_req_header(req, "Sec-Fetch-Site"), *type = mt_http_req_header(req, "Content-Type");
    if ((site && !strcmp(site, "cross-site")) || !type || strncmp(type, "application/json", 16) ||
        (type[16] && type[16] != ';')) { api_error(res, 400, "SETTINGS_INVALID", "Expected a same-origin JSON request", NULL); return false; }
    size_t length;
    const uint8_t *body = mt_http_req_body(req, &length);
    cJSON *json = mt_json_parse_document((const char *)body, length);
    const char *const keys[] = {"settings", "revision", "allow_dns_restart"};
    char field[96];
    const cJSON *expected = member(json, "revision"), *allow = member(json, "allow_dns_restart");
    size_t key_count = !preview && allow ? 3 : 2;
    if (!fields(json, keys, key_count, "", field) || !cJSON_IsString(expected) || strlen(expected->valuestring) != 64) {
        cJSON_Delete(json); api_error(res, 400, "SETTINGS_INVALID", "Invalid settings request", NULL); return false;
    }
    if (allow && !cJSON_IsBool(allow)) {
        cJSON_Delete(json); api_error(res, 400, "SETTINGS_INVALID", "Invalid settings field", "allow_dns_restart"); return false;
    }
    out->allow_dns_restart = !allow || cJSON_IsTrue(allow);
    mt_app_config_t effective = effective_settings(s);
    if (mt_settings_revision(&effective, out->revision) != MT_OK) {
        cJSON_Delete(json); api_error(res, 503, "SETTINGS_UNAVAILABLE", "Cannot read settings", NULL); return false;
    }
    if (strcmp(out->revision, expected->valuestring)) {
        cJSON_Delete(json); api_error(res, 409, "SETTINGS_CONFLICT", "Settings changed since they were loaded", NULL); return false;
    }
    mt_err_t err = mt_settings_parse(member(json, "settings"), &s->config->app, &out->candidate, field);
    cJSON_Delete(json);
    if (err != MT_OK) {
        api_error(res, err == MT_ERR_INVAL ? 400 : 503, err == MT_ERR_INVAL ? "SETTINGS_INVALID" : "SETTINGS_UNAVAILABLE",
                   err == MT_ERR_INVAL ? "Invalid settings field" : "Cannot prepare settings", field); return false;
    }
    if (mt_settings_revision(&out->candidate, out->target_revision) != MT_OK ||
        needs_dns_restart(s, &effective, &out->candidate, out->revision,
                          out->target_revision, &out->dns_restart_required) != MT_OK) {
        mt_app_config_clear(&out->candidate);
        api_error(res, 503, "SETTINGS_UNAVAILABLE", "Cannot prepare settings", NULL); return false;
    }
    return true;
}

static void preview_settings(mt_http_req_t *req, mt_http_res_t *res, void *userdata)
{
    settings_request_t request = {0};
    if (!read_settings_request(req, res, userdata, true, &request)) { return; }
    mt_app_config_clear(&request.candidate);
    cJSON *json = cJSON_CreateObject();
    if (!json || !cJSON_AddBoolToObject(json, "dns_restart_required", request.dns_restart_required)) {
        cJSON_Delete(json); api_error(res, 503, "SETTINGS_UNAVAILABLE", "Cannot prepare settings preview", NULL); return;
    }
    (void)mt_http_res_write_json(res, 200, json);
}

static void put_settings(mt_http_req_t *req, mt_http_res_t *res, void *userdata)
{
    mt_settings_t *s = userdata;
    settings_request_t request = {0};
    if (!read_settings_request(req, res, s, false, &request)) { return; }
    if (request.dns_restart_required && !request.allow_dns_restart) {
        mt_app_config_clear(&request.candidate);
        api_error(res, 409, "SETTINGS_RESTART_REQUIRED", "Settings require a DNS restart; preview and confirm before saving", NULL); return;
    }
    uint8_t id[16];
    if (mt_random_bytes(id, sizeof(id)) != MT_OK) {
        mt_app_config_clear(&request.candidate); api_error(res, 503, "SETTINGS_UNAVAILABLE", "Cannot prepare settings", NULL); return;
    }
    mt_app_config_clear(&s->requested); s->requested = request.candidate;
    memset(&request.candidate, 0, sizeof(request.candidate));
    for (size_t i = 0; i < sizeof(id); i++) { snprintf(s->id + 2 * i, 3, "%02x", id[i]); }
    snprintf(s->revision, sizeof(s->revision), "%s", request.revision);
    snprintf(s->target_revision, sizeof(s->target_revision), "%s", request.target_revision);
    s->dns_restart_required = request.dns_restart_required;
    s->allow_dns_restart = request.allow_dns_restart;
    s->error[0] = '\0'; s->resolved = false; s->acknowledged = false; s->ready = false;
    bool unchanged = !strcmp(request.revision, request.target_revision) && !request.dns_restart_required;
    s->state = unchanged ? "succeeded" : "applying";
    cJSON *ack = acknowledgement(s, &s->requested);
    if (!ack) { fail_operation(s, "Cannot encode settings acknowledgement"); api_error(res, 503, "SETTINGS_UNAVAILABLE", s->error, NULL); return; }
    if (unchanged) { (void)mt_http_res_write_json(res, 200, ack); return; }
    if (s->dns_restart_required) {
        if (pipe(s->resolve_pipe) != 0) { goto unavailable; }
        for (size_t i = 0; i < 2; i++) {
            int flags = fcntl(s->resolve_pipe[i], F_GETFD);
            if (flags < 0 || fcntl(s->resolve_pipe[i], F_SETFD, flags | FD_CLOEXEC) < 0) { goto unavailable; }
        }
        if (mt_loop_add_fd(s->loop, s->resolve_pipe[0], EPOLLIN, resolved, s) != MT_OK) { goto unavailable; }
        if (pthread_create(&s->resolver, NULL, resolve_upstream, s) != 0) { goto unavailable; }
        s->resolving = true;
    } else {
        s->resolved = true; s->resolve_error = MT_OK;
    }
    if (!mt_http_res_write_json(res, 202, ack)) { response_sent(s, false); return; }
    s->response_pending = true;
    mt_http_res_on_sent(res, response_sent, s);
    return;
unavailable:
    close_resolver_pipe(s); cJSON_Delete(ack);
    fail_operation(s, "Cannot start settings preparation");
    api_error(res, 503, "SETTINGS_UNAVAILABLE", s->error, NULL);
}

void mt_settings_register_routes(mt_httpd_t *http, mt_settings_t *settings)
{
    mt_httpd_route(http, "GET", "/api/v1/system/settings", get_settings, settings);
    mt_httpd_route(http, "PUT", "/api/v1/system/settings", put_settings, settings);
    mt_httpd_route(http, "POST", "/api/v1/system/settings/preview", preview_settings, settings);
}
