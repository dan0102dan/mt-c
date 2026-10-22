#include "greatest.h"
#include "magitrickle/dns_keenetic_hook.h"
#ifdef MT_ENTWARE_KN
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    mt_loop_t *loop;
    mt_httpd_t *unix_http, *tcp_http;
    mt_dns_clients_t *clients;
    pthread_t thread;
    bool thread_started;
    int subscription;
    uint16_t port;
    char directory[64], socket_path[108];
    atomic_uint fetches, notifications;
    atomic_bool hold;
} fixture_t;

static void pause_ms(unsigned ms)
{
    struct timespec delay = {.tv_sec = (time_t)(ms / 1000u), .tv_nsec = (long)(ms % 1000u) * 1000000L};
    while (nanosleep(&delay, &delay) && errno == EINTR) {}
}
static mt_err_t fetch(mt_dns_client_t **out, size_t *count, const atomic_bool *stop, void *ud)
{
    fixture_t *f = ud;
    *out = NULL; *count = 0;
    unsigned call = atomic_fetch_add(&f->fetches, 1) + 1;
    while (atomic_load(&f->hold) && !atomic_load(stop)) { pause_ms(1); }
    if (atomic_load(stop)) { return MT_ERR_CANCELED; }
    *out = calloc(1, sizeof(**out));
    if (!*out) { return MT_ERR_NOMEM; }
    *count = 1;
    strcpy((*out)->ip, "192.168.1.42");
    (void)snprintf((*out)->name, sizeof((*out)->name), "Device %u", call);
    return MT_OK;
}
static void changed(void *ud) { atomic_fetch_add(&((fixture_t *)ud)->notifications, 1); }
static void *run_loop(void *ud) { mt_loop_run(((fixture_t *)ud)->loop); return NULL; }

