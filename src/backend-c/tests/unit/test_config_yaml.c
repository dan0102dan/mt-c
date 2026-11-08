/* Real HTTP handlers, raw files and rollback; no netfilter privileges needed. */
#include "greatest.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "magitrickle/app.h"
#include "magitrickle/auth.h"
#include "magitrickle/groups.h"
#include "magitrickle/system.h"

static atomic_bool fail_rename;
int __real_rename(const char *old_path, const char *new_path);
int __wrap_rename(const char *old_path, const char *new_path);
int __wrap_rename(const char *old_path, const char *new_path)
{
    if (atomic_exchange(&fail_rename, false)) { errno = EIO; return -1; }
    return __real_rename(old_path, new_path);
}

typedef struct harness {
    char directory[64], path[96];
    uint16_t port;
    mt_loop_t *loop;
    mt_httpd_t *server;
    mt_config_t cfg;
    mt_app_t *app;
    mt_system_ctx_t system;
    mt_groups_ctx_t groups;
    mt_auth_ctx_t auth;
    pthread_t thread;
    bool started;
} harness_t;

static const char *initial =
    "# keep this comment\nconfigVersion: 0.99.0\n"
    "app: {showAllInterfaces: false}\n"
    "groups: [{id: aabbccdd, name: Original, interface: blackhole, enable: false, rules: []}]\n"
    "subscriptions: []\n";
static const char *replacement =
    "# browser edit — комментарий\r\nconfigVersion: 0.99.0\r\n"
    "app: {showAllInterfaces: true}\r\n"
    "groups: [{id: aabbccdd, name: Edited, interface: blackhole, enable: false, rules: []}]\r\n"
    "subscriptions: []\r\n";

static bool write_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    if (!file) { return false; }
    bool ok = fwrite(text, 1, strlen(text), file) == strlen(text);
    return fclose(file) == 0 && ok;
}

static char *read_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) { return NULL; }
    if (fseek(file, 0, SEEK_END)) { fclose(file); return NULL; }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET)) { fclose(file); return NULL; }
    char *text = malloc((size_t)length + 1);
    if (!text) { fclose(file); return NULL; }
    if (fread(text, 1, (size_t)length, file) != (size_t)length) { free(text); fclose(file); return NULL; }
    text[length] = '\0'; fclose(file); return text;
}

static bool auth_enabled(void *userdata) { return ((harness_t *)userdata)->cfg.app.http_web.auth.enabled; }
static const char *auth_directory(void *userdata) { return ((harness_t *)userdata)->directory; }
static void *run_loop(void *userdata) { mt_loop_run(((harness_t *)userdata)->loop); return NULL; }

static void stop_harness(harness_t *h)
{
    if (!h) { return; }
    if (h->started) { mt_loop_stop(h->loop); pthread_join(h->thread, NULL); }
    mt_httpd_destroy(h->server);
    mt_loop_destroy(h->loop);
    mt_app_destroy(h->app);
    mt_config_clear(&h->cfg);
    unlink(h->path); rmdir(h->directory); free(h);
}

static uint16_t available_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { return 0; }
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t size = sizeof(address);
    uint16_t port = bind(fd, (struct sockaddr *)&address, size) == 0 &&
        getsockname(fd, (struct sockaddr *)&address, &size) == 0 ? ntohs(address.sin_port) : 0;
    close(fd); return port;
}

