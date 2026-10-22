/* HTTP-level tests for system.c -- same harness pattern as
 * test_groups.c/test_httpd.c/test_auth.c. */
#include "greatest.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cjson/cJSON.h>

#include "magitrickle/app.h"
#include "magitrickle/dns_cache.h"
#include "magitrickle/loop.h"
#include "magitrickle/system.h"

#ifdef MT_ENTWARE_KN
#include "magitrickle/keenetic_ifstate.h"
static mt_err_t alias_result = MT_OK;
#else
_Static_assert(sizeof(mt_iface_info_t) == 16, "generic interfaces must not retain alias storage");
#endif

typedef struct harness {
#ifdef MT_ENTWARE_KN
    mt_kn_ifstate_t *interface_states;
#endif
    mt_loop_t *loop;
    mt_httpd_t *tcp;
    mt_config_t cfg;
    mt_cache_t *cache;
    mt_dns_query_log_t *query_log;
    mt_app_t *app;
    mt_system_ctx_t ctx;
    char config_path[64];
    pthread_t thread;
} harness_t;

static void *loop_thread(void *ud) {
    harness_t *h = ud;
    mt_loop_run(h->loop);
    return NULL;
}

#define TEST_PORT 18082

static harness_t *harness_start(bool with_config_path) {
    harness_t *h = calloc(1, sizeof(*h));
    mt_config_init_defaults(&h->cfg);
    h->cache = mt_cache_create(0);
    mt_app_deps_t deps = {.cfg = &h->cfg, .cache = h->cache};
    h->app = mt_app_create(&deps);
    h->ctx.app = h->app;
#ifdef MT_ENTWARE_KN
    h->interface_states = mt_kn_ifstate_new(NULL, NULL);
    mt_kn_ifinfo_t item = {.id = "Loopback", .name = "lo", .alias = "Router loopback"};
    if (alias_result == MT_OK) { (void)mt_kn_ifstate_snapshot(h->interface_states, &item, 1, 0); }
    mt_app_set_interface_states(h->app, h->interface_states);
#endif
    h->query_log = mt_dns_query_log_create();
    h->ctx.query_log = h->query_log;
    if (with_config_path) {
        snprintf(h->config_path, sizeof(h->config_path), "/tmp/mt_system_test_XXXXXX");
        int fd = mkstemp(h->config_path);
        if (fd >= 0) { close(fd); }
        h->ctx.config_path = h->config_path;
        h->ctx.config_version = "0.1";
    }

    if (mt_loop_create(&h->loop) != MT_OK) { return NULL; }
    if (mt_httpd_create(h->loop, &h->tcp) != MT_OK) { return NULL; }
    mt_system_register_routes(h->tcp, &h->ctx);
    if (mt_httpd_listen_tcp(h->tcp, "127.0.0.1", TEST_PORT) != MT_OK) { return NULL; }

    pthread_create(&h->thread, NULL, loop_thread, h);
    return h;
}

static void harness_stop(harness_t *h) {
    mt_loop_stop(h->loop);
    pthread_join(h->thread, NULL);
    mt_httpd_destroy(h->tcp);
    mt_loop_destroy(h->loop);
    mt_app_destroy(h->app);
#ifdef MT_ENTWARE_KN
    mt_kn_ifstate_free(h->interface_states);
#endif
    mt_cache_destroy(h->cache);
    mt_dns_query_log_destroy(h->query_log);
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

/* ---- tests -------------------------------------------------------------------- */

TEST list_interfaces_includes_blackhole_first(void) {
    harness_t *h = harness_start(false);
    ASSERT(h != NULL);
    h->cfg.app.show_all_interfaces = true;

    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("GET", "/api/v1/system/interfaces", NULL, &out));
    cJSON *ifaces = cJSON_GetObjectItemCaseSensitive(out, "interfaces");
    ASSERT(cJSON_IsArray(ifaces));
    ASSERT(cJSON_GetArraySize(ifaces) >= 1);
    cJSON *first = cJSON_GetArrayItem(ifaces, 0);
    ASSERT_STR_EQ("blackhole", cJSON_GetObjectItemCaseSensitive(first, "id")->valuestring);
    /* blackhole has no friendly name -> "name" key omitted (omitempty) */
    ASSERT(cJSON_GetObjectItemCaseSensitive(first, "name") == NULL);
    bool found_lo = false;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, ifaces) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        ASSERT(cJSON_IsString(id));
        if (strcmp(id->valuestring, "lo") == 0) {
            found_lo = true;
#ifdef MT_ENTWARE_KN
            ASSERT(cJSON_IsString(name));
            ASSERT_STR_EQ("Router loopback", name->valuestring);
#endif
        }
