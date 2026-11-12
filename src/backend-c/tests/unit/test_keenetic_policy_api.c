/* Policy API over real loopback HTTP and the real netfilter compiler.
 * RCI and iptables transports are in-memory; no router/kernel rules are changed. */
#include "greatest.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cjson/cJSON.h>

#include "magitrickle/app.h"
#include "magitrickle/dns_cache.h"
#include "magitrickle/loop.h"
#include "magitrickle/system.h"
#include "magitrickle/groups.h"
#include "magitrickle/subscriptions_api.h"
#include "magitrickle/yamlio.h"

#ifdef MT_ENTWARE_KN
#include "magitrickle/keenetic_policy_api.h"
#include "magitrickle/netfilter_cleaner.h"
#include "magitrickle/port_remap.h"
#include "fake_iptables.h"
static atomic_int fetch_calls, fetch_mode;
static atomic_bool hold_fetch;
static mt_err_t mock_fetch(mt_kn_policy_entry_t **entries, size_t *count, const atomic_bool *stop) {
    atomic_fetch_add(&fetch_calls, 1);
    for (int i = 0; i < 100 && atomic_load(&hold_fetch) && !atomic_load(stop); i++) {
        struct timespec ts = {0, 10000000}; nanosleep(&ts, NULL);
    }
    *entries = NULL; *count = 0;
    int mode = atomic_load(&fetch_mode);
    if (mode == 1) { return MT_ERR_UPSTREAM; }
    if (mode == 6) { return MT_ERR_TIMEOUT; }
    if (mode == 7) { return MT_ERR_LIMIT; }
    const char *json = mode == 2 ? "{}" :
      mode == 3 ? "{\"Policy42\":{\"description\":\"mt\",\"mark\":\"ffffd00\"}}" :
      mode == 4 ? "{\"ImportedPolicy42\":{\"description\":\"Imported VPN\",\"mark\":\"ffffd00\"}}" :
      mode == 5 ? "{\"Policy0\":{\"mark\":\"not-a-mark\"}}" :
      mode == 8 ? "{\"Policy0\":{\"description\":\"VPN\",\"mark\":\"ffffd01\"}}" :
      mode == 9 ? "{\"Policy0\":{\"description\":\"VPN\",\"mark\":0}}" :
      mode == 10 ? "{\"Policy0\":{\"description\":\"VPN\"}}" :
      "{\"Policy0\":{\"description\":\"VPN\",\"mark\":\"ffffd00\"},\"Waiting\":{}}";
    return mt_kn_policy_parse(json, strlen(json), entries, count);
}

/* Count writes, not just final contents: rewriting identical tables would
 * still introduce a transient routing/DNS gap. Access only on the loop. */
typedef struct counted_ipt {
    mt_ipt_executable_t base;
    mt_ipt_executable_t *inner;
    unsigned restores;
} counted_ipt_t;
static mt_err_t counted_save(mt_ipt_executable_t *exe, uint8_t **out, size_t *len) {
    counted_ipt_t *c = (counted_ipt_t *)exe;
    return c->inner->ops->save(c->inner, out, len);
}
static mt_err_t counted_restore(mt_ipt_executable_t *exe, const uint8_t *data, size_t len) {
    counted_ipt_t *c = (counted_ipt_t *)exe;
    c->restores++;
    return c->inner->ops->restore(c->inner, data, len);
}
static mt_ipt_proto_t counted_proto(mt_ipt_executable_t *exe) {
    counted_ipt_t *c = (counted_ipt_t *)exe;
    return c->inner->ops->proto(c->inner);
}
static void counted_destroy(mt_ipt_executable_t *exe) {
    counted_ipt_t *c = (counted_ipt_t *)exe;
    mt_ipt_executable_free(c->inner); free(c);
}
static const mt_ipt_executable_ops_t counted_ops = {
    .save = counted_save, .restore = counted_restore,
    .proto = counted_proto, .destroy = counted_destroy,
};
static counted_ipt_t *counted_new(mt_ipt_proto_t proto) {
    counted_ipt_t *c = calloc(1, sizeof(*c));
    if (!c) { return NULL; }
    mt_fake_ipt_t *f = mt_fake_ipt_new(proto);
    if (!f) { free(c); return NULL; }
    c->base.ops = &counted_ops;
    c->inner = mt_fake_ipt_as_executable(f);
    return c;
}
#endif

typedef struct harness {
#ifdef MT_ENTWARE_KN
    mt_kn_policy_service_t *service;
    mt_ipt_t *ipt[2];
    counted_ipt_t *transport[2]; /* owned by ipt */
    mt_port_remap_t *remap;
#endif
    mt_loop_t *loop;
    mt_httpd_t *tcp;
    mt_config_t cfg;
    mt_cache_t *cache;
    mt_app_t *app;
    mt_system_ctx_t ctx;
    mt_groups_ctx_t groups;
    mt_subs_ctx_t subs;
    char config_path[64];
    pthread_t thread;
} harness_t;

