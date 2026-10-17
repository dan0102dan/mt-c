/* Exercise the real HTTP transport and DNS stream on the same loop. */
#include "greatest.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include "magitrickle/system.h"
#include "magitrickle/http_stream.h"

typedef struct {
    mt_loop_t *loop;
    mt_httpd_t *http;
    mt_dns_query_log_t *log;
    mt_system_ctx_t ctx;
    pthread_t thread;
    char path[100];
} harness_t;

static bool authorize(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)ud;
    const char *token = mt_http_req_header(req, "Authorization");
    if (token && strcmp(token, "Bearer test") == 0) { return true; }
    mt_http_res_write_error(res, 401, "Unauthorized");
    return false;
}
static void append_query(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    harness_t *h = ud;
    unsigned count = mt_http_req_query_is_true(req, "burst") ? 1100u : 1u;
    for (unsigned i = 0; i < count; i++) {
        mt_dns_query_log_append(h->log, "192.168.1.11", "live.example", 28, true, 222);
    }
    mt_http_res_write_json(res, 200, cJSON_CreateObject());
}
static void *run_loop(void *ud) { mt_loop_run(((harness_t *)ud)->loop); return NULL; }
static harness_t *start(void) {
    harness_t *h = calloc(1, sizeof(*h));
    if (!h) { return NULL; }
    h->log = mt_dns_query_log_create();
    if (!h->log || mt_loop_create(&h->loop) != MT_OK) { goto fail; }
    if (mt_httpd_create(h->loop, &h->http) != MT_OK) { goto fail; }
    h->ctx.query_log = h->log;
    mt_system_register_routes(h->http, &h->ctx);
    mt_httpd_set_middleware(h->http, authorize, NULL);
    if (mt_httpd_route(h->http, "POST", "/test/append", append_query, h) != MT_OK) { goto fail; }
    snprintf(h->path, sizeof(h->path), "/tmp/mt-dns-stream-%ld.sock", (long)getpid());
    if (mt_httpd_listen_unix(h->http, h->path) != MT_OK) { goto fail; }
    mt_dns_query_log_append(h->log, "127.0.0.1", "initial.example", 1, false, 111);
    if (pthread_create(&h->thread, NULL, run_loop, h) != 0) { goto fail; }
    return h;
fail:
    mt_httpd_destroy(h->http); mt_loop_destroy(h->loop);
    mt_dns_query_log_destroy(h->log); free(h); return NULL;
}
static void stop(harness_t *h) {
    mt_loop_stop(h->loop); pthread_join(h->thread, NULL);
    /* Live streams must unsubscribe before the journal is destroyed. */
    mt_httpd_destroy(h->http); mt_loop_destroy(h->loop);
    mt_dns_query_log_destroy(h->log); free(h);
}
static int request(harness_t *h, const char *method, const char *path, bool auth) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { return -1; }
    struct sockaddr_un sa = {0}; sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", h->path);
    struct timeval timeout = {.tv_sec = 3};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { close(fd); return -1; }
    char buf[512];
    int size = snprintf(buf, sizeof(buf), "%s %s HTTP/1.1\r\nHost: localhost\r\n%sContent-Length: 0\r\n\r\n",
        method, path, auth ? "Authorization: Bearer test\r\n" : "");
    if (size < 0 || (size_t)size >= sizeof(buf) || send(fd, buf, (size_t)size, 0) != size) {
        close(fd); return -1;
    }
    return fd;
}
static bool receive_until(int fd, char *buf, size_t cap, const char *needle) {
    size_t used = 0; buf[0] = '\0';
    while (used + 1 < cap) {
        ssize_t n = recv(fd, buf + used, cap - used - 1, 0);
        if (n <= 0) { return false; }
        used += (size_t)n; buf[used] = '\0';
        if (strstr(buf, needle)) { return true; }
    }
    return false;
}
static bool command(harness_t *h, const char *method, const char *path) {
    int fd = request(h, method, path, true);
    if (fd < 0) { return false; }
    char buf[4096];
    bool ok = receive_until(fd, buf, sizeof(buf), "\r\n\r\n") && strstr(buf, "200 OK");
    close(fd); return ok;
}

