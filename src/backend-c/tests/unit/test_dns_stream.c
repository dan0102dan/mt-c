#include "greatest.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include "magitrickle/auth.h"
#include "magitrickle/jwt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include "magitrickle/http_stream.h"
#include "magitrickle/dns_query_log.h"
#include "magitrickle/system.h"

/* Count real kernel timer/interest operations, not wall-clock thresholds. */
int __real_timerfd_create(int clockid, int flags);
int __real_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
int __wrap_timerfd_create(int clockid, int flags);
int __wrap_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
static atomic_uint timers_created, interest_changes;
int __wrap_timerfd_create(int clockid, int flags) {
    atomic_fetch_add(&timers_created, 1);
    return __real_timerfd_create(clockid, flags);
}
int __wrap_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event) {
    if (op == EPOLL_CTL_MOD) atomic_fetch_add(&interest_changes, 1);
    return __real_epoll_ctl(epfd, op, fd, event);
}
typedef struct harness {
    mt_loop_t *loop;
    mt_httpd_t *http;
    mt_dns_query_log_t *log;
    mt_system_ctx_t system;
    pthread_t thread;
    char path[108];
    atomic_bool auth_enabled, revoked;
    atomic_uint alias;
    bool real_auth;
    char shadow[160], token[MT_JWT_MAX_TOKEN], state[108];
    mt_dns_clients_t *clients;
} harness_t;
static bool authorize(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    harness_t *h = ud;
    if (!atomic_load(&h->auth_enabled)) return true;
    const char *value = mt_http_req_header(req, "Authorization");
    if (!atomic_load(&h->revoked) && value &&
        (h->real_auth ? !strncmp(value, "Bearer ", 7) &&
         mt_auth_verify_token_from(h->shadow, h->shadow, h->state, value + 7) == MT_OK :
         strcmp(value, "Bearer test") == 0)) return true;
    mt_http_res_write_error(res, 401, "unauthorized");
    return false;
}
static void append_handler(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req; harness_t *h = ud;
    mt_dns_query_log_append(h->log, "192.168.1.50", "live.example", 1, false, 123);
    mt_http_res_write_json(res, 200, cJSON_CreateObject());
}
static void flood_handler(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req; harness_t *h = ud;
    for (size_t i = 0; i <= mt_dns_query_log_capacity(h->log); i++)
        mt_dns_query_log_append(h->log, "10.0.0.9", "ring.example", 1, false, (int64_t)i);
    mt_http_res_write_json(res, 200, cJSON_CreateObject());
}
static void burst_handler(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req;
    harness_t *h = ud;
    atomic_store(&timers_created, 0); atomic_store(&interest_changes, 0);
    for (unsigned i = 0; i < 128; i++)
        mt_dns_query_log_append(h->log, "10.0.0.1", "burst.example", 1, false, i);
    mt_http_res_write_json(res, 200, cJSON_CreateObject());
}
static void auth_change(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    harness_t *h = ud;
    if (strcmp(mt_http_req_path(req), "/test/auth-enable") == 0) atomic_store(&h->auth_enabled, true);
    else atomic_store(&h->revoked, true);
    mt_http_res_write_json(res, 200, cJSON_CreateObject());
}
static mt_err_t fetch_clients(mt_dns_client_t **out, size_t *count, const atomic_bool *stop, void *ud) {
    (void)stop;
    harness_t *h = ud;
    *out = calloc(1, sizeof(**out));
    if (!*out) return MT_ERR_NOMEM;
    strcpy((*out)->ip, "192.168.1.50");
    strcpy((*out)->name, atomic_load(&h->alias) ? "Renamed phone" : "Phone");
    *count = 1;
    return MT_OK;
}
static void rename_client(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req;
    harness_t *h = ud;
    atomic_store(&h->alias, 1);
    mt_dns_clients_refresh(h->clients);
    mt_http_res_write_json(res, 200, cJSON_CreateObject());
}
static void *worker(void *ud) { mt_loop_run(((harness_t *)ud)->loop); return NULL; }
static bool start_options(harness_t *h, size_t capacity, bool prefill, bool real_auth, bool aliases) {
    memset(h, 0, sizeof(*h));
    atomic_init(&h->auth_enabled, true); atomic_init(&h->revoked, false); atomic_init(&h->alias, 0);
    h->real_auth = real_auth;
    if (real_auth) {
        strcpy(h->state, "/tmp/mt-stream-auth-XXXXXX");
        if (!mkdtemp(h->state)) return false;
        snprintf(h->shadow, sizeof(h->shadow), "%s/shadow", h->state);
        FILE *f = fopen(h->shadow, "w");
        if (!f) return false;
        fputs("admin:$6$abcdefghijklmnop$EC.xeLW9zNWcX0r23FSpQaV7PG.Ibd4QnLe3w6UC47i3/"
              "vkPQouEDwvUpGtqFiad5mzQG96cD/LywQiXv9WfH/:19000:0:99999:7:::\n", f);
        fclose(f);
        if (mt_auth_authenticate_from(h->shadow, h->shadow, h->state, "admin", "hunter2",
                                     h->token, sizeof(h->token)) != MT_OK) return false;
    }
    snprintf(h->path, sizeof(h->path), "/tmp/mt-dns-stream-XXXXXX");
    int temp = mkstemp(h->path);
    if (temp < 0) return false;
    close(temp); unlink(h->path);
    if (mt_loop_create(&h->loop) != MT_OK || mt_httpd_create(h->loop, &h->http) != MT_OK) return false;
    h->log = mt_dns_query_log_create_budget(prefill || capacity > 4096 ? MT_DNS_QUERY_LOG_MEMORY_LIMIT : 65536);
    if (!h->log || !mt_dns_query_log_set_enabled(h->log, true)) return false;
    if (prefill) {
        for (size_t i = 0; i < capacity; i++) {
            uint8_t name[255]; size_t p = 0;
            for (size_t j = 0; j < 4; j++) {
                size_t len = j == 3 ? 61 : 63;
                name[p++] = (uint8_t)len; memset(name + p, 200, len); p += len;
            }
            name[p++] = 0; name[1] = (uint8_t)(i >> 8); name[2] = (uint8_t)i;
            if (!mt_dns_query_log_append_wire(h->log, NULL, 0, name, p, 28, true, (int64_t)i)) return false;
        }
    }
    h->system.query_log = h->log;
    if (aliases && mt_dns_clients_create(h->loop, fetch_clients, h, &h->clients) != MT_OK) return false;
    h->system.dns_clients = h->clients;
    mt_httpd_set_middleware(h->http, authorize, h);
    mt_system_register_routes(h->http, &h->system);
    if (mt_httpd_route(h->http, "POST", "/test/append", append_handler, h) != MT_OK ||
        mt_httpd_route(h->http, "POST", "/test/flood", flood_handler, h) != MT_OK ||
        mt_httpd_route(h->http, "POST", "/test/revoke", auth_change, h) != MT_OK ||
        mt_httpd_route(h->http, "POST", "/test/auth-enable", auth_change, h) != MT_OK ||
        mt_httpd_route(h->http, "POST", "/test/rename", rename_client, h) != MT_OK ||
        mt_httpd_route(h->http, "POST", "/test/burst", burst_handler, h) != MT_OK ||
        mt_httpd_listen_unix(h->http, h->path) != MT_OK) return false;
    return pthread_create(&h->thread, NULL, worker, h) == 0;
}
static bool start(harness_t *h, size_t capacity, bool prefill) {
    return start_options(h, capacity, prefill, false, false);
}
static void finish(harness_t *h) {
    mt_loop_stop(h->loop); pthread_join(h->thread, NULL);
    mt_httpd_destroy(h->http); mt_dns_clients_destroy(h->clients); mt_dns_query_log_destroy(h->log);
    mt_loop_destroy(h->loop); unlink(h->path);
    if (h->real_auth) {
        unlink(h->shadow);
        char secret[160]; snprintf(secret, sizeof(secret), "%s/auth_secret", h->state);
        unlink(secret); rmdir(h->state);
    }
}
static bool read_exact(int fd, char *out, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, out + got, len - got, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        got += (size_t)n;
    }
    return true;
}
static bool read_line(int fd, char *out, size_t cap) {
    for (size_t i = 0; i + 1 < cap; i++) {
        if (!read_exact(fd, out + i, 1)) return false;
        if (out[i] == '\n') { out[i + 1] = '\0'; return true; }
    }
    return false;
}
static int request_body(harness_t *h, const char *method, const char *path, bool auth, const char *body) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval timeout = {.tv_sec = 5};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", h->path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { close(fd); return -1; }
    char buffer[4096], authorization[MT_JWT_MAX_TOKEN + 32] = "";
    if (auth) snprintf(authorization, sizeof(authorization), "Authorization: Bearer %s\r\n", h->real_auth ? h->token : "test");
    int n = snprintf(buffer, sizeof(buffer), "%s %s HTTP/1.1\r\nHost: test\r\n%sContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
        method, path, authorization, strlen(body), body);
    if (n <= 0 || (size_t)n >= sizeof(buffer) || send(fd, buffer, (size_t)n, MSG_NOSIGNAL) != n) { close(fd); return -1; }
    return fd;
}
static int request(harness_t *h, const char *method, const char *path, bool auth) {
    return request_body(h, method, path, auth, "");
}
static bool headers(int fd, int status) {
    char line[512]; int actual = 0;
    if (!read_line(fd, line, sizeof(line)) || sscanf(line, "HTTP/1.1 %d", &actual) != 1 || actual != status) return false;
    while (read_line(fd, line, sizeof(line))) if (strcmp(line, "\r\n") == 0) return true;
    return false;
}
static bool control(harness_t *h, const char *method, const char *path) {
    int fd = request(h, method, path, true);
    bool ok = fd >= 0 && headers(fd, 200);
    if (fd >= 0) close(fd);
    return ok;
}
static bool capture(harness_t *h, bool enabled) {
    int fd = request_body(h, "PUT", "/api/v1/system/dns/capture", true,
        enabled ? "{\"enabled\":true}" : "{\"enabled\":false}");
    bool ok = fd >= 0 && headers(fd, 200);
    if (fd >= 0) close(fd);
    return ok;
}
/* Decode HTTP chunks exactly, never discard over-read SSE events. */
static cJSON *next_event(int fd, char kind[32], size_t *bytes) {
    for (;;) {
        char line[64], suffix[2];
        if (!read_line(fd, line, sizeof(line))) return NULL;
        char *end; unsigned long length = strtoul(line, &end, 16);
        if (end == line || length == 0 || length > MT_HTTP_STREAM_MAX_FRAME) return NULL;
        char *text = malloc((size_t)length + 1);
        if (!text) return NULL;
        bool ok = read_exact(fd, text, (size_t)length) && read_exact(fd, suffix, 2);
        text[length] = '\0';
        if (!ok || memcmp(suffix, "\r\n", 2) != 0) { free(text); return NULL; }
        *bytes += (size_t)length;
        if (text[0] == ':') { free(text); continue; }
        const char *data = strstr(text, "\ndata: ");
        cJSON *json = data && sscanf(text, "event: %31s", kind) == 1 ? cJSON_Parse(data + 7) : NULL;
        free(text); return json;
    }
}
static double number(cJSON *json, const char *key) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(json, key);
    return cJSON_IsNumber(value) ? value->valuedouble : -1;
}
static bool event_is(int fd, const char *expected) {
    char kind[32]; size_t bytes = 0;
    cJSON *data = next_event(fd, kind, &bytes);
    bool ok = data && strcmp(kind, expected) == 0;
    cJSON_Delete(data); return ok;
}
static bool capture_event(int fd, bool enabled) {
    char kind[32]; size_t bytes = 0;
    cJSON *data = next_event(fd, kind, &bytes);
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(data, "enabled");
    bool ok = data && strcmp(kind, "capture") == 0 && cJSON_IsBool(value) && cJSON_IsTrue(value) == enabled;
    cJSON_Delete(data); return ok;
}
/* A control action after begin deterministically interrupts a large snapshot:
 * socket buffering cannot swallow its multi-megabyte remainder. */