#ifndef MT_ENTWARE_KN
        ASSERT(name == NULL);
#endif
    }
    ASSERT(found_lo);
    cJSON_Delete(out);
    harness_stop(h);
    PASS();
}

#ifdef MT_ENTWARE_KN
TEST interface_alias_failure_keeps_kernel_ids(void) {
    alias_result = MT_ERR_UPSTREAM;
    harness_t *h = harness_start(false);
    ASSERT(h != NULL);
    h->cfg.app.show_all_interfaces = true;
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("GET", "/api/v1/system/interfaces", NULL, &out));
    const cJSON *ifaces = cJSON_GetObjectItemCaseSensitive(out, "interfaces");
    ASSERT(cJSON_IsArray(ifaces));
    bool found_lo = false;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, ifaces) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
        ASSERT(cJSON_IsString(id));
        if (strcmp(id->valuestring, "lo") == 0) { found_lo = true; }
        ASSERT(cJSON_GetObjectItemCaseSensitive(item, "name") == NULL);
    }
    ASSERT(found_lo);
    cJSON_Delete(out);
    harness_stop(h);
    alias_result = MT_OK;
    PASS();
}
#endif

TEST dns_journal_lists_and_clears_queries(void) {
    harness_t *h = harness_start(false);
    ASSERT(h != NULL);
    ASSERT_EQ(200, do_request("PUT", "/api/v1/system/dns/capture", "{\"enabled\":true}", NULL));
    mt_dns_query_log_append(h->query_log, "192.168.1.11", "example.org", 1, false, 123456);
    mt_dns_query_log_append(h->query_log, "2001:db8::2", "example.com", 28, true, 123457);

    cJSON *result = NULL;
    ASSERT_EQ(200, do_request("GET", "/api/v1/system/dns/queries", NULL, &result));
    ASSERT(cJSON_IsObject(result));
    ASSERT_EQ((int)mt_dns_query_log_capacity(h->query_log), cJSON_GetObjectItemCaseSensitive(result, "capacity")->valueint);
    const cJSON *queries = cJSON_GetObjectItemCaseSensitive(result, "queries");
    ASSERT(cJSON_IsArray(queries));
    ASSERT_EQ(2, cJSON_GetArraySize(queries));
    const cJSON *first = cJSON_GetArrayItem(queries, 0);
    ASSERT_STR_EQ("2001:db8::2", cJSON_GetObjectItemCaseSensitive(first, "client")->valuestring);
    ASSERT_STR_EQ("example.com", cJSON_GetObjectItemCaseSensitive(first, "domain")->valuestring);
    ASSERT_EQ(28, cJSON_GetObjectItemCaseSensitive(first, "qtype")->valueint);
    ASSERT_STR_EQ("tcp", cJSON_GetObjectItemCaseSensitive(first, "protocol")->valuestring);
    cJSON_Delete(result);

    ASSERT_EQ(200, do_request("DELETE", "/api/v1/system/dns/queries", NULL, &result));
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "cleared")));
    cJSON_Delete(result);
    ASSERT_EQ(200, do_request("GET", "/api/v1/system/dns/queries", NULL, &result));
    queries = cJSON_GetObjectItemCaseSensitive(result, "queries");
    ASSERT_EQ(0, cJSON_GetArraySize(queries));
    cJSON_Delete(result);
    harness_stop(h);
    PASS();
}