TEST authenticated_stream_pushes_and_broadcasts_clear(void) {
    harness_t *h = start(); ASSERT(h);
    char buf[8192];
    int denied = request(h, "GET", "/api/v1/system/dns/stream", false); ASSERT(denied >= 0);
    ASSERT(receive_until(denied, buf, sizeof(buf), "Unauthorized"));
    ASSERT(strstr(buf, "401")); close(denied);
    int a = request(h, "GET", "/api/v1/system/dns/stream", true); ASSERT(a >= 0);
    ASSERT(receive_until(a, buf, sizeof(buf), "\n\n"));
    ASSERT(strstr(buf, "text/event-stream")); ASSERT(strstr(buf, "Transfer-Encoding: chunked"));
    ASSERT(!strstr(buf, "Content-Length:")); ASSERT(strstr(buf, "initial.example"));
    int b = request(h, "GET", "/api/v1/system/dns/stream", true); ASSERT(b >= 0);
    ASSERT(receive_until(b, buf, sizeof(buf), "\n\n"));
    ASSERT(command(h, "POST", "/test/append"));
    ASSERT(receive_until(a, buf, sizeof(buf), "\n\n")); ASSERT(strstr(buf, "event: query"));
    ASSERT(strstr(buf, "live.example")); ASSERT(strstr(buf, "192.168.1.11"));
    ASSERT(receive_until(b, buf, sizeof(buf), "\n\n")); ASSERT(strstr(buf, "event: query"));
    ASSERT(command(h, "DELETE", "/api/v1/system/dns/queries"));
    ASSERT(receive_until(a, buf, sizeof(buf), "\n\n")); ASSERT(strstr(buf, "event: clear"));
    ASSERT(receive_until(b, buf, sizeof(buf), "\n\n")); ASSERT(strstr(buf, "event: clear"));
    ASSERT(command(h, "POST", "/test/append"));
    ASSERT(receive_until(a, buf, sizeof(buf), "\n\n")); ASSERT(strstr(buf, "event: query"));
    close(a); stop(h); close(b);
    PASS();
}

TEST overflow_resynchronizes_and_subscribers_are_bounded(void) {
    harness_t *h = start(); ASSERT(h);
    char *buf = malloc(MT_HTTP_STREAM_MAX_FRAME); ASSERT(buf);
    int clients[MT_HTTP_STREAM_MAX_CLIENTS];
    for (size_t i = 0; i < MT_HTTP_STREAM_MAX_CLIENTS; i++) {
        clients[i] = request(h, "GET", "/api/v1/system/dns/stream", true); ASSERT(clients[i] >= 0);
        ASSERT(receive_until(clients[i], buf, MT_HTTP_STREAM_MAX_FRAME, "\n\n"));
    }
    int extra = request(h, "GET", "/api/v1/system/dns/stream", true); ASSERT(extra >= 0);
    ASSERT(receive_until(extra, buf, MT_HTTP_STREAM_MAX_FRAME, "\r\n\r\n")); ASSERT(strstr(buf, "503")); close(extra);
    ASSERT(command(h, "POST", "/test/append?burst=true"));
    ASSERT(receive_until(clients[0], buf, MT_HTTP_STREAM_MAX_FRAME, "\n\n"));
    ASSERT(strstr(buf, "event: snapshot")); ASSERT(strstr(buf, "live.example"));
    ASSERT(!strstr(buf, "initial.example"));
    free(buf); stop(h);
    for (size_t i = 0; i < MT_HTTP_STREAM_MAX_CLIENTS; i++) { close(clients[i]); }
    PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(authenticated_stream_pushes_and_broadcasts_clear);
    RUN_TEST(overflow_resynchronizes_and_subscribers_are_bounded);
    GREATEST_MAIN_END();
}
