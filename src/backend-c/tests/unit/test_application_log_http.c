/* Real HTTP/SSE + event loop: exercise the worker-to-loop pipe boundary,
 * which the smaller protocol test deliberately replaces. No production hooks. */
#include "greatest.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include "magitrickle/http_stream.h"
#include "magitrickle/log_history.h"
#include "magitrickle/system.h"

typedef struct {
    mt_loop_t *loop;
    mt_httpd_t *http;
    mt_system_ctx_t system;
    pthread_t thread;
    atomic_bool revoked;
    char path[108];
} harness_t;
static bool authorize(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    harness_t *h = ud;
    const char *token = mt_http_req_header(req, "Authorization");
    if (!atomic_load(&h->revoked) && token && !strcmp(token, "Bearer test")) return true;
    mt_http_res_write_error(res, 401, "unauthorized");
    return false;
}
static void *run(void *ud) {
    harness_t *h = ud;
    (void)mt_loop_run(h->loop);
    return NULL;
}
static bool start(harness_t *h) {
    memset(h, 0, sizeof(*h));
    atomic_init(&h->revoked, false);
    snprintf(h->path, sizeof(h->path), "/tmp/mt-app-log-http-XXXXXX");
    int tmp = mkstemp(h->path);
    if (tmp < 0) return false;
    close(tmp); unlink(h->path);
    if (mt_loop_create(&h->loop) != MT_OK || mt_httpd_create(h->loop, &h->http) != MT_OK) return false;
    mt_httpd_set_middleware(h->http, authorize, h);
    mt_system_register_routes(h->http, &h->system);
    if (mt_httpd_listen_unix(h->http, h->path) != MT_OK) return false;
    return pthread_create(&h->thread, NULL, run, h) == 0;
}
static void finish(harness_t *h) {
    mt_loop_stop(h->loop);
    pthread_join(h->thread, NULL);
    mt_httpd_destroy(h->http);
    mt_loop_destroy(h->loop);
    unlink(h->path);
}
static bool read_exact(int fd, char *out, size_t len) {
    while (len) {
        ssize_t n = recv(fd, out, len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        out += (size_t)n; len -= (size_t)n;
    }
    return true;
}
static bool read_line(int fd, char *out, size_t size) {
    for (size_t i = 0; i + 1 < size; i++) {
        if (!read_exact(fd, out + i, 1)) return false;
        if (out[i] == '\n') { out[i + 1] = 0; return true; }
    }
    return false;
}
static int request(harness_t *h, const char *method, const char *path, const char *body, bool auth) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval timeout = {.tv_sec = 5};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", h->path);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) { close(fd); return -1; }
    char buffer[1024];
    int n = snprintf(buffer, sizeof(buffer),
        "%s %s HTTP/1.1\r\nHost: test\r\n%sContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
        method, path, auth ? "Authorization: Bearer test\r\n" : "", strlen(body), body);
    if (n < 0 || (size_t)n >= sizeof(buffer) || send(fd, buffer, (size_t)n, MSG_NOSIGNAL) != n) {
        close(fd); return -1;
    }
    return fd;
}
static bool headers(int fd, int expected) {
    char line[512]; int actual = 0;
    if (!read_line(fd, line, sizeof(line)) || sscanf(line, "HTTP/1.1 %d", &actual) != 1 || actual != expected) return false;
    while (read_line(fd, line, sizeof(line))) if (!strcmp(line, "\r\n")) return true;
    return false;
}
static cJSON *event(int fd, const char *expected) {
    for (;;) {
        char line[64], suffix[2];
        if (!read_line(fd, line, sizeof(line))) return NULL;
        char *end;
        unsigned long length = strtoul(line, &end, 16);
        if (end == line || !length || length > MT_HTTP_STREAM_MAX_FRAME) return NULL;
        char *data = malloc((size_t)length + 1);
        if (!data) return NULL;
        bool ok = read_exact(fd, data, (size_t)length) && read_exact(fd, suffix, 2);
        data[length] = 0;
        if (!ok || memcmp(suffix, "\r\n", 2)) { free(data); return NULL; }
        if (data[0] == ':') { free(data); continue; }
        char kind[32];
        const char *body = strstr(data, "\ndata: ");
        cJSON *result = body && sscanf(data, "event: %31s", kind) == 1 && !strcmp(kind, expected)
            ? cJSON_Parse(body + 7) : NULL;
        free(data);
        return result;
    }
}
static bool consume(int fd, const char *kind) {
    cJSON *json = event(fd, kind);
    bool ok = json != NULL;
    cJSON_Delete(json);
    return ok;
}
static bool capture(harness_t *h, bool enable) {
    int fd = request(h, "PUT", "/api/v1/logs/capture", enable ? "{\"enabled\":true}" : "{\"enabled\":false}", true);
    bool ok = fd >= 0 && headers(fd, 200);
    if (fd >= 0) close(fd);
    return ok;
}

