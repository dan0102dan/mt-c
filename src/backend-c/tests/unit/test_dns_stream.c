#include "greatest.h"
#include <errno.h>
#include <pthread.h>
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

typedef struct harness {
    mt_loop_t *loop;
    mt_httpd_t *http;
    mt_dns_query_log_t *log;
    mt_system_ctx_t system;
    pthread_t thread;
    char path[108];
} harness_t;
static bool authorize(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)ud;
    const char *value = mt_http_req_header(req, "Authorization");
    if (value && strcmp(value, "Bearer test") == 0) return true;
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
static void *worker(void *ud) { mt_loop_run(((harness_t *)ud)->loop); return NULL; }
static bool start(harness_t *h, size_t capacity, bool prefill) {
    memset(h, 0, sizeof(*h));
    snprintf(h->path, sizeof(h->path), "/tmp/mt-dns-stream-XXXXXX");
    int temp = mkstemp(h->path);
    if (temp < 0) return false;
    close(temp); unlink(h->path);
    if (mt_loop_create(&h->loop) != MT_OK || mt_httpd_create(h->loop, &h->http) != MT_OK) return false;
    h->log = mt_dns_query_log_create_sized(capacity);
    if (!h->log) return false;
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
    mt_httpd_set_middleware(h->http, authorize, NULL);
    mt_system_register_routes(h->http, &h->system);
    if (mt_httpd_route(h->http, "POST", "/test/append", append_handler, h) != MT_OK ||
        mt_httpd_route(h->http, "POST", "/test/flood", flood_handler, h) != MT_OK ||
        mt_httpd_listen_unix(h->http, h->path) != MT_OK) return false;
    return pthread_create(&h->thread, NULL, worker, h) == 0;
}
static void finish(harness_t *h) {
    mt_loop_stop(h->loop); pthread_join(h->thread, NULL);
    mt_httpd_destroy(h->http); mt_dns_query_log_destroy(h->log);
    mt_loop_destroy(h->loop); unlink(h->path);
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
static int request(harness_t *h, const char *method, const char *path, bool auth) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval timeout = {.tv_sec = 5};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", h->path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { close(fd); return -1; }
    char buffer[512];
    int n = snprintf(buffer, sizeof(buffer), "%s %s HTTP/1.1\r\nHost: test\r\n%sContent-Length: 0\r\n\r\n",
        method, path, auth ? "Authorization: Bearer test\r\n" : "");
    if (n <= 0 || send(fd, buffer, (size_t)n, MSG_NOSIGNAL) != n) { close(fd); return -1; }
    return fd;
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
        if (strcmp(kind, "snapshot_begin") == 0) {
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
            ok = received == count && count == expected_count && number(data, "through") == (double)through;
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
    ASSERT(snapshot(first, 0, 1000, &bytes, NULL, NULL));
    ASSERT(control(&h, "POST", "/test/append")); ASSERT(event_is(first, "query"));
    int second = request(&h, "GET", "/api/v1/system/dns/stream", true);
    ASSERT(second >= 0); ASSERT(headers(second, 200));
    ASSERT(snapshot(second, 1, 1000, &bytes, NULL, NULL));
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
        ASSERT(snapshot(fd, scenario == 1 ? 0 : 3000, 3000, &bytes, &h, action));
        if (scenario == 0) ASSERT(bytes > 2 * 1024 * 1024);
        ASSERT(control(&h, "POST", "/test/append")); ASSERT(event_is(fd, "query"));
        close(fd); finish(&h);
    }
    PASS();
}
TEST disconnect_and_subscriber_limit(void) {
    harness_t h; ASSERT(start(&h, 1000, false));
    int sockets[MT_HTTP_STREAM_MAX_CLIENTS]; size_t bytes = 0;
    for (size_t i = 0; i < MT_HTTP_STREAM_MAX_CLIENTS; i++) {
        sockets[i] = request(&h, "GET", "/api/v1/system/dns/stream", true);
        ASSERT(sockets[i] >= 0); ASSERT(headers(sockets[i], 200));
        ASSERT(snapshot(sockets[i], 0, 1000, &bytes, NULL, NULL));
    }
    int excess = request(&h, "GET", "/api/v1/system/dns/stream", true);
    ASSERT(excess >= 0); ASSERT(headers(excess, 503)); close(excess);
    ASSERT(control(&h, "POST", "/test/append"));
    finish(&h); /* Open subscriptions must release before the journal dies. */
    for (size_t i = 0; i < MT_HTTP_STREAM_MAX_CLIENTS; i++) close(sockets[i]);
    PASS();
}
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(snapshot_live_clear_and_authorization);
    RUN_TEST(large_snapshot_is_bounded_and_restarts_on_clear_or_eviction);
    RUN_TEST(disconnect_and_subscriber_limit);
    GREATEST_MAIN_END();
}