static void fill_journal(mt_loop_t *loop, void *ud) {
    (void)loop;
    harness_t *h = ud;
    for (unsigned i = 0; i < 300; i++) mt_dns_query_log_append(h->query_log, "10.0.0.1", "page.example", 1, false, i);
}
static void detach_journal(mt_loop_t *loop, void *ud) {
    (void)loop;
    ((harness_t *)ud)->ctx.query_log = NULL;
}
TEST journal_get_is_bounded_and_uses_stable_id_cursors(void) {
    harness_t *h = harness_start(false); ASSERT(h);
    ASSERT_EQ(200, do_request("PUT", "/api/v1/system/dns/capture", "{\"enabled\":true}", NULL));
    ASSERT_EQ(MT_OK, mt_loop_post(h->loop, fill_journal, h));
    cJSON *out = NULL;
    /* The post is consumed before or independently of the HTTP read; wait
     * for the explicit count, never mutate the loop-owned journal directly. */
    for (unsigned tries = 0; tries < 100; tries++) {
        ASSERT_EQ(200, do_request("GET", "/api/v1/system/dns/capture", NULL, &out));
        bool ready = cJSON_GetObjectItemCaseSensitive(out, "count")->valueint == 300;
        cJSON_Delete(out); out = NULL;
        if (ready) break;
        struct timespec wait = {.tv_nsec = 1000000}; nanosleep(&wait, NULL);
    }
    ASSERT_EQ(200, do_request("GET", "/api/v1/system/dns/queries", NULL, &out));
    cJSON *rows = cJSON_GetObjectItemCaseSensitive(out, "queries");
    ASSERT_EQ(128, cJSON_GetArraySize(rows));
    ASSERT_EQ(300, cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(rows, 0), "id")->valueint);
    ASSERT_EQ(173, cJSON_GetObjectItemCaseSensitive(out, "next_before")->valueint);
    cJSON_Delete(out);
    ASSERT_EQ(200, do_request("GET", "/api/v1/system/dns/queries?before=173&limit=2", NULL, &out));
    rows = cJSON_GetObjectItemCaseSensitive(out, "queries");
    ASSERT_EQ(2, cJSON_GetArraySize(rows));
    ASSERT_EQ(172, cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(rows, 0), "id")->valueint);
    ASSERT_EQ(171, cJSON_GetObjectItemCaseSensitive(out, "next_before")->valueint);
    cJSON_Delete(out);
    const char *bad[] = {"?limit=0", "?limit=129", "?limit=-1", "?before=0", "?before=abc", "?before=18446744073709551616"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        char path[128]; snprintf(path, sizeof(path), "/api/v1/system/dns/queries%s", bad[i]);
        ASSERT_EQ(400, do_request("GET", path, NULL, NULL));
    }
    harness_stop(h); PASS();
}
TEST unavailable_history_does_not_break_other_api_routes(void) {
    harness_t *h = harness_start(false); ASSERT(h);
    ASSERT_EQ(MT_OK, mt_loop_post(h->loop, detach_journal, h));
    const char *paths[] = {"/api/v1/system/dns/capture", "/api/v1/system/dns/queries", "/api/v1/system/dns/stream"};
    for (size_t i = 0; i < 3; i++) {
        cJSON *out = NULL;
        int status = do_request("GET", paths[i], NULL, &out);
        /* The post runs through the wakeup watch before any later retry. */
        if (status == 200) { cJSON_Delete(out); out = NULL; status = do_request("GET", paths[i], NULL, &out); }
        ASSERT_EQ(503, status);
        ASSERT_STR_EQ("DNS_LOG_UNAVAILABLE", cJSON_GetObjectItemCaseSensitive(out, "code")->valuestring);
        cJSON_Delete(out);
    }
    ASSERT_EQ(200, do_request("GET", "/api/v1/system/interfaces", NULL, NULL));
    harness_stop(h); PASS();
}