static bool snapshot(int fd, size_t expected_count, size_t capacity, size_t *bytes,
    harness_t *h, const char *action) {
    size_t received = 0, count = 0; uint64_t through = 0;
    bool begun = false, controlled = false;
    for (size_t frames = 0; frames < 10000; frames++) {
        char kind[32]; cJSON *data = next_event(fd, kind, bytes);
        if (!data) return false;
        bool ok = true, done = false;
        if (strcmp(kind, "capture") == 0) {
            ok = cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(data, "enabled"));
        } else if (strcmp(kind, "clients_begin") == 0) {
            ok = number(data, "count") >= 0 && number(data, "count") <= MT_DNS_CLIENT_LIMIT;
        } else if (strcmp(kind, "clients_chunk") == 0) {
            ok = cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(data, "clients"));
        } else if (strcmp(kind, "clients_end") == 0) {
            ok = cJSON_IsObject(data);
        } else if (strcmp(kind, "snapshot_begin") == 0) {
            count = (size_t)number(data, "count"); through = (uint64_t)number(data, "through");
            received = 0; begun = true;
            ok = number(data, "capacity") == (double)capacity && count <= capacity;
            if (ok && action && !controlled) {
                controlled = true;
                ok = control(h, strcmp(action, "/test/flood") == 0 ? "POST" : "DELETE", action);
            }
        } else if (begun && strcmp(kind, "snapshot_chunk") == 0) {
            cJSON *rows = cJSON_GetObjectItemCaseSensitive(data, "queries");
            int n = cJSON_GetArraySize(rows);
            ok = cJSON_IsArray(rows) && n > 0 && n <= 32 &&
                number(data, "offset") == (double)received && received + (size_t)n <= count;
            for (int i = 0; ok && i < n; i++) {
                ok = number(cJSON_GetArrayItem(rows, i), "id") == (double)(through - count + received + 1);
                received++;
            }
        } else if (begun && strcmp(kind, "snapshot_end") == 0) {
            ok = received == count && (expected_count == SIZE_MAX || count == expected_count) && number(data, "through") == (double)through;
            done = true;
        } else ok = false;
        cJSON_Delete(data);
        if (!ok) return false;
        if (done) return true;
    }
    return false;
}
TEST snapshot_live_clear_and_authorization(void) {
    harness_t h; ASSERT(start(&h, 1000, false));
    int unauthorized = request(&h, "GET", "/api/v1/system/dns/stream", false);
    ASSERT(unauthorized >= 0); ASSERT(headers(unauthorized, 401)); close(unauthorized);
    int first = request(&h, "GET", "/api/v1/system/dns/stream", true);
    ASSERT(first >= 0); ASSERT(headers(first, 200)); size_t bytes = 0;
    ASSERT(snapshot(first, 0, mt_dns_query_log_capacity(h.log), &bytes, NULL, NULL));
    ASSERT(control(&h, "POST", "/test/append")); ASSERT(event_is(first, "query"));
    int second = request(&h, "GET", "/api/v1/system/dns/stream", true);
    ASSERT(second >= 0); ASSERT(headers(second, 200));
    ASSERT(snapshot(second, 1, mt_dns_query_log_capacity(h.log), &bytes, NULL, NULL));
    ASSERT(control(&h, "DELETE", "/api/v1/system/dns/queries"));
    ASSERT(event_is(first, "clear")); ASSERT(event_is(second, "clear"));
    ASSERT(control(&h, "POST", "/test/append"));
    ASSERT(event_is(first, "query")); ASSERT(event_is(second, "query"));
    close(first); close(second); finish(&h); PASS();
}
TEST large_snapshot_is_bounded_and_restarts_on_clear_or_eviction(void) {
    for (int scenario = 0; scenario < 3; scenario++) {
        harness_t h; ASSERT(start(&h, 3000, true));
        int fd = request(&h, "GET", "/api/v1/system/dns/stream", true);
        ASSERT(fd >= 0); ASSERT(headers(fd, 200)); size_t bytes = 0;
        const char *action = scenario == 1 ? "/api/v1/system/dns/queries" : scenario == 2 ? "/test/flood" : NULL;
        ASSERT(snapshot(fd, scenario == 1 ? 0 : scenario == 2 ? SIZE_MAX : 3000, mt_dns_query_log_capacity(h.log), &bytes, &h, action));
        if (scenario == 0) ASSERT(bytes > 2 * 1024 * 1024);
        ASSERT(control(&h, "POST", "/test/append")); ASSERT(event_is(fd, "query"));
        close(fd); finish(&h);
    }
    PASS();
}
TEST capture_changes_are_authorized_and_broadcast_to_all_viewers(void) {
    harness_t h; ASSERT(start(&h, 1000, false));
    ASSERT(capture(&h, false));
    int denied = request_body(&h, "PUT", "/api/v1/system/dns/capture", false, "{\"enabled\":true}");
    ASSERT(denied >= 0); ASSERT(headers(denied, 401)); close(denied);
    int viewers[2]; size_t bytes = 0;
    for (size_t i = 0; i < 2; i++) {
        viewers[i] = request(&h, "GET", "/api/v1/system/dns/stream", true);
        ASSERT(viewers[i] >= 0); ASSERT(headers(viewers[i], 200));
        ASSERT(capture_event(viewers[i], false));
        ASSERT(snapshot(viewers[i], 0, mt_dns_query_log_capacity(h.log), &bytes, NULL, NULL));
    }
    ASSERT(control(&h, "POST", "/test/append")); /* No stored row while off. */
    ASSERT(capture(&h, true));
    for (size_t i = 0; i < 2; i++) {
        ASSERT(capture_event(viewers[i], true));
        ASSERT(snapshot(viewers[i], 0, mt_dns_query_log_capacity(h.log), &bytes, NULL, NULL));
    }
    ASSERT(control(&h, "POST", "/test/append"));
    for (size_t i = 0; i < 2; i++) ASSERT(event_is(viewers[i], "query"));
    ASSERT(capture(&h, false));
    for (size_t i = 0; i < 2; i++) {
        ASSERT(capture_event(viewers[i], false));
        ASSERT(snapshot(viewers[i], 0, mt_dns_query_log_capacity(h.log), &bytes, NULL, NULL));
        close(viewers[i]);
    }
    finish(&h); PASS();
}
TEST disconnect_and_subscriber_limit(void) {
    harness_t h; ASSERT(start(&h, 1000, false));
    int sockets[MT_HTTP_STREAM_MAX_CLIENTS]; size_t bytes = 0;
    for (size_t i = 0; i < MT_HTTP_STREAM_MAX_CLIENTS; i++) {
        sockets[i] = request(&h, "GET", "/api/v1/system/dns/stream", true);
        ASSERT(sockets[i] >= 0); ASSERT(headers(sockets[i], 200));
        ASSERT(snapshot(sockets[i], 0, mt_dns_query_log_capacity(h.log), &bytes, NULL, NULL));
    }
    int excess = request(&h, "GET", "/api/v1/system/dns/stream", true);
    ASSERT(excess >= 0); ASSERT(headers(excess, 503)); close(excess);
    ASSERT(control(&h, "POST", "/test/append"));
    finish(&h); /* Open subscriptions must release before the journal dies. */
    for (size_t i = 0; i < MT_HTTP_STREAM_MAX_CLIENTS; i++) close(sockets[i]);
    PASS();
}
/* Wait for actual EOF, not a timeout; revocation is enforced even on an idle
 * stream and not just when the next DNS event happens. */