TEST real_stream_off_live_resume_and_auth(void) {
    ASSERT(mt_log_history_enable(false));
    harness_t h;
    ASSERT(start(&h));
    int denied = request(&h, "PUT", "/api/v1/logs/capture", "{\"enabled\":true}", false);
    ASSERT(denied >= 0 && headers(denied, 401)); close(denied);
    int stream = request(&h, "GET", "/api/v1/logs/stream", "", true);
    ASSERT(stream >= 0 && headers(stream, 200));
    ASSERT(consume(stream, "capture") && consume(stream, "ready"));
    MT_INFO("before Start is not stored");
    mt_log_history_state_t state;
    mt_log_history_state(&state); ASSERT(!state.enabled && state.count == 0);
    ASSERT(capture(&h, true));
    ASSERT(consume(stream, "capture") && consume(stream, "ready"));
    /* Producer is this thread; HTTP dispatch is another. This must wake the
     * idle SSE socket, rather than wait for its 15-second heartbeat. */
    MT_INFO("worker event");
    cJSON *json = event(stream, "log"); ASSERT(json);
    ASSERT_STR_EQ("1", cJSON_GetObjectItemCaseSensitive(json, "id")->valuestring);
    char session[33]; snprintf(session, sizeof(session), "%s", cJSON_GetObjectItemCaseSensitive(json, "session")->valuestring);
    cJSON_Delete(json);
    close(stream);
    MT_INFO("recorded while disconnected");
    char path[128]; snprintf(path, sizeof(path), "/api/v1/logs/stream?session=%s&after=1", session);
    stream = request(&h, "GET", path, "", true);
    ASSERT(stream >= 0 && headers(stream, 200));
    ASSERT(consume(stream, "capture"));
    json = event(stream, "log"); ASSERT(json);
    ASSERT_STR_EQ("2", cJSON_GetObjectItemCaseSensitive(json, "id")->valuestring);
    cJSON_Delete(json); ASSERT(consume(stream, "ready"));
    ASSERT(capture(&h, false));
    ASSERT(consume(stream, "capture") && consume(stream, "ready"));
    mt_log_history_state(&state); ASSERT(!state.enabled && !state.count);
    /* The ordinary stream authorization maintenance must still run with the
     * new notification watch attached, even when capture is stopped. */
    atomic_store(&h.revoked, true);
    char byte;
    ASSERT_EQ(0, recv(stream, &byte, 1, 0));
    close(stream);
    finish(&h);
    PASS();
}

static atomic_bool writing;
static void *produce(void *unused) {
    (void)unused;
    for (int i = 0; i < 2000 && atomic_load(&writing); i++) MT_INFO("cleanup race %d", i);
    return NULL;
}
TEST active_notification_teardown_is_safe(void) {
    ASSERT(mt_log_history_enable(true));
    harness_t h; ASSERT(start(&h));
    int stream = request(&h, "GET", "/api/v1/logs/stream", "", true);
    ASSERT(stream >= 0 && headers(stream, 200));
    ASSERT(consume(stream, "capture") && consume(stream, "ready"));
    atomic_store(&writing, true);
    pthread_t thread; ASSERT_EQ(0, pthread_create(&thread, NULL, produce, NULL));
    close(stream);
    /* Destroy the HTTP owner with a producer still active. Unsubscribe must
     * synchronize with pending notifications before closing either pipe end. */
    finish(&h);
    atomic_store(&writing, false);
    ASSERT_EQ(0, pthread_join(thread, NULL));
    ASSERT(mt_log_history_enable(false));
    PASS();
}
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    int fd = open("/dev/null", O_WRONLY);
    if (fd < 0) return 1;
    mt_log_set_fd(fd);
    GREATEST_MAIN_BEGIN();
    RUN_TEST(real_stream_off_live_resume_and_auth);
    RUN_TEST(active_notification_teardown_is_safe);
    (void)mt_log_history_enable(false);
    mt_log_set_fd(STDOUT_FILENO); close(fd);
    GREATEST_MAIN_END();
}