TEST capture_requires_explicit_valid_put_and_releases_storage(void) {
    harness_t *h = harness_start(false); ASSERT(h != NULL);
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("GET", "/api/v1/system/dns/capture", NULL, &out));
    ASSERT(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(out, "enabled")));
    ASSERT_EQ(1048576, cJSON_GetObjectItemCaseSensitive(out, "memory_limit")->valueint);
    double initial_bytes = cJSON_GetObjectItemCaseSensitive(out, "memory_used")->valuedouble;
    cJSON_Delete(out);
    const char *invalid[] = {"{}", "true", "{\"enabled\":\"true\"}", "{\"enabled\":true}trailing", "{\"enabled\":true,\"extra\":1}"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++)
        ASSERT_EQ(400, do_request("PUT", "/api/v1/system/dns/capture", invalid[i], NULL));
    ASSERT(!mt_dns_query_log_enabled(h->query_log));
    ASSERT_EQ(200, do_request("GET", "/api/v1/system/dns/queries", NULL, &out));
    ASSERT_EQ(0, cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(out, "queries")));
    cJSON_Delete(out);
    ASSERT_EQ(200, do_request("PUT", "/api/v1/system/dns/capture", "{\"enabled\":true}", &out));
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(out, "enabled")));
    ASSERT(cJSON_GetObjectItemCaseSensitive(out, "memory_used")->valuedouble <= 1048576);
    cJSON_Delete(out);
    ASSERT_EQ(200, do_request("DELETE", "/api/v1/system/dns/queries", NULL, NULL));
    ASSERT(mt_dns_query_log_enabled(h->query_log));
    ASSERT_EQ(200, do_request("PUT", "/api/v1/system/dns/capture", "{\"enabled\":false}", &out));
    ASSERT(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(out, "enabled")));
    ASSERT_EQ(initial_bytes, cJSON_GetObjectItemCaseSensitive(out, "memory_used")->valuedouble);
    ASSERT_EQ(0, cJSON_GetObjectItemCaseSensitive(out, "count")->valueint);
    cJSON_Delete(out); harness_stop(h); PASS();
}

TEST save_config_writes_file(void) {
    harness_t *h = harness_start(true);
    ASSERT(h != NULL);
    ASSERT_EQ(200, do_request("POST", "/api/v1/system/config/save", NULL, NULL));

    mt_config_t reloaded;
    mt_config_init_defaults(&reloaded);
    ASSERT_EQ(MT_OK, mt_config_load_file(&reloaded, h->config_path));
    mt_config_clear(&reloaded);
    harness_stop(h);
    PASS();
}


TEST config_snapshot_is_read_only_and_includes_live_settings(void) {
    harness_t *h = harness_start(true);
    ASSERT(h != NULL);
    cJSON *snapshot = NULL;
    ASSERT_EQ(200, do_request("GET", "/api/v1/system/config/snapshot", NULL, &snapshot));
    ASSERT(cJSON_IsObject(snapshot));
    ASSERT(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(snapshot, "revision")));
    cJSON *config = cJSON_GetObjectItemCaseSensitive(snapshot, "config");
    ASSERT(cJSON_IsObject(config));
    ASSERT(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(config, "app")));
    ASSERT(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(config, "groups")));
    ASSERT(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(config, "subscriptions")));
    ASSERT(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(config, "configVersion")));
    FILE *written = fopen(h->config_path, "rb");
    ASSERT(written != NULL);
    ASSERT_EQ(EOF, fgetc(written)); /* An empty mkstemp file was not saved by GET. */
    fclose(written);
    cJSON_Delete(snapshot);
    harness_stop(h);
    PASS();
}

