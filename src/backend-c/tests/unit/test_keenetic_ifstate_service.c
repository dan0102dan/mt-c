#include "greatest.h"
#include "magitrickle/keenetic_ifstate_service.h"
#include "magitrickle/system.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static atomic_uint calls, changes;
static atomic_int answer;
static atomic_bool hold, loopback;
typedef struct {
    mt_loop_t *loop;
    mt_httpd_t *http, *ordinary;
    mt_kn_ifstate_t *cache;
    mt_kn_ifstate_service_t *service;
    mt_config_t config;
    mt_cache_t *records;
    mt_app_t *app;
    mt_system_ctx_t system;
    pthread_t thread;
    char dir[64], socket[128], ordinary_socket[128], marker[128];
} fixture_t;
static void pause_ms(unsigned ms)
{
    struct timespec delay = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&delay, NULL);
}
static double now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}
static mt_err_t fetch(mt_kn_ifinfo_t **out, size_t *count, const atomic_bool *stop)
{
    *out = NULL; *count = 0;
    int captured = atomic_load(&answer);
    atomic_fetch_add(&calls, 1);
    while (atomic_load(&hold) && !atomic_load(stop)) { pause_ms(1); }
    if (atomic_load(stop)) { return MT_ERR_CANCELED; }
    if (captured < 0) { return MT_ERR_UPSTREAM; }
    *out = calloc(1, sizeof(**out));
    if (!*out) { return MT_ERR_NOMEM; }
    *count = 1;
    strcpy((*out)->id, "Wireguard0");
    strcpy((*out)->name, atomic_load(&loopback) ? "lo" : "nwg0");
    strcpy((*out)->alias, captured ? "After" : "Before");
    (*out)->layered = true;
    for (size_t i = 0; i < 5; i++) { (*out)->fields[i] = MT_KN_IF_RUNNING; }
    if (captured == 1) { (*out)->fields[MT_KN_IF_CTRL] = MT_KN_IF_BLOCKED; }
    return MT_OK;
}
static void changed(void *ud, const char *name)
{
    (void)ud; (void)name;
    atomic_fetch_add(&changes, 1);
}
static void *run_loop(void *ud) { fixture_t *f = ud; mt_loop_run(f->loop); return NULL; }
static void status(mt_http_req_t *req, mt_http_res_t *res, void *ud)
{
    (void)req;
    fixture_t *f = ud;
    bool blocked = mt_kn_ifstate_blocks(f->cache, "nwg0", AF_INET);
    const char *text = blocked ? "blocked" : "permitted";
    mt_http_res_write(res, 200, "text/plain", (const uint8_t *)text, strlen(text));
}
static bool setup(fixture_t *f, bool marker)
{
    memset(f, 0, sizeof(*f));
    strcpy(f->dir, "/tmp/mt-ndm-service-XXXXXX");
    if (!mkdtemp(f->dir)) { return false; }
    snprintf(f->socket, sizeof(f->socket), "%s/api.sock", f->dir);
    snprintf(f->ordinary_socket, sizeof(f->ordinary_socket), "%s/ordinary.sock", f->dir);
    snprintf(f->marker, sizeof(f->marker), "%s/lost", f->dir);
    if (mt_loop_create(&f->loop) != MT_OK) { return false; }
    f->cache = mt_kn_ifstate_new(changed, f);
    if (!f->cache || mt_httpd_create(f->loop, &f->http) != MT_OK ||
        mt_httpd_create(f->loop, &f->ordinary) != MT_OK) { return false; }
    if (mt_httpd_listen_unix(f->http, f->socket) != MT_OK ||
        mt_httpd_listen_unix(f->ordinary, f->ordinary_socket) != MT_OK ||
        mt_httpd_route(f->http, "GET", "/test/status", status, f) != MT_OK) { return false; }
    if (mt_kn_ifstate_service_create(f->loop, f->cache, marker ? f->marker : NULL, fetch, &f->service) != MT_OK ||
        mt_kn_ifstate_register_hook(f->http, f->service) != MT_OK) { return false; }
    if (mt_config_init_defaults(&f->config) != MT_OK) { return false; }
    f->config.app.show_all_interfaces = true;
    f->records = mt_cache_create(0);
    mt_app_deps_t deps = {.cfg = &f->config, .cache = f->records};
    f->app = mt_app_create(&deps);
    if (!f->records || !f->app) { return false; }
    mt_app_set_interface_states(f->app, f->cache);
    f->system = (mt_system_ctx_t){.app = f->app, .interface_service = f->service};
    mt_system_register_routes(f->http, &f->system);
    return pthread_create(&f->thread, NULL, run_loop, f) == 0;
}
static void teardown(fixture_t *f)
{
    mt_loop_stop(f->loop); pthread_join(f->thread, NULL);
    mt_httpd_destroy(f->http); mt_httpd_destroy(f->ordinary);
    mt_kn_ifstate_service_destroy(f->service);
    mt_app_destroy(f->app); mt_cache_destroy(f->records); mt_config_clear(&f->config);
    mt_kn_ifstate_free(f->cache); mt_loop_destroy(f->loop);
    rmdir(f->marker); rmdir(f->dir);
}
static int request(const char *socket_path, const char *method, const char *path,
                    const char *body, char *response, size_t capacity)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0); if (fd < 0) { return -1; }
    struct timeval timeout = {2, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_un sa = {0}; sa.sun_family = AF_UNIX;
    if (strlen(socket_path) >= sizeof(sa.sun_path)) { close(fd); return -1; }
    strcpy(sa.sun_path, socket_path);
    if (connect(fd, (const struct sockaddr *)&sa, sizeof(sa))) { close(fd); return -1; }
    char text[4096];
    int length = snprintf(text, sizeof(text), "%s %s HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: %zu\r\n\r\n%s", method, path, body ? strlen(body) : 0, body ? body : "");
    if (length < 0 || (size_t)length >= sizeof(text) || send(fd, text, (size_t)length, 0) != length) { close(fd); return -1; }
    size_t n = 0;
    while (n + 1 < capacity) {
        ssize_t part = recv(fd, response + n, capacity - n - 1, 0);
        if (part < 0 && errno == EINTR) { continue; }
        if (part <= 0) { break; }
        n += (size_t)part;
    }
    response[n] = '\0'; close(fd);
    int code = 0; sscanf(response, "HTTP/1.1 %d", &code); return code;
}
static bool wait_counter(atomic_uint *value, unsigned expected)
{
    double deadline = now_ms() + 2500;
    while (atomic_load(value) < expected && now_ms() < deadline) { pause_ms(2); }
    return atomic_load(value) >= expected;
}
static bool permitted(fixture_t *f)
{
    char response[1024];
    return request(f->socket, "GET", "/test/status", NULL, response, sizeof(response)) == 200 && strstr(response, "permitted");
}
static const char *down = "{\"kind\":\"layer\",\"id\":\"Wireguard0\",\"system_name\":\"nwg0\",\"layer\":\"ctrl\",\"level\":\"disabled\"}";
static const char *up = "{\"kind\":\"layer\",\"id\":\"Wireguard0\",\"system_name\":\"nwg0\",\"layer\":\"ctrl\",\"level\":\"running\"}";
static void reset(void) { atomic_store(&calls, 0); atomic_store(&changes, 0); atomic_store(&answer, 0); atomic_store(&hold, false); atomic_store(&loopback, false); }
TEST hook_ingestion_is_bounded_and_ordinary_events_do_not_request_rci(void)
{
    reset(); fixture_t f; ASSERT(setup(&f, false));
    ASSERT(wait_counter(&changes, 1)); ASSERT(permitted(&f));
    char response[1024];
    for (size_t i = 0; i < 20; i++) { ASSERT_EQ(202, request(f.socket, "POST", MT_KN_IFSTATE_HOOK_PATH, down, response, sizeof(response))); }
    ASSERT_FALSE(permitted(&f));
    ASSERT_EQ(202, request(f.socket, "POST", MT_KN_IFSTATE_HOOK_PATH, up, response, sizeof(response)));
    ASSERT(permitted(&f));
    ASSERT_EQ(400, request(f.socket, "POST", MT_KN_IFSTATE_HOOK_PATH, "{}", response, sizeof(response)));
    ASSERT_EQ(404, request(f.ordinary_socket, "POST", MT_KN_IFSTATE_HOOK_PATH, down, response, sizeof(response)));
    pause_ms(60); /* longer than the one-shot coalescing interval, not a fake RCI poll */
    ASSERT_EQ(1u, atomic_load(&calls));
    ASSERT_EQ(3u, atomic_load(&changes));
    teardown(&f); PASS();
}
TEST slow_rcI_never_blocks_hook_ack_and_a_late_response_does_not_unblock(void)
{
    reset(); atomic_store(&hold, true);
    fixture_t f; ASSERT(setup(&f, false)); ASSERT(wait_counter(&calls, 1));
    atomic_store(&answer, 1); /* the next (fresh) read reflects the actual down transition */
    char response[1024]; double start = now_ms();
    ASSERT_EQ(202, request(f.socket, "POST", MT_KN_IFSTATE_HOOK_PATH, down, response, sizeof(response)));
    ASSERT(now_ms() - start < 500.0);
    ASSERT_FALSE(permitted(&f));
    atomic_store(&hold, false);
    ASSERT(wait_counter(&calls, 2));
    pause_ms(60); ASSERT_FALSE(permitted(&f));
    teardown(&f); PASS();
}
TEST reported_delivery_failure_triggers_resync_without_another_hook(void)
{
    reset(); fixture_t f; ASSERT(setup(&f, true));
    ASSERT(wait_counter(&changes, 1)); ASSERT(permitted(&f));
    atomic_store(&answer, 1);
    ASSERT_EQ(0, mkdir(f.marker, 0700));
    ASSERT(wait_counter(&calls, 2)); ASSERT(wait_counter(&changes, 2));
    ASSERT_FALSE(permitted(&f));
    ASSERT(access(f.marker, F_OK) != 0);
    /* Once synchronized there is no periodic readiness refresh. */
    pause_ms(60); ASSERT_EQ(2u, atomic_load(&calls));
    teardown(&f); PASS();
}
TEST rci_error_preserves_known_down_and_shutdown_cancels_inflight_work(void)
{
    reset(); fixture_t f; ASSERT(setup(&f, true));
    ASSERT(wait_counter(&changes, 1));
    char response[1024];
    ASSERT_EQ(202, request(f.socket, "POST", MT_KN_IFSTATE_HOOK_PATH, down, response, sizeof(response)));
    atomic_store(&answer, -1);
    ASSERT_EQ(0, mkdir(f.marker, 0700)); ASSERT(wait_counter(&calls, 2));
    pause_ms(60); ASSERT_FALSE(permitted(&f));
    teardown(&f);
    reset(); atomic_store(&hold, true);
    ASSERT(setup(&f, false)); ASSERT(wait_counter(&calls, 1));
    double start = now_ms(); teardown(&f);
    ASSERT(now_ms() - start < 500.0);
    PASS();
}