static harness_t *start_harness(const char *contents)
{
    harness_t *h = calloc(1, sizeof(*h));
    if (!h) { return NULL; }
    snprintf(h->directory, sizeof(h->directory), "/tmp/mt-yaml-api-XXXXXX");
    if (!mkdtemp(h->directory)) { free(h); return NULL; }
    snprintf(h->path, sizeof(h->path), "%s/config.yaml", h->directory);
    if (mt_config_init_defaults(&h->cfg) != MT_OK) { goto fail; }
    if (contents && (!write_file(h->path, contents) ||
        mt_config_load_buffer(&h->cfg, contents, strlen(contents)) != MT_OK)) { goto fail; }
    mt_app_deps_t deps = {.cfg = &h->cfg};
    h->app = mt_app_create(&deps);
    if (!h->app || mt_loop_create(&h->loop) != MT_OK ||
        mt_httpd_create(h->loop, &h->server) != MT_OK) {
        fprintf(stderr, "YAML test app/loop/server setup failed: %s\n", strerror(errno)); goto fail;
    }
    h->system = (mt_system_ctx_t){.app = h->app, .config_path = h->path, .config_version = "0.99.0"};
    h->groups = (mt_groups_ctx_t){.app = h->app};
    h->auth = (mt_auth_ctx_t){.enabled = auth_enabled, .state_dir = auth_directory, .ud = h};
    mt_system_register_yaml_routes(h->server, &h->system);
    mt_groups_register_routes(h->server, &h->groups);
    mt_httpd_set_middleware(h->server, mt_auth_middleware, &h->auth);
    h->port = available_port();
    mt_err_t listen_error = h->port ? mt_httpd_listen_tcp(h->server, "127.0.0.1", h->port) : MT_ERR_SYS;
    if (listen_error != MT_OK || pthread_create(&h->thread, NULL, run_loop, h)) {
        fprintf(stderr, "YAML test listener/thread setup failed: %s, %s\n",
                mt_err_str(listen_error), strerror(errno)); goto fail;
    }
    h->started = true;
    return h;
fail:
    stop_harness(h); return NULL;
}

static bool send_all(int fd, const char *data, size_t length)
{
    while (length) {
        ssize_t sent = send(fd, data, length, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) { continue; }
        if (sent <= 0) { return false; }
        data += (size_t)sent; length -= (size_t)sent;
    }
    return true;
}

static int request(harness_t *h, const char *method, const char *path,
                    const char *body, const char *headers, cJSON **out, bool *no_store)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { return -1; }
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
                                  .sin_port = htons(h->port)};
    if (connect(fd, (struct sockaddr *)&address, sizeof(address))) { close(fd); return -1; }
    struct timeval timeout = {.tv_sec = 5};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    size_t length = body ? strlen(body) : 0;
    char head[2048];
    int n = snprintf(head, sizeof(head),
        "%s %s HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n%s\r\n", method, path, length, headers ? headers : "");
    if (n < 0 || (size_t)n >= sizeof(head) || !send_all(fd, head, (size_t)n) ||
        (length && !send_all(fd, body, length))) { close(fd); return -1; }
    size_t capacity = 8192, used = 0;
    char *response = malloc(capacity);
    if (!response) { close(fd); return -1; }
    for (;;) {
        if (used + 1 == capacity) {
            capacity *= 2;
            char *grown = realloc(response, capacity);
            if (!grown) { free(response); close(fd); return -1; }
            response = grown;
        }
        ssize_t got = recv(fd, response + used, capacity - used - 1, 0);
        if (got < 0 && errno == EINTR) { continue; }
        if (got <= 0) { break; }
        used += (size_t)got;
    }
    close(fd);
    response[used] = '\0';
    int status = 0;
    sscanf(response, "HTTP/1.1 %d", &status);
    if (no_store) { *no_store = strstr(response, "Cache-Control: no-store\r\n") != NULL; }
    const char *split = strstr(response, "\r\n\r\n");
    if (out) { *out = split ? cJSON_Parse(split + 4) : NULL; }
    free(response);
    return status;
}

static cJSON *snapshot(harness_t *h)
{
    cJSON *out = NULL;
    if (request(h, "GET", "/api/v1/system/config/yaml", NULL, NULL, &out, NULL) != 200) {
        cJSON_Delete(out); return NULL;
    }
    return out;
}

static int put_yaml(harness_t *h, const cJSON *before, const char *yaml, bool override, cJSON **out)
{
    const cJSON *revision = cJSON_GetObjectItemCaseSensitive(before, "revision");
    if (!cJSON_IsString(revision)) { return -1; }
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "yaml", yaml);
    cJSON_AddStringToObject(body, "revision", revision->valuestring);
    if (override) { cJSON_AddBoolToObject(body, "allow_out_of_sync", true); }
    char *text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    int status = request(h, "PUT", "/api/v1/system/config/yaml", text, NULL, out, NULL);
    free(text); return status;
}