#ifdef MT_ENTWARE_KN
/* Test-only route: collect snapshots on the loop thread, never race a worker
 * or read the app/iptables structures directly from the HTTP client thread. */
static void netfilter_snapshot(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req;
    harness_t *h = ud;
    cJSON *out = cJSON_CreateObject();
    for (size_t i = 0; out && i < 2; i++) {
        uint8_t *bytes = NULL; size_t len = 0;
        mt_ipt_executable_t *inner = h->transport[i]->inner;
        mt_err_t err = inner->ops->save(inner, &bytes, &len);
        char *text = err == MT_OK && len < SIZE_MAX ? malloc(len + 1) : NULL;
        if (text) {
            if (len) { memcpy(text, bytes, len); }
            text[len] = 0;
        }
        free(bytes);
        if (!text || !cJSON_AddStringToObject(out, i ? "ipv6" : "ipv4", text) ||
            !cJSON_AddNumberToObject(out, i ? "restores6" : "restores4", h->transport[i]->restores)) {
            cJSON_Delete(out); out = NULL;
        }
        free(text);
    }
    if (out) { mt_http_res_write_json(res, 200, out); }
    else { mt_http_res_write_error(res, 500, "cannot snapshot test netfilter"); }
}
#endif

static void *loop_thread(void *ud) {
    harness_t *h = ud;
    mt_loop_run(h->loop);
    return NULL;
}

#define TEST_PORT 18129

static harness_t *harness_start_seeded(bool with_config_path, const char *yaml, int initial_fetch_mode) {
    harness_t *h = calloc(1, sizeof(*h));
    if (!h || mt_config_init_defaults(&h->cfg) != MT_OK) { return NULL; }
    if (yaml && mt_config_load_buffer(&h->cfg, yaml, strlen(yaml)) != MT_OK) { return NULL; }
    h->cache = mt_cache_create(0);
    mt_app_deps_t deps = {.cfg = &h->cfg, .cache = h->cache};
#ifdef MT_ENTWARE_KN
    for (size_t i = 0; i < 2; i++) {
        h->transport[i] = counted_new(i ? MT_IPT_PROTO_IPV6 : MT_IPT_PROTO_IPV4);
        if (!h->transport[i]) { return NULL; }
        h->ipt[i] = mt_ipt_new(&h->transport[i]->base);
        if (!h->ipt[i]) { return NULL; }
    }
    deps.ipt4 = h->ipt[0]; deps.ipt6 = h->ipt[1];
    if (mt_netfilter_register_base_chains(deps.ipt4, deps.ipt6) != MT_OK) { return NULL; }
#else
    (void)initial_fetch_mode;
#endif
    h->app = mt_app_create(&deps);
    if (!h->app) { return NULL; }
#ifdef MT_ENTWARE_KN
    const mt_remap_addr_t addrs[] = {
        {.family = AF_INET, .ip = {192,168,1,1}, .iplen = 4},
        {.family = AF_INET6, .ip = {0xfd,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1}, .iplen = 16},
    };
    h->remap = mt_port_remap_new("MT_", 53, 5353, addrs, 2, deps.ipt4, deps.ipt6);
    if (!h->remap) { return NULL; }
    mt_app_set_port_remap(h->app, h->remap);
    if (mt_port_remap_enable(h->remap) != MT_OK || mt_app_rebuild_netfilter(h->app, NULL) != MT_OK) { return NULL; }
#endif
    h->ctx.app = h->app;
    if (with_config_path) {
        snprintf(h->config_path, sizeof(h->config_path), "/tmp/mt_system_test_XXXXXX");
        int fd = mkstemp(h->config_path);
        if (fd >= 0) { close(fd); }
        h->ctx.config_path = h->config_path;
        h->ctx.config_version = "0.1";
    }

    if (mt_loop_create(&h->loop) != MT_OK) { return NULL; }
    if (mt_httpd_create(h->loop, &h->tcp) != MT_OK) { return NULL; }
#ifdef MT_ENTWARE_KN
    atomic_store(&fetch_calls, 0); atomic_store(&fetch_mode, initial_fetch_mode); atomic_store(&hold_fetch, false);
    if (mt_kn_policy_service_create(h->loop, h->app, h->ctx.config_path, "0.8", mock_fetch, &h->service) != MT_OK) { return NULL; }
    if (mt_kn_policy_register_routes(h->tcp, h->service) != MT_OK) { return NULL; }
    if (mt_httpd_route(h->tcp, "GET", "/test/netfilter", netfilter_snapshot, h) != MT_OK) { return NULL; }
#endif
    mt_system_register_routes(h->tcp, &h->ctx);
    h->groups.app = h->app; h->subs.app = h->app;
    mt_groups_register_routes(h->tcp, &h->groups);
    mt_subs_register_routes(h->tcp, &h->subs);
    if (mt_httpd_listen_tcp(h->tcp, "127.0.0.1", TEST_PORT) != MT_OK) { return NULL; }

    pthread_create(&h->thread, NULL, loop_thread, h);
    return h;
}
static harness_t *harness_start(bool with_config_path) {
    return harness_start_seeded(with_config_path, NULL, 0);
}