static uint16_t unused_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { return 0; }
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t size = sizeof(address);
    bool ok = bind(fd, (struct sockaddr *)&address, size) == 0 &&
              getsockname(fd, (struct sockaddr *)&address, &size) == 0;
    close(fd);
    return ok ? ntohs(address.sin_port) : 0;
}
static bool setup(fixture_t *f, bool viewing)
{
    memset(f, 0, sizeof(*f));
    atomic_init(&f->fetches, 0); atomic_init(&f->notifications, 0);
    atomic_init(&f->hold, viewing);
    strcpy(f->directory, "/tmp/mt-dns-hook-XXXXXX");
    if (!mkdtemp(f->directory)) { f->directory[0] = 0; return false; }
    (void)snprintf(f->socket_path, sizeof(f->socket_path), "%s/api.sock", f->directory);
    if (mt_loop_create(&f->loop) != MT_OK ||
        mt_dns_clients_create(f->loop, fetch, f, &f->clients) != MT_OK ||
        mt_httpd_create(f->loop, &f->unix_http) != MT_OK ||
        mt_httpd_create(f->loop, &f->tcp_http) != MT_OK) { return false; }
    if (mt_dns_keenetic_register_hook(f->unix_http, f->clients) != MT_OK ||
        mt_httpd_listen_unix(f->unix_http, f->socket_path) != MT_OK) { return false; }
    f->port = unused_port();
    if (!f->port || mt_httpd_listen_tcp(f->tcp_http, "127.0.0.1", f->port) != MT_OK) { return false; }
    if (viewing) {
        f->subscription = mt_dns_clients_subscribe(f->clients, changed, f);
        if (!f->subscription) { return false; }
    }
    if (pthread_create(&f->thread, NULL, run_loop, f)) { return false; }
    f->thread_started = true;
    return true;
}
static void teardown(fixture_t *f)
{
    if (f->thread_started) { mt_loop_stop(f->loop); pthread_join(f->thread, NULL); }
    mt_httpd_destroy(f->tcp_http); mt_httpd_destroy(f->unix_http);
    mt_dns_clients_unsubscribe(f->clients, f->subscription);
    mt_dns_clients_destroy(f->clients);
    mt_loop_destroy(f->loop);
    if (f->socket_path[0]) { (void)unlink(f->socket_path); }
    if (f->directory[0]) { (void)rmdir(f->directory); }
}
static int request(const fixture_t *f, bool tcp, const char *method)
{
    int fd = socket(tcp ? AF_INET : AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { return -1; }
    struct timeval timeout = {.tv_sec = 1};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    int connected;
    if (tcp) {
        struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(f->port),
                                      .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        connected = connect(fd, (struct sockaddr *)&address, sizeof(address));
    } else {
        struct sockaddr_un address = {.sun_family = AF_UNIX};
        strcpy(address.sun_path, f->socket_path);
        connected = connect(fd, (struct sockaddr *)&address, sizeof(address));
    }
    if (connected) { close(fd); return -1; }
    char text[512];
    int length = snprintf(text, sizeof(text), "%s " MT_DNS_KEENETIC_HOOK_PATH
        " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nContent-Length: 0\r\n\r\n", method);
    if (length < 0 || (size_t)length >= sizeof(text) ||
        send(fd, text, (size_t)length, MSG_NOSIGNAL) != length) { close(fd); return -1; }
    size_t used = 0;
    while (used + 1 < sizeof(text)) {
        ssize_t n = recv(fd, text + used, sizeof(text) - used - 1, 0);
        if (n < 0 && errno == EINTR) { continue; }
        if (n <= 0) { break; }
        used += (size_t)n;
    }
    text[used] = 0; close(fd);
    int status = 0;
    (void)sscanf(text, "HTTP/1.1 %d", &status);
    return status;
}
static bool wait_counter(const atomic_uint *counter, unsigned expected)
{
    for (unsigned elapsed = 0; elapsed < 3000; elapsed += 5) {
        if (atomic_load(counter) >= expected) { return true; }
        pause_ms(5);
    }
    return false;
}

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "DNS hook check failed at %d: %s\n", __LINE__, #expr); ok = false; goto cleanup; } } while (0)
TEST hook_is_post_only_unix_only_and_idle_without_viewers(void)
{
    fixture_t f; bool ok = true;
    CHECK(setup(&f, false));
    CHECK(request(&f, false, "POST") == 202);
    CHECK(request(&f, false, "GET") == 404);
    CHECK(request(&f, true, "POST") == 404);
    pause_ms(1200);
    CHECK(atomic_load(&f.fetches) == 0);
    CHECK(atomic_load(&f.notifications) == 0);
cleanup:
    teardown(&f);
    if (!ok) { FAIL(); }
    PASS();
}
TEST hook_burst_acknowledges_while_fetch_is_blocked_and_requests_one_followup(void)
{
    fixture_t f; bool ok = true;
    CHECK(setup(&f, true));
    CHECK(wait_counter(&f.fetches, 1));
    for (unsigned i = 0; i < 10; i++) { CHECK(request(&f, false, "POST") == 202); }
    /* All acknowledgements arrive while discovery is deliberately held. */
    CHECK(atomic_load(&f.fetches) == 1);
    CHECK(atomic_load(&f.notifications) == 0);
    atomic_store(&f.hold, false);
    CHECK(wait_counter(&f.fetches, 2));
    CHECK(wait_counter(&f.notifications, 1));
    pause_ms(1200);
    CHECK(atomic_load(&f.fetches) == 2);
cleanup:
    teardown(&f);
    if (!ok) { FAIL(); }
    PASS();
}
#undef CHECK
#endif

GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
#ifdef MT_ENTWARE_KN
    RUN_TEST(hook_is_post_only_unix_only_and_idle_without_viewers);
    RUN_TEST(hook_burst_acknowledges_while_fetch_is_blocked_and_requests_one_followup);
#endif
    GREATEST_MAIN_END();
}