TEST editor_validation_rejects_ambiguous_and_invalid_documents(void)
{
    const char *bad[] = {
        "", "configVersion: [", "configVersion: 0.1\nconfigVersion: 0.2\n",
        "configVersion: 0.1\n---\nconfigVersion: 0.1\n", "configVersion: 2.0\n",
        "configVersion: 0.1\napp: {dnsProxy: {upsteam: {address: 1.1.1.1}}}\n",
        "configVersion: 0.1\napp: {showAllInterfaces: true, showAllInterfaces: false}\n",
        "configVersion: 0.1\ngroups: [{name: Missing}]\n",
        "configVersion: 0.1\ngroups: [{id: null}]\n",
        "configVersion: 0.1\ngroups: [{id: invalid}]\n",
        "configVersion: 0.1\ngroups: [{id: aabbccdd, priority: 0}]\n",
        "configVersion: 0.1\ngroups: [{id: aabbccdd, profile: absent}]\n",
        "configVersion: 0.1\ngroups: [{id: aabbccdd}, {id: aabbccdd}]\n",
        "configVersion: 0.1\ngroups: [{id: aabbccdd, rules: [{id: 11111111, type: typo, rule: example.com}]}]\n",
        "configVersion: 0.1\ngroups: [{id: aabbccdd, rules: [{id: 11111111, type: regex, rule: '('}]}]\n",
        "configVersion: 0.1\ngroups: [{id: aabbccdd, rules: [{id: 11111111, type: subnet, rule: 999.1.1.1/8}]}]\n",
        "configVersion: 0.1\napp: {dnsProxy: {maxConcurrent: .inf}}\n",
        "configVersion: 0.1\napp: {dnsProxy: {timeout: -9223372036854775808}}\n",
        "configVersion: 0.1\napp: {netfilter: {ipset: {additionalTTL: -9223372036854775808}}}\n",
        "configVersion: 0.1\napp: &app {link: [*app]}\n",
        "configVersion: 0.1\ngroups: [{id: aabbccdd, name: \"bad\\0name\"}]\n"
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        mt_config_t cfg = {0}; mt_config_diagnostic_t error;
        ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
        ASSERT(mt_config_load_editor_buffer(&cfg, bad[i], strlen(bad[i]), &error) != MT_OK);
        ASSERT(error.message[0]);
        mt_config_clear(&cfg);
    }
    PASS();
}

TEST get_is_read_only_and_save_preserves_verbatim_yaml(void)
{
    harness_t *h = start_harness(initial); ASSERT(h);
    struct stat before_stat, after_stat;
    ASSERT_EQ(0, stat(h->path, &before_stat));
    cJSON *before = NULL; bool no_store = false;
    ASSERT_EQ(200, request(h, "GET", "/api/v1/system/config/yaml", NULL, NULL, &before, &no_store));
    ASSERT(no_store); ASSERT(before);
    ASSERT_STR_EQ(initial, cJSON_GetObjectItemCaseSensitive(before, "yaml")->valuestring);
    ASSERT(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(before, "out_of_sync")));
    ASSERT_EQ(0, stat(h->path, &after_stat)); ASSERT_EQ(before_stat.st_ino, after_stat.st_ino);
    cJSON *out = NULL;
    ASSERT_EQ(200, put_yaml(h, before, replacement, false, &out));
    ASSERT_STR_EQ(replacement, cJSON_GetObjectItemCaseSensitive(out, "yaml")->valuestring);
    ASSERT(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(out, "restart_required")));
    ASSERT(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(out, "out_of_sync")));
    ASSERT(h->cfg.app.show_all_interfaces); ASSERT_STR_EQ("Edited", h->cfg.groups[0]->name);
    char *disk = read_file(h->path); ASSERT(disk); ASSERT_STR_EQ(replacement, disk); free(disk);
    ASSERT_EQ(0, stat(h->path, &after_stat)); ASSERT_EQ((mode_t)0600, after_stat.st_mode & 0777);
    cJSON *again = snapshot(h); ASSERT(again);
    ASSERT_STR_EQ(cJSON_GetObjectItemCaseSensitive(out, "revision")->valuestring,
                  cJSON_GetObjectItemCaseSensitive(again, "revision")->valuestring);
    mt_config_t reloaded = {0}; ASSERT_EQ(MT_OK, mt_config_init_defaults(&reloaded));
    ASSERT_EQ(MT_OK, mt_config_load_file(&reloaded, h->path)); ASSERT(reloaded.app.show_all_interfaces);
    ASSERT_STR_EQ("Edited", reloaded.groups[0]->name); mt_config_clear(&reloaded);
    cJSON_Delete(before); cJSON_Delete(out); cJSON_Delete(again); stop_harness(h); PASS();
}