typedef struct { fixture_t *fixture; char response[4096]; int status; atomic_bool done; } reader_t;
static void *read_interfaces(void *ud)
{
    reader_t *r = ud;
    r->status = request(r->fixture->socket, "GET", "/api/v1/system/interfaces", NULL,
                        r->response, sizeof(r->response));
    atomic_store(&r->done, true);
    return NULL;
}
TEST interface_api_defers_without_blocking_the_shared_event_loop(void)
{
    reset(); atomic_store(&hold, true); atomic_store(&loopback, true);
    fixture_t f; ASSERT(setup(&f, false)); ASSERT(wait_counter(&calls, 1));
    reader_t reader = {.fixture = &f}; atomic_init(&reader.done, false);
    pthread_t thread; ASSERT_EQ(0, pthread_create(&thread, NULL, read_interfaces, &reader));
    pause_ms(50);
    ASSERT_FALSE(atomic_load(&reader.done));
    char response[4096]; double start = now_ms();
    ASSERT_EQ(200, request(f.socket, "GET", "/test/status", NULL, response, sizeof(response)));
    ASSERT(now_ms() - start < 500.0);
    atomic_store(&answer, 1); atomic_store(&hold, false);
    pthread_join(thread, NULL);
    ASSERT_EQ(200, reader.status);
    ASSERT(strstr(reader.response, "\"name\":\"After\""));
    ASSERT(atomic_load(&calls) >= 2u); /* cannot satisfy a new request with the old read */
    atomic_store(&answer, -1);
    ASSERT_EQ(200, request(f.socket, "GET", "/api/v1/system/interfaces", NULL, response, sizeof(response)));
    ASSERT(strstr(response, "\"id\":\"lo\""));
    ASSERT(strstr(response, "\"name\"") == NULL); /* failed refresh has kernel-only fallback */
    teardown(&f); PASS();
}
TEST disconnected_interface_request_is_safe_during_shutdown(void)
{
    reset(); atomic_store(&hold, true);
    fixture_t f; ASSERT(setup(&f, false)); ASSERT(wait_counter(&calls, 1));
    int fd = socket(AF_UNIX, SOCK_STREAM, 0); ASSERT(fd >= 0);
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    ASSERT(strlen(f.socket) < sizeof(sa.sun_path)); strcpy(sa.sun_path, f.socket);
    ASSERT_EQ(0, connect(fd, (const struct sockaddr *)&sa, sizeof(sa)));
    const char request_text[] = "GET /api/v1/system/interfaces HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ((ssize_t)(sizeof(request_text)-1), send(fd, request_text, sizeof(request_text)-1, 0));
    pause_ms(30); close(fd);
    double start = now_ms(); teardown(&f);
    ASSERT(now_ms() - start < 500.0);
    PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(interface_api_defers_without_blocking_the_shared_event_loop);
    RUN_TEST(disconnected_interface_request_is_safe_during_shutdown);
    RUN_TEST(hook_ingestion_is_bounded_and_ordinary_events_do_not_request_rci);
    RUN_TEST(slow_rcI_never_blocks_hook_ack_and_a_late_response_does_not_unblock);
    RUN_TEST(reported_delivery_failure_triggers_resync_without_another_hook);
    RUN_TEST(rci_error_preserves_known_down_and_shutdown_cancels_inflight_work);
    GREATEST_MAIN_END();
}