TEST config_apply_rejects_stale_preview_and_commits_valid_settings(void) {
    harness_t *h = harness_start(true);
    ASSERT(h != NULL);
    cJSON *snapshot = NULL;
    ASSERT_EQ(200, do_request("GET", "/api/v1/system/config/snapshot", NULL, &snapshot));
    const cJSON *revision = cJSON_GetObjectItemCaseSensitive(snapshot, "revision");
    ASSERT(cJSON_IsString(revision));
    cJSON *candidate = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(snapshot, "config"), 1);
    ASSERT(candidate != NULL);
    cJSON *app = cJSON_GetObjectItemCaseSensitive(candidate, "app");
    cJSON_ReplaceItemInObjectCaseSensitive(app, "logLevel", cJSON_CreateString("debug"));
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "revision", "0000000000000000");
    cJSON_AddItemToObject(request, "config", candidate);
    char *payload = cJSON_PrintUnformatted(request);
    ASSERT(payload != NULL);
    ASSERT_EQ(409, do_request("POST", "/api/v1/system/config/apply", payload, NULL));
    free(payload);
    cJSON_ReplaceItemInObjectCaseSensitive(request, "revision", cJSON_CreateString(revision->valuestring));
    payload = cJSON_PrintUnformatted(request);
    ASSERT(payload != NULL);
    ASSERT_EQ(200, do_request("POST", "/api/v1/system/config/apply", payload, NULL));
    free(payload);
    ASSERT_STR_EQ("debug", h->cfg.app.log_level);
    mt_config_t reloaded = {0};
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&reloaded));
    ASSERT_EQ(MT_OK, mt_config_load_file(&reloaded, h->config_path));
    ASSERT_STR_EQ("debug", reloaded.app.log_level);
    mt_config_clear(&reloaded);
    cJSON_Delete(request);
    cJSON_Delete(snapshot);
    harness_stop(h);
    PASS();
}

TEST config_apply_save_failure_restores_subscription_exceptions(void) {
    harness_t *h = harness_start(true);
    ASSERT(h != NULL);
    const char *fixture =
        "configVersion: 0.8.5\nsubscriptions:\n- id: aabbccdd\n"
        "  url: https://example.org/list\n"
        "  overrides: [{rule: example.org, type: domain, enable: false}]\n"
        "  rules: [{id: aaaa0001, rule: example.org, type: domain, enable: false}]\n";
    ASSERT_EQ(MT_OK, mt_config_load_buffer(&h->cfg, fixture, strlen(fixture)));
    ASSERT_EQ(200, do_request("POST", "/api/v1/system/config/save", NULL, NULL));

    cJSON *snapshot = NULL;
    ASSERT_EQ(200, do_request("GET", "/api/v1/system/config/snapshot", NULL, &snapshot));
    cJSON *config = cJSON_GetObjectItemCaseSensitive(snapshot, "config");
    cJSON *sub = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(config, "subscriptions"), 0);
    ASSERT(sub != NULL);
    cJSON_DeleteItemFromObjectCaseSensitive(sub, "rules");
    ASSERT(cJSON_ReplaceItemInObjectCaseSensitive(sub, "overrides", cJSON_CreateArray()));
    char *payload = cJSON_PrintUnformatted(snapshot);
    ASSERT(payload != NULL);

    /* The existing file cannot serve as a directory. Reload succeeds first,
     * then persistence fails with ENOTDIR and must restore the old model. */
    char failed_path[96];
    snprintf(failed_path, sizeof(failed_path), "%s/absent", h->config_path);
    h->ctx.config_path = failed_path;
    ASSERT_EQ(500, do_request("POST", "/api/v1/system/config/apply", payload, NULL));
    free(payload);
    cJSON_Delete(snapshot);
    ASSERT_EQ((size_t)1, h->cfg.n_subscriptions);
    mt_subscription_t *restored = h->cfg.subscriptions[0];
    ASSERT_EQ((size_t)1, restored->n_overrides);
    const mt_sub_override_t *override = mt_subscription_override_find(restored, "example.org");
    ASSERT(override != NULL);
    ASSERT_STR_EQ("domain", override->type);
    ASSERT_EQ(0, override->enable);
    ASSERT(!restored->rules[0]->enable);
    ASSERT_STR_EQ("domain", restored->rules[0]->type);

    mt_config_t on_disk = {0};
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&on_disk));
    ASSERT_EQ(MT_OK, mt_config_load_file(&on_disk, h->config_path));
    ASSERT_EQ((size_t)1, on_disk.subscriptions[0]->n_overrides);
    ASSERT(!on_disk.subscriptions[0]->rules[0]->enable);
    ASSERT_STR_EQ("domain", on_disk.subscriptions[0]->rules[0]->type);
    mt_config_clear(&on_disk);
    harness_stop(h);
    PASS();
}