static void harness_stop(harness_t *h) {
    mt_loop_stop(h->loop);
    pthread_join(h->thread, NULL);
#ifdef MT_ENTWARE_KN
    mt_kn_policy_service_destroy(h->service);
#endif
    mt_httpd_destroy(h->tcp);
    mt_loop_destroy(h->loop);
    mt_app_destroy(h->app);
#ifdef MT_ENTWARE_KN
    mt_port_remap_free(h->remap);
    mt_ipt_free(h->ipt[0]); mt_ipt_free(h->ipt[1]);
#endif
    mt_cache_destroy(h->cache);
    mt_config_clear(&h->cfg);
    if (h->config_path[0]) { unlink(h->config_path); }
    free(h);
}

/* ---- tiny blocking HTTP/1.1 client (see test_groups.c) ----------------------- */

static int connect_tcp(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    for (int i = 0; i < 50; i++) {
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) { return fd; }
        struct timespec ts = {0, 10000000};
        nanosleep(&ts, NULL);
    }
    close(fd);
    return -1;
}

static ssize_t recv_response(int fd, char *buf, size_t cap) {
    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    size_t total = 0;
    char *body_start = NULL;
    long content_length = -1;
    for (;;) {
        if (total >= cap - 1) { break; }
        ssize_t n = recv(fd, buf + total, cap - 1 - total, 0);
        if (n <= 0) { break; }
        total += (size_t)n;
        buf[total] = '\0';
        if (!body_start) {
            char *marker = strstr(buf, "\r\n\r\n");
            if (marker) {
                body_start = marker + 4;
                char *cl = strstr(buf, "Content-Length:");
                if (cl && cl < marker) { content_length = strtol(cl + 15, NULL, 10); }
            }
        }
        if (body_start && content_length >= 0) {
            size_t body_have = (size_t)(buf + total - body_start);
            if ((long)body_have >= content_length) { break; }
        }
    }
    return (ssize_t)total;
}

static int status_code_of(const char *resp) {
    int code = 0;
    sscanf(resp, "HTTP/1.1 %d", &code);
    return code;
}

static const char *body_of(const char *resp) {
    const char *marker = strstr(resp, "\r\n\r\n");
    return marker ? marker + 4 : "";
}

static int do_request(const char *method, const char *path, const char *body, cJSON **out_json) {
    int fd = connect_tcp(TEST_PORT);
    if (fd < 0) { return -1; }
    char req[8192];
    size_t body_len = body ? strlen(body) : 0;
    snprintf(req, sizeof(req),
            "%s %s HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
            "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
            method, path, body_len, body ? body : "");
    if (send(fd, req, strlen(req), 0) <= 0) {
        close(fd);
        return -1;
    }
    char resp[16384];
    ssize_t got = recv_response(fd, resp, sizeof(resp));
    close(fd);
    if (got <= 0) { return -1; }
    int status = status_code_of(resp);
    if (out_json) {
        const char *b = body_of(resp);
        *out_json = b[0] ? cJSON_Parse(b) : NULL;
    }
    return status;
}