static bool peer_closed(int fd) {
    char buf[4096];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n == 0) return true;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return false;
    }
}
TEST authorization_changes_close_existing_streams(void) {
    for (int mode = 0; mode < 3; mode++) {
        harness_t h; ASSERT(start(&h, 1000, false));
        if (mode == 1) atomic_store(&h.auth_enabled, false);
        int fd = request(&h, "GET", "/api/v1/system/dns/stream", mode != 1);
        ASSERT(fd >= 0); ASSERT(headers(fd, 200)); size_t bytes = 0;
        ASSERT(snapshot(fd, 0, mt_dns_query_log_capacity(h.log), &bytes, NULL, NULL));
        if (mode == 0) ASSERT(control(&h, "POST", "/test/revoke"));
        else if (mode == 1) ASSERT(control(&h, "POST", "/test/auth-enable"));
        else atomic_store(&h.revoked, true); /* external change, periodic recheck */
        ASSERT(peer_closed(fd));
        close(fd); finish(&h);
    }
    PASS();
}
TEST password_change_revokes_a_real_signed_token_on_an_idle_stream(void) {
    harness_t h; ASSERT(start_options(&h, 1000, false, true, false));
    int fd = request(&h, "GET", "/api/v1/system/dns/stream", true);
    ASSERT(fd >= 0); ASSERT(headers(fd, 200)); size_t bytes = 0;
    ASSERT(snapshot(fd, 0, mt_dns_query_log_capacity(h.log), &bytes, NULL, NULL));
    /* Atomic rename models the shadow-file update without a partial-read race. */
    char replacement[180]; snprintf(replacement, sizeof(replacement), "%s.new", h.shadow);
    FILE *file = fopen(replacement, "w"); ASSERT(file != NULL);
    fputs("admin:$1$abcdefgh$vhxKZ/s1ygZHyCEDPyqtQ/:19000:0:99999:7:::\n", file);
    ASSERT_EQ(0, fclose(file)); ASSERT_EQ(0, rename(replacement, h.shadow));
    ASSERT(peer_closed(fd)); close(fd);
    int denied = request(&h, "GET", "/api/v1/system/dns/stream", true);
    ASSERT(denied >= 0); ASSERT(headers(denied, 401)); close(denied);
    finish(&h); PASS();
}
static bool client_inventory(int fd, const char *expected) {
    size_t bytes = 0; bool found = false;
    for (unsigned i = 0; i < 10; i++) {
        char kind[32]; cJSON *data = next_event(fd, kind, &bytes);
        if (!data) return false;
        if (!strcmp(kind, "clients_chunk")) {
            cJSON *rows = cJSON_GetObjectItemCaseSensitive(data, "clients");
            const cJSON *row;
            cJSON_ArrayForEach(row, rows) {
                const cJSON *name = cJSON_GetObjectItemCaseSensitive(row, "name");
                if (cJSON_IsString(name) && !strcmp(name->valuestring, expected)) found = true;
            }
        } else if (!strcmp(kind, "clients_end")) {
            cJSON_Delete(data);
            if (found) return true;
            continue;
        } else if (strcmp(kind, "clients_begin")) { cJSON_Delete(data); return false; }
        cJSON_Delete(data);
    }
    return false;
}
TEST alias_rename_does_not_replay_dns_history(void) {
    harness_t h; ASSERT(start_options(&h, 1000, false, false, true));
    ASSERT(control(&h, "POST", "/test/append"));
    int fd = request(&h, "GET", "/api/v1/system/dns/stream", true);
    ASSERT(fd >= 0); ASSERT(headers(fd, 200)); size_t bytes = 0;
    ASSERT(snapshot(fd, 1, mt_dns_query_log_capacity(h.log), &bytes, NULL, NULL));
    /* The first fetch may finish before or after the initial DNS snapshot.
     * Wait on the provider, then consume any metadata still pending. */
    struct timespec wait = {.tv_nsec = 100000000}; nanosleep(&wait, NULL);
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    if (poll(&pfd, 1, 0) > 0) ASSERT(client_inventory(fd, "Phone"));
    ASSERT(control(&h, "POST", "/test/rename"));
    ASSERT(client_inventory(fd, "Renamed phone")); /* rejects snapshot_* events */
    ASSERT(control(&h, "POST", "/test/append")); ASSERT(event_is(fd, "query"));
    close(fd); finish(&h); PASS();
}
TEST streaming_reuses_timers_and_coalesces_wakeup_syscalls(void) {
    harness_t h; ASSERT(start(&h, 1000, false));
    int fd = request(&h, "GET", "/api/v1/system/dns/stream", true);
    ASSERT(fd >= 0); ASSERT(headers(fd, 200)); size_t bytes = 0;
    ASSERT(snapshot(fd, 0, mt_dns_query_log_capacity(h.log), &bytes, NULL, NULL));
    ASSERT(control(&h, "POST", "/test/burst"));
    for (unsigned i = 0; i < 128; i++) ASSERT(event_is(fd, "query"));
    /* A finite control request can add/reset its own idle timer, but 128
     * streamed writes must not create 128 timers or 128 EPOLLOUT rearms. */
    ASSERT(atomic_load(&timers_created) < 16);
    ASSERT(atomic_load(&interest_changes) < 16);
    close(fd); finish(&h); PASS();
}
TEST byte_budget_history_over_60000_survives_streaming(void) {
    harness_t h; ASSERT(start(&h, 100000, false));
    ASSERT(control(&h, "POST", "/test/flood"));
    /* The handler finished on the loop before its response. No concurrent
     * capture source exists in this harness between the control requests. */
    size_t count = mt_dns_query_log_size(h.log);
    ASSERT(count > 95000);
    int fd = request(&h, "GET", "/api/v1/system/dns/stream", true);
    ASSERT(fd >= 0); ASSERT(headers(fd, 200)); size_t bytes = 0;
    ASSERT(snapshot(fd, count, mt_dns_query_log_capacity(h.log), &bytes, NULL, NULL));
    ASSERT(bytes > 2 * 1024 * 1024);
    ASSERT(control(&h, "POST", "/test/append")); ASSERT(event_is(fd, "query"));
    close(fd); finish(&h); PASS();
}
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(byte_budget_history_over_60000_survives_streaming);
    RUN_TEST(snapshot_live_clear_and_authorization);
    RUN_TEST(large_snapshot_is_bounded_and_restarts_on_clear_or_eviction);
    RUN_TEST(capture_changes_are_authorized_and_broadcast_to_all_viewers);
    RUN_TEST(disconnect_and_subscriber_limit);
    RUN_TEST(authorization_changes_close_existing_streams);
    RUN_TEST(password_change_revokes_a_real_signed_token_on_an_idle_stream);
    RUN_TEST(alias_rename_does_not_replay_dns_history);
    RUN_TEST(streaming_reuses_timers_and_coalesces_wakeup_syscalls);
    GREATEST_MAIN_END();
}