TEST comments_only_keep_runtime_objects(void)
{
    harness_t *h = start_harness(initial); ASSERT(h);
    cJSON *before = snapshot(h); ASSERT(before);
    mt_group_t *group = h->cfg.groups[0]; mt_ruleset_t *ruleset = mt_app_user_group_at(h->app, 0);
    char text[1024]; snprintf(text, sizeof(text), "# another comment\n%s", initial);
    ASSERT_EQ(200, put_yaml(h, before, text, false, NULL));
    ASSERT(group == h->cfg.groups[0]); ASSERT(ruleset == mt_app_user_group_at(h->app, 0));
    char *disk = read_file(h->path); ASSERT(disk); ASSERT_STR_EQ(text, disk); free(disk);
    cJSON_Delete(before); stop_harness(h); PASS();
}

TEST invalid_yaml_preserves_file_models_and_baseline(void)
{
    harness_t *h = start_harness(initial); ASSERT(h);
    cJSON *before = snapshot(h), *out = NULL; ASSERT(before);
    mt_group_t *group = h->cfg.groups[0];
    ASSERT_EQ(400, put_yaml(h, before, "configVersion: 0.1\napp: [", false, &out));
    ASSERT_STR_EQ("INVALID_CONFIG", cJSON_GetObjectItemCaseSensitive(out, "code")->valuestring);
    ASSERT(cJSON_GetObjectItemCaseSensitive(out, "line")->valueint >= 2);
    ASSERT(cJSON_GetObjectItemCaseSensitive(out, "column")->valueint >= 1);
    ASSERT(group == h->cfg.groups[0]); ASSERT(!h->cfg.app.show_all_interfaces);
    char *disk = read_file(h->path); ASSERT(disk); ASSERT_STR_EQ(initial, disk); free(disk);
    cJSON *again = snapshot(h); ASSERT(again);
    ASSERT_STR_EQ(cJSON_GetObjectItemCaseSensitive(before, "revision")->valuestring,
                  cJSON_GetObjectItemCaseSensitive(again, "revision")->valuestring);
    ASSERT_EQ(200, put_yaml(h, before, replacement, false, NULL));
    cJSON_Delete(before); cJSON_Delete(out); cJSON_Delete(again); stop_harness(h); PASS();
}

TEST stale_disk_or_live_revision_never_overwrites_changes(void)
{
    harness_t *h = start_harness(initial); ASSERT(h);
    cJSON *before = snapshot(h), *out = NULL; ASSERT(before);
    char external[1024]; snprintf(external, sizeof(external), "# external edit\n%s", initial);
    ASSERT(write_file(h->path, external));
    ASSERT_EQ(409, put_yaml(h, before, replacement, true, &out));
    ASSERT_STR_EQ("CONFIG_CONFLICT", cJSON_GetObjectItemCaseSensitive(out, "code")->valuestring);
    char *disk = read_file(h->path); ASSERT(disk); ASSERT_STR_EQ(external, disk); free(disk);
    cJSON_Delete(before); cJSON_Delete(out); before = snapshot(h); ASSERT(before);
    ASSERT_EQ(200, request(h, "POST", "/api/v1/groups", "{\"id\":\"11223344\",\"name\":\"Unpersisted\",\"enable\":false}", NULL, NULL, NULL));
    ASSERT_EQ(409, put_yaml(h, before, replacement, true, &out));
    ASSERT_STR_EQ("CONFIG_CONFLICT", cJSON_GetObjectItemCaseSensitive(out, "code")->valuestring);
    ASSERT_EQ((size_t)2, h->cfg.n_groups);
    cJSON_Delete(before); cJSON_Delete(out); stop_harness(h); PASS();
}