TEST generic_has_no_policy_routes(void) {
#ifndef MT_ENTWARE_KN
    harness_t *h = harness_start(false); ASSERT(h);
    ASSERT_EQ(404, do_request("GET", "/api/v1/keenetic/policy", NULL, NULL));
    ASSERT_EQ(404, do_request("GET", "/api/v1/keenetic/policies", NULL, NULL));
    ASSERT_EQ(404, do_request("PUT", "/api/v1/keenetic/policy", "{}", NULL));
    harness_stop(h);
#endif
    PASS();
}
#ifdef MT_ENTWARE_KN
static const char *include = "{\"enabled\":true,\"policy_id\":\"Policy0\",\"mode\":\"include\"}";
static const char *exclude = "{\"enabled\":true,\"policy_id\":\"Policy0\",\"mode\":\"exclude\"}";
static const char *disabled = "{\"enabled\":false,\"policy_id\":\"Policy0\",\"mode\":\"exclude\"}";
static cJSON *settings(cJSON *out) { return cJSON_GetObjectItemCaseSensitive(out, "settings"); }
static const char *state(cJSON *out) {
    return cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(out, "runtime"), "state")->valuestring;
}
static char *snapshot_text(void) {
    cJSON *out = NULL;
    if (do_request("GET", "/test/netfilter", NULL, &out) != 200) { cJSON_Delete(out); return NULL; }
    char *text = cJSON_PrintUnformatted(out);
    cJSON_Delete(out); return text;
}
static int assert_snapshot_unchanged(const char *before) {
    char *after = snapshot_text(); ASSERT(after);
    ASSERT_STR_EQ(before, after);
    free(after); PASS();
}
static int assert_active_mark(const char *mark, bool exclude_mode) {
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("GET", "/test/netfilter", NULL, &out)); ASSERT(out);
    for (unsigned family = 0; family < 2; family++) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(out, family ? "ipv6" : "ipv4");
        ASSERT(cJSON_IsString(value));
        char gate[160], dns[160];
        snprintf(gate, sizeof(gate), "-A MT_POLICY -m mark %s--mark %s -j RETURN", exclude_mode ? "" : "! ", mark);
        snprintf(dns, sizeof(dns), "-A MT_DNSOR -m mark %s--mark %s -j RETURN", exclude_mode ? "" : "! ", mark);
        ASSERT(strstr(value->valuestring, gate));
        ASSERT(strstr(value->valuestring, dns));
        ASSERT_FALSE(strstr(value->valuestring, "-A MT_POLICY -j RETURN"));
    }
    cJSON_Delete(out); PASS();
}
static int assert_open_gates(void) {
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("GET", "/test/netfilter", NULL, &out)); ASSERT(out);
    for (unsigned family = 0; family < 2; family++) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(out, family ? "ipv6" : "ipv4");
        ASSERT(cJSON_IsString(value));
        ASSERT(strstr(value->valuestring, "-A MT_POLICY -j MT_POLICY_RULES"));
        ASSERT(strstr(value->valuestring, "-A MT_POLICY -m conntrack --ctdir REPLY -j RETURN"));
        ASSERT(strstr(value->valuestring, family ? "-d ::1/128 -j RETURN" : "-d 127.0.0.1/32 -j RETURN"));
        ASSERT(strstr(value->valuestring, "-A MT_DNSOR -p tcp"));
        ASSERT(strstr(value->valuestring, "-A MT_DNSOR -p udp"));
        ASSERT_FALSE(strstr(value->valuestring, "-A MT_POLICY -j RETURN"));
        ASSERT_FALSE(strstr(value->valuestring, "-A MT_DNSOR -j RETURN"));
    }
    cJSON_Delete(out); PASS();
}
static int assert_closed_gates(void) {
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("GET", "/test/netfilter", NULL, &out)); ASSERT(out);
    for (unsigned family = 0; family < 2; family++) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(out, family ? "ipv6" : "ipv4");
        ASSERT(cJSON_IsString(value));
        ASSERT(strstr(value->valuestring, "-A MT_POLICY -j RETURN"));
        ASSERT(strstr(value->valuestring, "-A MT_DNSOR -j RETURN"));
    }
    cJSON_Delete(out); PASS();
}
TEST defaults_no_rci_and_invalid_requests_have_no_side_effects(void) {
    harness_t *h = harness_start(false); ASSERT(h);
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policy", NULL, &out));
    ASSERT(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings(out), "enabled")));
    ASSERT_STR_EQ("disabled", state(out)); cJSON_Delete(out);
    const char *bad[] = {"{}", "[]", "{\"enabled\":1,\"policy_id\":\"Policy0\",\"mode\":\"include\"}",
        "{\"enabled\":true,\"policy_id\":\"\",\"mode\":\"include\"}",
        "{\"enabled\":true,\"policy_id\":\"Policy0\",\"mode\":\"unknown\"}"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(*bad); i++) {
        ASSERT_EQ(400, do_request("PUT", "/api/v1/keenetic/policy", bad[i], NULL));
    }
    ASSERT_EQ(0, atomic_load(&fetch_calls));
    ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policies", NULL, &out));
    cJSON *list = cJSON_GetObjectItemCaseSensitive(out, "policies"); ASSERT_EQ(2, cJSON_GetArraySize(list));
    ASSERT_STR_EQ("Policy0", cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(list,0),"id")->valuestring);
    ASSERT(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(list,1),"available")));
    ASSERT_FALSE(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(list,0),"mark"));
    cJSON_Delete(out); harness_stop(h); PASS();
}
TEST both_modes_persist_and_missing_include_stays_closed(void) {
    harness_t *h = harness_start(true); ASSERT(h);
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("PUT", "/api/v1/keenetic/policy", include, &out));
    ASSERT_STR_EQ("ready", state(out)); cJSON_Delete(out);
    ASSERT_EQ(200, do_request("PUT", "/api/v1/keenetic/policy", exclude, &out));
    ASSERT_STR_EQ("exclude", cJSON_GetObjectItemCaseSensitive(settings(out),"mode")->valuestring); cJSON_Delete(out);
    atomic_store(&fetch_mode, 1);
    ASSERT_EQ(502, do_request("GET", "/api/v1/keenetic/policies", NULL, NULL));
    ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policy", NULL, &out));
    ASSERT_STR_EQ("ready", state(out));
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(settings(out),"enabled")));
    ASSERT_STR_EQ("Policy0", cJSON_GetObjectItemCaseSensitive(settings(out),"policy_id")->valuestring); cJSON_Delete(out);
    ASSERT_EQ(502, do_request("PUT", "/api/v1/keenetic/policy", include, NULL));
    atomic_store(&fetch_mode, 2);
    ASSERT_EQ(200, do_request("PUT", "/api/v1/keenetic/policy", include, &out));
    ASSERT_STR_EQ("unavailable", state(out)); cJSON_Delete(out);
    int calls = atomic_load(&fetch_calls);
    ASSERT_EQ(200, do_request("PUT", "/api/v1/keenetic/policy", disabled, &out));
    ASSERT_STR_EQ("disabled", state(out)); cJSON_Delete(out);
    ASSERT_EQ(calls, atomic_load(&fetch_calls));
    mt_config_t persisted; ASSERT_EQ(MT_OK, mt_config_init_defaults(&persisted));
    ASSERT_EQ(MT_OK, mt_config_load_file(&persisted, h->config_path));
    ASSERT_FALSE(persisted.app.keenetic_policy.enabled);
    ASSERT_STR_EQ("Policy0", persisted.app.keenetic_policy.policy_id);
    ASSERT_EQ(MT_KN_POLICY_EXCLUDE, persisted.app.keenetic_policy.mode);
    mt_config_clear(&persisted); harness_stop(h); PASS();
}
TEST missing_policy_is_persisted_in_both_modes_and_resolves_when_created(void) {
    harness_t *h = harness_start(true); ASSERT(h);
    atomic_store(&fetch_mode, 2);
    const char *ids[] = {"ImportedPolicy42", "Policy0", "mt"};
    const char *modes[] = {"include", "exclude"};
    for (size_t i = 0; i < sizeof(ids)/sizeof(*ids); i++) {
        for (size_t j = 0; j < sizeof(modes)/sizeof(*modes); j++) {
            char body[256];
            snprintf(body, sizeof(body), "{\"enabled\":true,\"policy_id\":\"%s\",\"mode\":\"%s\"}", ids[i], modes[j]);
            cJSON *out = NULL;
            ASSERT_EQ(200, do_request("PUT", "/api/v1/keenetic/policy", body, &out));
            ASSERT_STR_EQ("unavailable", state(out));
            ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(settings(out), "enabled")));
            ASSERT_STR_EQ(ids[i], cJSON_GetObjectItemCaseSensitive(settings(out), "policy_id")->valuestring);
            cJSON *runtime = cJSON_GetObjectItemCaseSensitive(out, "runtime");
            ASSERT_STR_EQ("POLICY_NOT_FOUND", cJSON_GetObjectItemCaseSensitive(runtime, "reason")->valuestring);
            cJSON_Delete(out);
            mt_config_t persisted; ASSERT_EQ(MT_OK, mt_config_init_defaults(&persisted));
            ASSERT_EQ(MT_OK, mt_config_load_file(&persisted, h->config_path));
            ASSERT(persisted.app.keenetic_policy.enabled);
            ASSERT_STR_EQ(ids[i], persisted.app.keenetic_policy.policy_id);
            ASSERT_EQ(j == 0 ? MT_KN_POLICY_INCLUDE : MT_KN_POLICY_EXCLUDE, persisted.app.keenetic_policy.mode);
            mt_config_clear(&persisted);
        }
        atomic_store(&fetch_mode, i == 0 ? 4 : i == 2 ? 3 : 0);
        ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policies", NULL, NULL));
        cJSON *out = NULL;
        ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policy", NULL, &out));
        ASSERT_STR_EQ("ready", state(out));
        ASSERT_STR_EQ(ids[i], cJSON_GetObjectItemCaseSensitive(settings(out), "policy_id")->valuestring);
        cJSON_Delete(out);
        atomic_store(&fetch_mode, 2);
    }
    harness_stop(h); PASS();
}
TEST policy_without_a_mark_can_be_saved_without_broadening_scope(void) {
    harness_t *h = harness_start(true); ASSERT(h);
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("PUT", "/api/v1/keenetic/policy",
        "{\"enabled\":true,\"policy_id\":\"Waiting\",\"mode\":\"exclude\"}", &out));
    ASSERT_STR_EQ("unavailable", state(out));
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(settings(out), "enabled")));
    ASSERT_STR_EQ("Waiting", cJSON_GetObjectItemCaseSensitive(settings(out), "policy_id")->valuestring);
    cJSON_Delete(out);
    harness_stop(h); PASS();
}
TEST disable_supersedes_late_activation_without_blocking_the_loop(void) {
    harness_t *h = harness_start(false); ASSERT(h);
    atomic_store(&hold_fetch, true);
    int fd = connect_tcp(TEST_PORT); ASSERT(fd >= 0);
    char req[1024];
    snprintf(req, sizeof(req), "PUT /api/v1/keenetic/policy HTTP/1.1\r\nHost: x\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s", strlen(include), include);
    ASSERT(send(fd, req, strlen(req), 0) > 0);
    for (unsigned i=0; i<50 && atomic_load(&fetch_calls)==0; i++) {
        struct timespec ts={0,1000000}; nanosleep(&ts,NULL);
    }
    ASSERT_EQ(1, atomic_load(&fetch_calls));
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("PUT", "/api/v1/keenetic/policy", disabled, &out));
    ASSERT_STR_EQ("disabled", state(out)); cJSON_Delete(out);
    char response[4096]; ASSERT(recv_response(fd, response, sizeof(response)) > 0);
    ASSERT_EQ(409, status_code_of(response)); close(fd);
    atomic_store(&hold_fetch, false);
    ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policies", NULL, NULL));
    ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policy", NULL, &out));
    ASSERT_STR_EQ("disabled", state(out)); cJSON_Delete(out);
    harness_stop(h); PASS();
}
TEST persistence_error_acknowledges_memory_state_and_supports_retry(void) {
    harness_t *h = harness_start(true); ASSERT(h);
    ASSERT_EQ(0, unlink(h->config_path));
    ASSERT_EQ(0, mkdir(h->config_path, 0700));
    cJSON *out = NULL;
    ASSERT_EQ(500, do_request("PUT", "/api/v1/keenetic/policy", include, &out));
    ASSERT_STR_EQ("PERSISTENCE_FAILED", cJSON_GetObjectItemCaseSensitive(out,"code")->valuestring);
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(out,"applied")));
    ASSERT_STR_EQ("Policy0", cJSON_GetObjectItemCaseSensitive(settings(out),"policy_id")->valuestring);
    cJSON_Delete(out);
    ASSERT_EQ(0, rmdir(h->config_path));
    ASSERT_EQ(200, do_request("PUT", "/api/v1/keenetic/policy", include, &out));
    ASSERT_FALSE(cJSON_GetObjectItemCaseSensitive(out,"code")); cJSON_Delete(out);
    harness_stop(h); PASS();
}
TEST failed_refresh_preserves_both_families_dns_and_exact_mark_without_writes(void) {
    const int failures[] = {1, 5, 6, 7, 1};
    for (unsigned mode = 0; mode < 2; mode++) {
        for (unsigned zero = 0; zero < 2; zero++) {
            harness_t *h = harness_start(false); ASSERT(h);
            int initial = zero ? 9 : 0;
            atomic_store(&fetch_mode, initial);
            cJSON *out = NULL;
            ASSERT_EQ(200, do_request("PUT", "/api/v1/keenetic/policy", mode ? exclude : include, &out));
            ASSERT_STR_EQ("ready", state(out)); cJSON_Delete(out);
            CHECK_CALL(assert_active_mark(zero ? "0x0/0x0fffffff" : "0xffffd00/0x0fffffff", mode != 0));
            char *before = snapshot_text(); ASSERT(before);
            for (size_t i = 0; i < sizeof(failures)/sizeof(*failures); i++) {
                atomic_store(&fetch_mode, failures[i]);
                ASSERT_EQ(502, do_request("GET", "/api/v1/keenetic/policies", NULL, NULL));
                ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policy", NULL, &out));
                ASSERT_STR_EQ("ready", state(out));
                ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(settings(out), "enabled")));
                ASSERT_STR_EQ("Policy0", cJSON_GetObjectItemCaseSensitive(settings(out), "policy_id")->valuestring);
                ASSERT_STR_EQ(mode ? "exclude" : "include", cJSON_GetObjectItemCaseSensitive(settings(out), "mode")->valuestring);
                cJSON_Delete(out);
                CHECK_CALL(assert_snapshot_unchanged(before));
            }
            /* A failed save must not transfer the old mark to another ID. */
            ASSERT_EQ(502, do_request("PUT", "/api/v1/keenetic/policy",
                "{\"enabled\":true,\"policy_id\":\"Other\",\"mode\":\"exclude\"}", NULL));
            ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policy", NULL, &out));
            ASSERT_STR_EQ("Policy0", cJSON_GetObjectItemCaseSensitive(settings(out), "policy_id")->valuestring);
            cJSON_Delete(out); CHECK_CALL(assert_snapshot_unchanged(before));
            /* Same valid inventory recovers without rewriting either family. */
            atomic_store(&fetch_mode, initial);
            ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policies", NULL, NULL));
            CHECK_CALL(assert_snapshot_unchanged(before)); free(before);
            atomic_store(&fetch_mode, 8);
            ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policies", NULL, NULL));
            CHECK_CALL(assert_active_mark("0xffffd01/0x0fffffff", mode != 0));
            before = snapshot_text(); ASSERT(before);
            ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policies", NULL, NULL));
            CHECK_CALL(assert_snapshot_unchanged(before)); free(before);
            harness_stop(h);
        }
    }
    PASS();
}
TEST absent_exclude_preserves_facts_across_errors_and_policy_transitions(void) {
    harness_t *h = harness_start_seeded(false,
        "configVersion: 0.8\napp: {keeneticPolicy: {enabled: true, policyId: mt, mode: exclude}}\n", 1);
    ASSERT(h);
    ASSERT_EQ(502, do_request("GET", "/api/v1/keenetic/policies", NULL, NULL));
    CHECK_CALL(assert_closed_gates());
    atomic_store(&fetch_mode, 2);
    ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policies", NULL, NULL));
    CHECK_CALL(assert_open_gates());
    char *before = snapshot_text(); ASSERT(before);
    const int inventories[] = {2, 1, 5, 6, 7, 2};
    for (size_t i = 0; i < sizeof(inventories)/sizeof(*inventories); i++) {
        atomic_store(&fetch_mode, inventories[i]);
        ASSERT_EQ(inventories[i] == 2 ? 200 : 502,
            do_request("GET", "/api/v1/keenetic/policies", NULL, NULL));
        cJSON *out = NULL;
        ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policy", NULL, &out));
        ASSERT_STR_EQ("unavailable", state(out));
        const cJSON *runtime = cJSON_GetObjectItemCaseSensitive(out, "runtime");
        ASSERT_STR_EQ("POLICY_NOT_FOUND", cJSON_GetObjectItemCaseSensitive(runtime, "reason")->valuestring);
        ASSERT_FALSE(cJSON_GetObjectItemCaseSensitive(runtime, "processing_all"));
        cJSON_Delete(out);
        CHECK_CALL(assert_snapshot_unchanged(before));
    }
    free(before);
    atomic_store(&fetch_mode, 3);
    ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policies", NULL, NULL));
    CHECK_CALL(assert_active_mark("0xffffd00/0x0fffffff", true));
    atomic_store(&fetch_mode, 2);
    ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policies", NULL, NULL));
    CHECK_CALL(assert_open_gates());
    ASSERT_EQ(200, do_request("PUT", "/api/v1/keenetic/policy",
        "{\"enabled\":true,\"policy_id\":\"mt\",\"mode\":\"include\"}", NULL));
    CHECK_CALL(assert_closed_gates());
    atomic_store(&fetch_mode, 0);
    ASSERT_EQ(200, do_request("PUT", "/api/v1/keenetic/policy",
        "{\"enabled\":true,\"policy_id\":\"Waiting\",\"mode\":\"exclude\"}", NULL));
    CHECK_CALL(assert_closed_gates());
    harness_stop(h); PASS();
}
TEST validated_absence_or_mark_loss_invalidates_scope_and_errors_cannot_resurrect_it(void) {
    const int missing[] = {2, 10};
    for (unsigned mode = 0; mode < 2; mode++) {
        for (size_t i = 0; i < sizeof(missing)/sizeof(*missing); i++) {
            harness_t *h = harness_start(false); ASSERT(h);
            ASSERT_EQ(200, do_request("PUT", "/api/v1/keenetic/policy", mode ? exclude : include, NULL));
            atomic_store(&fetch_mode, missing[i]);
            ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policies", NULL, NULL));
            cJSON *out = NULL;
            ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policy", NULL, &out));
            ASSERT_STR_EQ("unavailable", state(out));
            cJSON *runtime = cJSON_GetObjectItemCaseSensitive(out, "runtime");
            ASSERT_STR_EQ(missing[i] == 2 ? "POLICY_NOT_FOUND" : "POLICY_UNAVAILABLE",
                          cJSON_GetObjectItemCaseSensitive(runtime, "reason")->valuestring);
            cJSON_Delete(out);
            if (mode == 1 && missing[i] == 2) { CHECK_CALL(assert_open_gates()); }
            else { CHECK_CALL(assert_closed_gates()); }
            char *before = snapshot_text(); ASSERT(before);
            atomic_store(&fetch_mode, 1);
            ASSERT_EQ(502, do_request("GET", "/api/v1/keenetic/policies", NULL, NULL));
            ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policy", NULL, &out));
            ASSERT_STR_EQ("unavailable", state(out)); cJSON_Delete(out);
            CHECK_CALL(assert_snapshot_unchanged(before)); free(before);
            atomic_store(&fetch_mode, 8);
            ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policies", NULL, NULL));
            CHECK_CALL(assert_active_mark("0xffffd01/0x0fffffff", mode != 0));
            harness_stop(h);
        }
    }
    PASS();
}
TEST startup_and_changed_selection_never_borrow_a_previous_policy_mark(void) {
    for (unsigned mode = 0; mode < 2; mode++) {
        char yaml[256];
        snprintf(yaml, sizeof(yaml), "configVersion: 0.8\napp: {keeneticPolicy: {enabled: true, policyId: Policy0, mode: %s}}\n", mode ? "exclude" : "include");
        harness_t *h = harness_start_seeded(false, yaml, 1); ASSERT(h);
        ASSERT_EQ(502, do_request("GET", "/api/v1/keenetic/policies", NULL, NULL));
        cJSON *out = NULL;
        ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policy", NULL, &out));
        ASSERT_STR_EQ("unavailable", state(out)); cJSON_Delete(out);
        CHECK_CALL(assert_closed_gates());
        atomic_store(&fetch_mode, 0);
        ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policies", NULL, NULL));
        CHECK_CALL(assert_active_mark("0xffffd00/0x0fffffff", mode != 0));
        ASSERT_EQ(200, do_request("PUT", "/api/v1/keenetic/policy",
            "{\"enabled\":true,\"policy_id\":\"Waiting\",\"mode\":\"exclude\"}", &out));
        ASSERT_STR_EQ("unavailable", state(out)); cJSON_Delete(out);
        CHECK_CALL(assert_closed_gates());
        char *before = snapshot_text(); ASSERT(before);
        atomic_store(&fetch_mode, 1);
        ASSERT_EQ(502, do_request("GET", "/api/v1/keenetic/policies", NULL, NULL));
        ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policy", NULL, &out));
        ASSERT_STR_EQ("Waiting", cJSON_GetObjectItemCaseSensitive(settings(out), "policy_id")->valuestring);
        ASSERT_STR_EQ("unavailable", state(out)); cJSON_Delete(out);
        CHECK_CALL(assert_snapshot_unchanged(before)); free(before);
        harness_stop(h);
    }
    PASS();
}
TEST disabling_active_filter_wins_over_late_success_and_failed_refresh(void) {
    for (int result = 0; result < 2; result++) {
        harness_t *h = harness_start(false); ASSERT(h);
        ASSERT_EQ(200, do_request("PUT", "/api/v1/keenetic/policy", exclude, NULL));
        int calls = atomic_load(&fetch_calls);
        atomic_store(&fetch_mode, result); atomic_store(&hold_fetch, true);
        int fd = connect_tcp(TEST_PORT); ASSERT(fd >= 0);
        const char *request = "GET /api/v1/keenetic/policies HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        ASSERT(send(fd, request, strlen(request), 0) > 0);
        for (unsigned i = 0; i < 500 && atomic_load(&fetch_calls) == calls; i++) {
            struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL);
        }
        ASSERT(atomic_load(&fetch_calls) > calls);
        ASSERT_EQ(200, do_request("PUT", "/api/v1/keenetic/policy", disabled, NULL));
        char *before = snapshot_text(); ASSERT(before);
        atomic_store(&hold_fetch, false);
        char response[4096]; ASSERT(recv_response(fd, response, sizeof(response)) > 0);
        ASSERT_EQ(result ? 502 : 200, status_code_of(response)); close(fd);
        cJSON *out = NULL;
        ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policy", NULL, &out));
        ASSERT_STR_EQ("disabled", state(out)); cJSON_Delete(out);
        CHECK_CALL(assert_snapshot_unchanged(before));
        atomic_store(&fetch_mode, 1);
        ASSERT_EQ(502, do_request("PUT", "/api/v1/keenetic/policy", include, NULL));
        ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policy", NULL, &out));
        ASSERT_STR_EQ("disabled", state(out)); cJSON_Delete(out);
        CHECK_CALL(assert_snapshot_unchanged(before)); free(before);
        harness_stop(h);
    }
    PASS();
}
#endif
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(generic_has_no_policy_routes);
#ifdef MT_ENTWARE_KN
    RUN_TEST(defaults_no_rci_and_invalid_requests_have_no_side_effects);
    RUN_TEST(both_modes_persist_and_missing_include_stays_closed);
    RUN_TEST(missing_policy_is_persisted_in_both_modes_and_resolves_when_created);
    RUN_TEST(policy_without_a_mark_can_be_saved_without_broadening_scope);
    RUN_TEST(disable_supersedes_late_activation_without_blocking_the_loop);
    RUN_TEST(persistence_error_acknowledges_memory_state_and_supports_retry);
    RUN_TEST(failed_refresh_preserves_both_families_dns_and_exact_mark_without_writes);
    RUN_TEST(absent_exclude_preserves_facts_across_errors_and_policy_transitions);
    RUN_TEST(validated_absence_or_mark_loss_invalidates_scope_and_errors_cannot_resurrect_it);
    RUN_TEST(startup_and_changed_selection_never_borrow_a_previous_policy_mark);
    RUN_TEST(disabling_active_filter_wins_over_late_success_and_failed_refresh);
#endif
    GREATEST_MAIN_END();
}