TEST config_apply_checks_field_names_not_a_magic_field_count(void) {
    harness_t *h = harness_start(true); ASSERT(h);
    cJSON *snapshot = NULL;
    ASSERT_EQ(200, do_request("GET", "/api/v1/system/config/snapshot", NULL, &snapshot));
    cJSON *app = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(snapshot, "config"), "app");
    cJSON_DeleteItemFromObjectCaseSensitive(app, "logLevel");
    ASSERT(cJSON_AddBoolToObject(app, "unsupportedSetting", true));
    char *text = cJSON_PrintUnformatted(snapshot); ASSERT(text);
    ASSERT_EQ(409, do_request("POST", "/api/v1/system/config/apply", text, NULL));
    free(text); cJSON_Delete(snapshot);
    ASSERT_EQ(400, do_request("POST", "/api/v1/system/config/apply", "{}{}", NULL));
    ASSERT_EQ(400, do_request("POST", "/api/v1/system/config/apply", "{\"revision\":\"a\\u0000b\"}", NULL));
    harness_stop(h); PASS();
}

TEST save_config_noop_without_path(void) {
    harness_t *h = harness_start(false);
    ASSERT(h != NULL);
    ASSERT_EQ(200, do_request("POST", "/api/v1/system/config/save", NULL, NULL));
    harness_stop(h);
    PASS();
}

TEST netfilterd_hook_ok_and_bad_json(void) {
    harness_t *h = harness_start(false);
    ASSERT(h != NULL);
    ASSERT_EQ(200, do_request("POST", "/api/v1/system/hooks/netfilterd",
                             "{\"type\":\"iptables\",\"table\":\"nat\"}", NULL));
    ASSERT_EQ(400, do_request("POST", "/api/v1/system/hooks/netfilterd", "not json", NULL));
    harness_stop(h);
    PASS();
}

/* Mirrors Go's constant.IgnoredInterfaces (empty by default, a fixed
 * Keenetic virtual-interface list under the entware_kn build tag --
 * src/backend/constant/iface-ignore_entware_kn.go). Go's own dedicated
 * entware_kn test (keenetic_router_specific_test.go) only covers the RCI
 * alias lookup, not this list, so there is no Go test to differentially
 * match here either -- this asserts the C side's behavior is correct for
 * whichever mode it was built in. */
TEST ignored_interfaces_list(void) {
#ifdef MT_ENTWARE_KN
    ASSERT(mt_iface_is_ignored_for_test("ra0"));
    ASSERT(mt_iface_is_ignored_for_test("ra15"));
    ASSERT(mt_iface_is_ignored_for_test("ezcfg0"));
    ASSERT(!mt_iface_is_ignored_for_test("br0"));
    ASSERT(!mt_iface_is_ignored_for_test("ra16"));
#else
    ASSERT(!mt_iface_is_ignored_for_test("ra0"));
    ASSERT(!mt_iface_is_ignored_for_test("ezcfg0"));
    ASSERT(!mt_iface_is_ignored_for_test("br0"));
#endif
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(list_interfaces_includes_blackhole_first);
#ifdef MT_ENTWARE_KN
    RUN_TEST(interface_alias_failure_keeps_kernel_ids);
#endif
    RUN_TEST(dns_journal_lists_and_clears_queries);
    RUN_TEST(journal_get_is_bounded_and_uses_stable_id_cursors);
    RUN_TEST(unavailable_history_does_not_break_other_api_routes);
    RUN_TEST(capture_requires_explicit_valid_put_and_releases_storage);
    RUN_TEST(save_config_writes_file);
    RUN_TEST(config_snapshot_is_read_only_and_includes_live_settings);
    RUN_TEST(config_apply_rejects_stale_preview_and_commits_valid_settings);
    RUN_TEST(config_apply_save_failure_restores_subscription_exceptions);
    RUN_TEST(config_apply_checks_field_names_not_a_magic_field_count);
    RUN_TEST(save_config_noop_without_path);
    RUN_TEST(netfilterd_hook_ok_and_bad_json);
    RUN_TEST(ignored_interfaces_list);
    GREATEST_MAIN_END();
}