TEST replacing_different_live_state_requires_explicit_current_revision_confirmation(void)
{
    harness_t *h = start_harness(initial); ASSERT(h);
    ASSERT_EQ(200, request(h, "POST", "/api/v1/groups", "{\"id\":\"11223344\",\"enable\":false}", NULL, NULL, NULL));
    cJSON *before = snapshot(h), *out = NULL; ASSERT(before);
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(before, "out_of_sync")));
    ASSERT_EQ(409, put_yaml(h, before, replacement, false, &out));
    ASSERT_STR_EQ("CONFIG_OUT_OF_SYNC", cJSON_GetObjectItemCaseSensitive(out, "code")->valuestring);
    ASSERT_EQ((size_t)2, h->cfg.n_groups);
    ASSERT_EQ(200, put_yaml(h, before, replacement, true, NULL));
    ASSERT_EQ((size_t)1, h->cfg.n_groups);
    cJSON_Delete(before); cJSON_Delete(out); stop_harness(h); PASS();
}

TEST broken_existing_file_can_be_repaired_without_cli(void)
{
    harness_t *h = start_harness(initial); ASSERT(h);
    ASSERT(write_file(h->path, "configVersion: [broken"));
    cJSON *before = snapshot(h); ASSERT(before);
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(before, "out_of_sync")));
    ASSERT_STR_EQ("configVersion: [broken", cJSON_GetObjectItemCaseSensitive(before, "yaml")->valuestring);
    ASSERT_EQ(200, put_yaml(h, before, replacement, true, NULL));
    ASSERT_STR_EQ("Edited", h->cfg.groups[0]->name);
    cJSON_Delete(before); stop_harness(h); PASS();
}

TEST absent_file_get_does_not_write_and_save_creates_it(void)
{
    harness_t *h = start_harness(NULL); ASSERT(h);
    cJSON *before = snapshot(h); ASSERT(before);
    ASSERT(access(h->path, F_OK) != 0);
    ASSERT(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(before, "out_of_sync")));
    ASSERT_EQ(200, put_yaml(h, before, cJSON_GetObjectItemCaseSensitive(before, "yaml")->valuestring, false, NULL));
    ASSERT_EQ(0, access(h->path, F_OK));
    cJSON_Delete(before); stop_harness(h); PASS();
}

TEST persistence_failure_rolls_back_exact_models_and_supports_retry(void)
{
    harness_t *h = start_harness(initial); ASSERT(h);
    cJSON *before = snapshot(h), *out = NULL; ASSERT(before);
    mt_group_t *group = h->cfg.groups[0]; mt_ruleset_t *ruleset = mt_app_user_group_at(h->app, 0);
    atomic_store(&fail_rename, true);
    ASSERT_EQ(500, put_yaml(h, before, replacement, false, &out));
    ASSERT_STR_EQ("CONFIG_SAVE_FAILED", cJSON_GetObjectItemCaseSensitive(out, "code")->valuestring);
    ASSERT(group == h->cfg.groups[0]); ASSERT(ruleset == mt_app_user_group_at(h->app, 0));
    ASSERT(!h->cfg.app.show_all_interfaces);
    char *disk = read_file(h->path); ASSERT(disk); ASSERT_STR_EQ(initial, disk); free(disk);
    DIR *directory = opendir(h->directory); ASSERT(directory);
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) { ASSERT(strstr(entry->d_name, ".tmp.") == NULL); }
    closedir(directory);
    ASSERT_EQ(200, put_yaml(h, before, replacement, false, NULL));
    ASSERT_STR_EQ("Edited", h->cfg.groups[0]->name);
    cJSON_Delete(before); cJSON_Delete(out); stop_harness(h); PASS();
}

TEST startup_settings_remain_pending_until_restart_or_revert(void)
{
    harness_t *h = start_harness(initial); ASSERT(h);
    cJSON *before = snapshot(h), *out = NULL; ASSERT(before);
    const char *changed = "configVersion: 0.99.0\napp: {dnsProxy: {upstream: {address: 9.9.9.9, port: 5353}}}\ngroups: []\nsubscriptions: []\n";
    ASSERT_EQ(200, put_yaml(h, before, changed, false, &out));
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(out, "restart_required")));
    cJSON *again = snapshot(h); ASSERT(again);
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(again, "restart_required")));
    char commented[512]; snprintf(commented, sizeof(commented), "# saved again\n%s", changed);
    ASSERT_EQ(200, put_yaml(h, again, commented, false, NULL));
    cJSON_Delete(again); again = snapshot(h); ASSERT(again);
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(again, "restart_required")));
    cJSON_Delete(out); out = NULL;
    ASSERT_EQ(200, put_yaml(h, again, initial, false, &out));
    ASSERT(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(out, "restart_required")));
    cJSON_Delete(before); cJSON_Delete(out); cJSON_Delete(again); stop_harness(h);
    h = start_harness(changed); ASSERT(h); again = snapshot(h); ASSERT(again);
    ASSERT(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(again, "restart_required")));
    cJSON_Delete(again); stop_harness(h); PASS();
}

TEST authentication_request_shape_and_cross_site_protection(void)
{
    harness_t *h = start_harness(initial); ASSERT(h);
    h->cfg.app.http_web.auth.enabled = true;
    ASSERT_EQ(401, request(h, "GET", "/api/v1/system/config/yaml", NULL, NULL, NULL, NULL));
    ASSERT_EQ(401, request(h, "PUT", "/api/v1/system/config/yaml", "{}", NULL, NULL, NULL));
    h->cfg.app.http_web.auth.enabled = false;
    const char *bad[] = {"{}", "{}{}", "{\"yaml\":\"x\",\"revision\":\"x\",\"allow_out_of_sync\":1}",
        "{\"yaml\":\"x\",\"yaml\":\"y\",\"revision\":\"z\"}",
        "{\"yaml\":\"x\\u0000y\",\"revision\":\"z\"}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        bool no_store = false;
        ASSERT_EQ(400, request(h, "PUT", "/api/v1/system/config/yaml", bad[i], NULL, NULL, &no_store));
        ASSERT(no_store);
    }
    ASSERT_EQ(400, request(h, "PUT", "/api/v1/system/config/yaml", "{}", "Sec-Fetch-Site: cross-site\r\n", NULL, NULL));
    h->system.config_path = NULL;
    ASSERT_EQ(503, request(h, "GET", "/api/v1/system/config/yaml", NULL, NULL, NULL, NULL));
    ASSERT_EQ(503, request(h, "PUT", "/api/v1/system/config/yaml", "{}", NULL, NULL, NULL));
    stop_harness(h); PASS();
}

TEST larger_config_uses_the_dedicated_http_body_budget(void)
{
    harness_t *h = start_harness(initial); ASSERT(h);
    cJSON *before = snapshot(h); ASSERT(before);
    size_t padding = (size_t)1024 * 1024 + 1024;
    char *text = malloc(padding + strlen(initial) + 3); ASSERT(text);
    text[0] = '#'; memset(text + 1, 'x', padding); text[padding + 1] = '\n';
    strcpy(text + padding + 2, initial);
    ASSERT_EQ(200, put_yaml(h, before, text, false, NULL));
    char *disk = read_file(h->path); ASSERT(disk); ASSERT_STR_EQ(text, disk);
    free(disk); free(text); cJSON_Delete(before); stop_harness(h); PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(editor_validation_rejects_ambiguous_and_invalid_documents);
    RUN_TEST(get_is_read_only_and_save_preserves_verbatim_yaml);
    RUN_TEST(comments_only_keep_runtime_objects);
    RUN_TEST(invalid_yaml_preserves_file_models_and_baseline);
    RUN_TEST(stale_disk_or_live_revision_never_overwrites_changes);
    RUN_TEST(replacing_different_live_state_requires_explicit_current_revision_confirmation);
    RUN_TEST(broken_existing_file_can_be_repaired_without_cli);
    RUN_TEST(absent_file_get_does_not_write_and_save_creates_it);
    RUN_TEST(persistence_failure_rolls_back_exact_models_and_supports_retry);
    RUN_TEST(startup_settings_remain_pending_until_restart_or_revert);
    RUN_TEST(authentication_request_shape_and_cross_site_protection);
    RUN_TEST(larger_config_uses_the_dedicated_http_body_budget);
    GREATEST_MAIN_END();
}
