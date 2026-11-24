/* Exercise the production transport over ordinary local sockets. Only the
 * original-destination port is mapped to a local fixture; no root, transparent
 * socket exception or production testing API is needed. */
#define _GNU_SOURCE /* NOLINT(bugprone-reserved-identifier) */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "greatest.h"

static uint16_t fixture_port;
static int fixture_connect(int fd, const struct sockaddr *address, socklen_t length)
{
    if (fixture_port && address->sa_family == AF_INET) {
        struct sockaddr_in mapped = *(const struct sockaddr_in *)address;
        mapped.sin_port = htons(fixture_port);
        return connect(fd, (const struct sockaddr *)&mapped, sizeof(mapped));
    }
    return connect(fd, address, length);
}
#define connect fixture_connect
#include "../../src/platform/tproxy.c"
#undef connect

struct probe_ctx {
    mt_loop_t *loop;
    unsigned direct_prepares;
    unsigned alternate_prepares;
    unsigned direct_learned;
    unsigned alternate_learned;
    bool reject;
    bool reject_direct;
    bool reject_all;
    bool stop_on_learn;
};

static bool authorize(void *userdata, const mt_tproxy_endpoint_t *client,
                       const mt_tproxy_endpoint_t *destination)
{
    (void)client; (void)destination;
    struct probe_ctx *ctx = userdata;
    return !ctx->reject;
}

static mt_err_t prepare(void *userdata, int fd, const mt_tproxy_endpoint_t *client,
                         const mt_tproxy_endpoint_t *destination, bool alternate)
{
    (void)fd; (void)client; (void)destination;
    struct probe_ctx *ctx = userdata;
    if (alternate) { ctx->alternate_prepares++; } else { ctx->direct_prepares++; }
    return ctx->reject_all || (!alternate && ctx->reject_direct) ? MT_ERR_UPSTREAM : MT_OK;
}

static void learned(void *userdata, const mt_tproxy_endpoint_t *client,
                      const mt_tproxy_endpoint_t *destination, bool alternate)
{
    (void)client; (void)destination;
    struct probe_ctx *ctx = userdata;
    if (alternate) { ctx->alternate_learned++; } else { ctx->direct_learned++; }
    if (ctx->stop_on_learn) { mt_loop_stop(ctx->loop); }
}

static mt_tproxy_t *fixture_proxy(mt_loop_t *loop, struct probe_ctx *ctx)
{
    mt_tproxy_t *p = calloc(1, sizeof(*p));
    if (!p) { abort(); }
    ctx->loop = loop;
    p->loop = loop; p->ctx = ctx;
    p->listener4 = p->listener6 = -1;
    p->ops = (mt_tproxy_ops_t){.authorize = authorize, .prepare = prepare, .learned = learned};
    p->config = (mt_tproxy_config_t){.ipv4 = true, .connect_timeout_ms = 50,
        .tls_timeout_ms = 20, .idle_timeout_ms = 1000, .max_connections = 8};
    return p;
}

static tproxy_flow_t *fixture_flow(mt_tproxy_t *p, int client, int upstream, bool tls)
{
    tproxy_flow_t *f = calloc(1, sizeof(*f));
    if (!f) { abort(); }
    f->proxy = p; f->client_fd = client; f->upstream_fd = upstream;
    f->destination = (mt_tproxy_endpoint_t){.family = AF_INET, .addr = {127,0,0,1},
        .port = tls ? 443 : 80};
    f->client = (mt_tproxy_endpoint_t){.family = AF_INET, .addr = {192,168,1,5}, .port = 30000};
    f->tls = tls ? TLS_INSPECT : TLS_OFF;
    f->active_at = monotonic_ms();
    f->next = p->flows; f->prev = &p->flows;
    if (f->next) { f->next->prev = &f->next; }
    p->flows = f; p->stats.active++; p->stats.accepted++;
    return f;
}

static int make_pair(int fds[2])
{
    return socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds);
}

static size_t hello_bytes(uint8_t *out, bool early_data, bool fragmented)
{
    uint8_t handshake[64] = {1,0,0,0,3,3};
    size_t n = 38;
    handshake[n++] = 0; /* session ID */
    handshake[n++] = 0; handshake[n++] = 2;
    handshake[n++] = 0x13; handshake[n++] = 1;
    handshake[n++] = 1; handshake[n++] = 0;
    handshake[n++] = 0; handshake[n++] = early_data ? 4 : 0;
    if (early_data) {
        handshake[n++] = 0; handshake[n++] = 42;
        handshake[n++] = 0; handshake[n++] = 0;
    }
    handshake[3] = (uint8_t)(n - 4);
    size_t at = 0, part = fragmented ? 2 : n;
    out[at++] = 22; out[at++] = 3; out[at++] = 1;
    out[at++] = 0; out[at++] = (uint8_t)part;
    memcpy(out + at, handshake, part); at += part;
    if (fragmented) {
        out[at++] = 22; out[at++] = 3; out[at++] = 3;
        out[at++] = 0; out[at++] = (uint8_t)(n - part);
        memcpy(out + at, handshake + part, n - part); at += n - part;
    }
    return at;
}

static const uint8_t server_hello[] = {22,3,3,0,42,2,0,0,38};
static const uint8_t application[] = {23,3,3,0,4,'P','O','S','T'};

TEST client_hello_inspection_is_incremental_and_bounded(void)
{
    for (unsigned fragmented = 0; fragmented < 2; fragmented++) {
        uint8_t hello[128]; size_t n = hello_bytes(hello, false, fragmented != 0), wire = 0;
        for (size_t i = 0; i < n; i++) { ASSERT_EQ(HELLO_MORE, inspect_hello(hello, i, &wire)); }
        ASSERT_EQ(HELLO_SAFE, inspect_hello(hello, n, &wire)); ASSERT_EQ(n, wire);
        memcpy(hello + n, application, sizeof(application));
        ASSERT_EQ(HELLO_SAFE, inspect_hello(hello, n + sizeof(application), &wire));
        ASSERT_EQ(n, wire); /* later application data is never in the replay */
        n = hello_bytes(hello, true, fragmented != 0);
        ASSERT_EQ(HELLO_OPAQUE, inspect_hello(hello, n, &wire));
    }
    uint8_t garbage[8] = {22,3,3,0xff,0xff}; size_t wire = 0;
    ASSERT_EQ(HELLO_OPAQUE, inspect_hello(garbage, sizeof(garbage), &wire));
    ASSERT_EQ(HELLO_OPAQUE, inspect_hello((const uint8_t *)"POST /x", 7, &wire));
    uint8_t hello[128]; size_t n = hello_bytes(hello, false, false);
    hello[4]++; hello[n++] = 1; /* extra handshake in the same record */
    ASSERT_EQ(HELLO_OPAQUE, inspect_hello(hello, n, &wire));
    PASS();
}

TEST tls_holds_application_bytes_until_server_response(void)
{
    mt_loop_t *loop = NULL; ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    struct probe_ctx ctx = {0}; mt_tproxy_t *p = fixture_proxy(loop, &ctx);
    int client[2], server[2]; ASSERT_EQ(0, make_pair(client)); ASSERT_EQ(0, make_pair(server));
    tproxy_flow_t *f = fixture_flow(p, client[0], server[0], true);
    uint8_t input[128], received[128]; size_t n = hello_bytes(input, false, true);
    memcpy(input + n, application, sizeof(application));
    ASSERT_EQ((ssize_t)(n + sizeof(application)), send(client[1], input, n + sizeof(application), 0));
    flow_event(loop, client[0], EPOLLIN, f);
    ASSERT_EQ((ssize_t)n, recv(server[1], received, sizeof(received), 0));
    ASSERT_EQ(0, memcmp(input, received, n));
    ASSERT_EQ(-1, recv(server[1], received, sizeof(received), 0)); ASSERT_EQ(EAGAIN, errno);
    ASSERT(can_retry(f)); ASSERT_EQ(n, f->to_server.off);
    ASSERT_EQ((ssize_t)sizeof(server_hello), send(server[1], server_hello, sizeof(server_hello), 0));
    flow_event(loop, server[0], EPOLLIN, f);
    ASSERT_EQ((ssize_t)sizeof(application), recv(server[1], received, sizeof(received), 0));
    ASSERT_EQ(0, memcmp(application, received, sizeof(application)));
    ASSERT_EQ((ssize_t)sizeof(server_hello), recv(client[1], received, sizeof(received), 0));
    ASSERT(!can_retry(f)); ASSERT_EQ(1u, ctx.direct_learned);
    ASSERT_EQ(1u, p->stats.direct_success); ASSERT_EQ(0u, p->stats.alternate_success);
    mt_tproxy_destroy(p); close(client[1]); close(server[1]); mt_loop_destroy(loop);
    PASS();
}

TEST post_and_early_data_are_never_replayed(void)
{
    for (unsigned tls = 0; tls < 2; tls++) {
        mt_loop_t *loop = NULL; ASSERT_EQ(MT_OK, mt_loop_create(&loop));
        struct probe_ctx ctx = {0}; mt_tproxy_t *p = fixture_proxy(loop, &ctx);
        int client[2], server[2]; ASSERT_EQ(0, make_pair(client)); ASSERT_EQ(0, make_pair(server));
        tproxy_flow_t *f = fixture_flow(p, client[0], server[0], tls != 0);
        uint8_t input[128], received[128]; size_t n;
        if (tls) {
            n = hello_bytes(input, true, false);
            memcpy(input + n, application, sizeof(application)); n += sizeof(application);
        } else {
            const char *request = "POST /transfer HTTP/1.1\r\nContent-Length: 1\r\n\r\n1";
            n = strlen(request); memcpy(input, request, n);
        }
        ASSERT_EQ((ssize_t)n, send(client[1], input, n, 0));
        flow_event(loop, client[0], EPOLLIN, f);
        ASSERT_EQ((ssize_t)n, recv(server[1], received, sizeof(received), 0));
        ASSERT_EQ(0, memcmp(input, received, n));
        ASSERT(!can_retry(f)); ASSERT(!retry_upstream(f));
        ASSERT_EQ(0u, ctx.alternate_prepares); ASSERT(!f->alternate);
        mt_tproxy_destroy(p); close(client[1]); close(server[1]); mt_loop_destroy(loop);
    }
    PASS();
}

TEST any_server_bytes_end_retry_but_alerts_do_not_teach_routes(void)
{
    mt_loop_t *loop = NULL; ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    struct probe_ctx ctx = {0}; mt_tproxy_t *p = fixture_proxy(loop, &ctx);
    int client[2], server[2]; ASSERT_EQ(0, make_pair(client)); ASSERT_EQ(0, make_pair(server));
    tproxy_flow_t *f = fixture_flow(p, client[0], server[0], true);
    static const uint8_t alert[] = {21,3,3,0,2,2,40};
    ASSERT_EQ((ssize_t)sizeof(alert), send(server[1], alert, sizeof(alert), 0));
    flow_event(loop, server[0], EPOLLIN, f);
    ASSERT(f->server_seen); ASSERT(!can_retry(f)); ASSERT_EQ(0u, ctx.direct_learned);
    uint8_t received[16]; ASSERT_EQ((ssize_t)sizeof(alert), recv(client[1], received, sizeof(received), 0));
    ASSERT_EQ(0, memcmp(received, alert, sizeof(alert)));
    mt_tproxy_destroy(p); close(client[1]); close(server[1]); mt_loop_destroy(loop);
    PASS();
}

TEST fragmented_server_hello_teaches_once_after_header(void)
{
    mt_loop_t *loop = NULL; ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    struct probe_ctx ctx = {0}; mt_tproxy_t *p = fixture_proxy(loop, &ctx);
    int client[2], server[2]; ASSERT_EQ(0, make_pair(client)); ASSERT_EQ(0, make_pair(server));
    tproxy_flow_t *f = fixture_flow(p, client[0], server[0], true);
    f->alternate = true;
    for (size_t i = 0; i < sizeof(server_hello); i++) {
        ASSERT_EQ(1, send(server[1], server_hello + i, 1, 0));
        flow_event(loop, server[0], EPOLLIN, f);
        ASSERT_EQ(i + 1 == sizeof(server_hello) ? 1u : 0u, ctx.alternate_learned);
    }
    ASSERT_EQ(4, send(server[1], "more", 4, 0));
    flow_event(loop, server[0], EPOLLIN, f);
    ASSERT_EQ(1u, ctx.alternate_learned); ASSERT_EQ(1u, p->stats.alternate_success);
    mt_tproxy_destroy(p); close(client[1]); close(server[1]); mt_loop_destroy(loop);
    PASS();
}

TEST partial_writes_preserve_every_byte_and_apply_backpressure(void)
{
    mt_loop_t *loop = NULL; ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    struct probe_ctx ctx = {0}; mt_tproxy_t *p = fixture_proxy(loop, &ctx);
    int client[2], server[2]; ASSERT_EQ(0, make_pair(client)); ASSERT_EQ(0, make_pair(server));
    int tiny = 1024; ASSERT_EQ(0, setsockopt(server[0], SOL_SOCKET, SO_SNDBUF, &tiny, sizeof(tiny)));
    tproxy_flow_t *f = fixture_flow(p, client[0], server[0], false);
    f->to_server.len = sizeof(f->to_server.bytes);
    for (size_t i = 0; i < f->to_server.len; i++) { f->to_server.bytes[i] = (uint8_t)i; }
    ASSERT(write_upstream(f)); ASSERT(f->to_server.off > 0); ASSERT(f->to_server.off < f->to_server.len);
    size_t sent = f->to_server.off; ASSERT(write_upstream(f)); ASSERT_EQ(sent, f->to_server.off);
    uint8_t received[MT_TPROXY_BUFFER]; size_t used = 0;
    while (used < sizeof(received)) {
        ssize_t n = recv(server[1], received + used, sizeof(received) - used, 0);
        ASSERT(n > 0); used += (size_t)n;
        ASSERT(write_upstream(f));
    }
    ASSERT_EQ(0, memcmp(received, f->to_server.bytes, sizeof(received)));
    ASSERT_EQ(f->to_server.len, f->to_server.off); ASSERT(!can_retry(f));
    mt_tproxy_destroy(p); close(client[1]); close(server[1]); mt_loop_destroy(loop);
    PASS();
}

TEST half_closes_flush_request_then_response(void)
{
    mt_loop_t *loop = NULL; ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    struct probe_ctx ctx = {0}; mt_tproxy_t *p = fixture_proxy(loop, &ctx);
    int client[2], server[2]; ASSERT_EQ(0, make_pair(client)); ASSERT_EQ(0, make_pair(server));
    tproxy_flow_t *f = fixture_flow(p, client[0], server[0], false);
    ASSERT_EQ(7, send(client[1], "request", 7, 0)); ASSERT_EQ(0, shutdown(client[1], SHUT_WR));
    flow_event(loop, client[0], EPOLLIN | EPOLLRDHUP, f);
    flow_event(loop, client[0], EPOLLIN | EPOLLRDHUP, f);
    uint8_t received[64]; ASSERT_EQ(7, recv(server[1], received, sizeof(received), 0));
    ASSERT_EQ(0, memcmp(received, "request", 7)); ASSERT_EQ(0, recv(server[1], received, sizeof(received), 0));
    ASSERT_EQ(8, send(server[1], "response", 8, 0)); ASSERT_EQ(0, shutdown(server[1], SHUT_WR));
    flow_event(loop, server[0], EPOLLIN | EPOLLRDHUP, f);
    flow_event(loop, server[0], EPOLLIN | EPOLLRDHUP, f);
    ASSERT_EQ(8, recv(client[1], received, sizeof(received), 0)); ASSERT_EQ(0, memcmp(received, "response", 8));
    ASSERT_EQ(0, recv(client[1], received, sizeof(received), 0));
    ASSERT_EQ(0u, p->stats.active); ASSERT_EQ(0u, p->stats.failures);
    mt_tproxy_destroy(p); close(client[1]); close(server[1]); mt_loop_destroy(loop);
    PASS();
}

static int local_listener(uint16_t *port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { return -1; }
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t n = sizeof(addr);
    if (bind(fd, (struct sockaddr *)&addr, n) != 0 || listen(fd, 4) != 0 ||
        getsockname(fd, (struct sockaddr *)&addr, &n) != 0) { close(fd); return -1; }
    *port = ntohs(addr.sin_port);
    return fd;
}

TEST connection_limit_pauses_and_resumes_listener(void)
{
    mt_loop_t *loop = NULL; ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    struct probe_ctx ctx = {0}; mt_tproxy_t *p = fixture_proxy(loop, &ctx);
    p->config.max_connections = 1;
    p->listener4 = local_listener(&p->config.port); ASSERT(p->listener4 >= 0);
    ASSERT_EQ(MT_OK, listeners_update(p)); ASSERT(p->listener4_watched);
    int client[2], server[2]; ASSERT_EQ(0, make_pair(client)); ASSERT_EQ(0, make_pair(server));
    tproxy_flow_t *f = fixture_flow(p, client[0], server[0], false);
    ASSERT_EQ(MT_OK, listeners_update(p)); ASSERT(!p->listener4_watched); ASSERT_EQ(1u, p->stats.over_limit);
    ASSERT_EQ(MT_OK, listeners_update(p)); ASSERT_EQ(1u, p->stats.over_limit);
    flow_free(f, false); ASSERT(p->listener4_watched); ASSERT_EQ(0u, p->stats.active);
    mt_tproxy_destroy(p); close(client[1]); close(server[1]); mt_loop_destroy(loop);
    PASS();
}

TEST authorization_and_failed_attempts_leave_no_flows(void)
{
    mt_loop_t *loop = NULL; ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    struct probe_ctx ctx = {.reject = true}; mt_tproxy_t *p = fixture_proxy(loop, &ctx);
    mt_tproxy_endpoint_t client = {.family = AF_INET, .addr = {192,168,1,5}, .port = 30000};
    mt_tproxy_endpoint_t destination = {.family = AF_INET, .addr = {127,0,0,1}, .port = 443};
    int fds[2]; ASSERT_EQ(0, make_pair(fds));
    accept_flow(p, fds[0], &client, &destination);
    ASSERT_EQ(0u, p->stats.active); ASSERT_EQ(0u, ctx.direct_prepares); ASSERT_EQ(1u, p->stats.failures);
    close(fds[1]); ctx.reject = false; ctx.reject_all = true;
    ASSERT_EQ(0, make_pair(fds)); accept_flow(p, fds[0], &client, &destination);
    ASSERT_EQ(1u, ctx.direct_prepares); ASSERT_EQ(1u, ctx.alternate_prepares);
    ASSERT_EQ(0u, p->stats.active); ASSERT(p->flows == NULL); ASSERT_EQ(2u, p->stats.failures);
    close(fds[1]); mt_tproxy_destroy(p); mt_loop_destroy(loop);
    PASS();
}

TEST resource_failure_pauses_listener_and_destruction_cancels_retry(void)
{
    mt_loop_t *loop = NULL; ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    struct probe_ctx ctx = {0}; mt_tproxy_t *p = fixture_proxy(loop, &ctx);
    p->listener4 = local_listener(&p->config.port); ASSERT(p->listener4 >= 0);
    ASSERT_EQ(MT_OK, listeners_update(p)); ASSERT(p->listener4_watched);
    listener_backoff(p); ASSERT(!p->listener4_watched); ASSERT(p->listener_retry_id > 0);
    ASSERT_EQ(MT_OK, listeners_update(p)); ASSERT(!p->listener4_watched);
    ASSERT_EQ(1u, p->stats.failures); ASSERT_EQ(0u, p->stats.over_limit);
    mt_tproxy_destroy(p);
    mt_loop_destroy(loop);
    PASS();
}

TEST client_hangup_frees_blocked_flow_and_its_deadline(void)
{
    mt_loop_t *loop = NULL; ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    struct probe_ctx ctx = {0}; mt_tproxy_t *p = fixture_proxy(loop, &ctx);
    int client[2], server[2]; ASSERT_EQ(0, make_pair(client)); ASSERT_EQ(0, make_pair(server));
    tproxy_flow_t *f = fixture_flow(p, client[0], server[0], false);
    f->to_server.len = sizeof(f->to_server.bytes);
    ASSERT(flow_update(f)); ASSERT(f->timer_id > 0);
    flow_event(loop, client[0], EPOLLHUP, f);
    ASSERT_EQ(0u, p->stats.active); ASSERT(p->flows == NULL); ASSERT_EQ(1u, p->stats.failures);
    mt_tproxy_destroy(p); close(client[1]); close(server[1]); mt_loop_destroy(loop);
    PASS();
}

struct fixture_server {
    mt_loop_t *loop;
    int listener;
    int connections[2];
    unsigned accepted;
    bool tls;
    uint8_t received[2][128];
    size_t sizes[2];
};

static void fixture_server_read(mt_loop_t *loop, int fd, uint32_t events, void *userdata)
{
    (void)events;
    struct fixture_server *s = userdata;
    unsigned index = fd == s->connections[0] ? 0u : 1u;
    ssize_t n = recv(fd, s->received[index] + s->sizes[index], sizeof(s->received[index]) - s->sizes[index], 0);
    if (n <= 0) {
        if (!n) { (void)mt_loop_del_fd(loop, fd); }
        return;
    }
    s->sizes[index] += (size_t)n;
    if (!s->tls) { (void)send(fd, "HTTP/1.1 200 OK\r\n\r\nok", 21, MSG_NOSIGNAL); }
    else if (index == 1) { (void)send(fd, server_hello, sizeof(server_hello), MSG_NOSIGNAL); }
}

static void fixture_server_accept(mt_loop_t *loop, int fd, uint32_t events, void *userdata)
{
    (void)events;
    struct fixture_server *s = userdata;
    int conn = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (conn < 0) { return; }
    if (s->accepted >= 2) { close(conn); return; }
    s->connections[s->accepted++] = conn;
    if (mt_loop_add_fd(loop, conn, EPOLLIN, fixture_server_read, s) != MT_OK) { abort(); }
}

static void watchdog(mt_loop_t *loop, void *userdata)
{
    bool *expired = userdata; *expired = true; mt_loop_stop(loop);
}

static int run_fallback(bool tls)
{
    mt_loop_t *loop = NULL; ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    struct probe_ctx ctx = {.reject_direct = !tls, .stop_on_learn = true};
    mt_tproxy_t *p = fixture_proxy(loop, &ctx);
    struct fixture_server server = {.loop = loop, .connections = {-1, -1}, .tls = tls};
    server.listener = local_listener(&fixture_port); ASSERT(server.listener >= 0);
    ASSERT_EQ(MT_OK, mt_loop_add_fd(loop, server.listener, EPOLLIN, fixture_server_accept, &server));
    int client_pair[2]; ASSERT_EQ(0, make_pair(client_pair));
    mt_tproxy_endpoint_t client = {.family = AF_INET, .addr = {192,168,1,5}, .port = 30000};
    mt_tproxy_endpoint_t destination = {.family = AF_INET, .addr = {127,0,0,1}, .port = tls ? 443 : 80};
    accept_flow(p, client_pair[0], &client, &destination);
    ASSERT_EQ(1u, p->stats.active);
    uint8_t sent[128]; size_t sent_size, hello_size = 0;
    if (tls) {
        hello_size = hello_bytes(sent, false, true);
        memcpy(sent + hello_size, application, sizeof(application)); sent_size = hello_size + sizeof(application);
    } else { memcpy(sent, "GET / HTTP/1.1\r\n\r\n", 18); sent_size = 18; }
    ASSERT_EQ((ssize_t)sent_size, send(client_pair[1], sent, sent_size, 0));
    bool expired = false; ASSERT_EQ(MT_OK, mt_loop_add_timer(loop, 2000, 0, watchdog, &expired, NULL));
    ASSERT_EQ(MT_OK, mt_loop_run(loop)); ASSERT(!expired);
    ASSERT_EQ(1u, ctx.direct_prepares); ASSERT_EQ(1u, ctx.alternate_prepares);
    ASSERT_EQ(0u, ctx.direct_learned); ASSERT_EQ(1u, ctx.alternate_learned);
    ASSERT_EQ(tls ? 2u : 1u, server.accepted);
    uint8_t received[128]; ssize_t n = recv(client_pair[1], received, sizeof(received), 0);
    ASSERT(n > 0);
    if (tls) {
        ASSERT_EQ(hello_size, server.sizes[0]); ASSERT_EQ(hello_size, server.sizes[1]);
        ASSERT_EQ(0, memcmp(server.received[0], sent, hello_size));
        ASSERT_EQ(0, memcmp(server.received[1], sent, hello_size));
        ASSERT_EQ((ssize_t)sizeof(server_hello), n); ASSERT_EQ(0, memcmp(received, server_hello, sizeof(server_hello)));
        ASSERT_EQ((ssize_t)sizeof(application), recv(server.connections[1], received, sizeof(received), 0));
        ASSERT_EQ(0, memcmp(application, received, sizeof(application)));
    } else {
        ASSERT_EQ(sent_size, server.sizes[0]); ASSERT_EQ(0, memcmp(sent, server.received[0], sent_size));
    }
    ASSERT_EQ(1u, p->stats.active); /* original browser-side socket survived retry */
    mt_tproxy_destroy(p); close(client_pair[1]); close(server.listener);
    for (size_t i = 0; i < 2; i++) { if (server.connections[i] >= 0) { close(server.connections[i]); } }
    mt_loop_destroy(loop); fixture_port = 0;
    return GREATEST_TEST_RES_PASS;
}

TEST connect_failure_retries_without_breaking_client_socket(void)
{
    CHECK_CALL(run_fallback(false)); PASS();
}

TEST silent_tls_replays_only_client_hello_over_alternative(void)
{
    CHECK_CALL(run_fallback(true)); PASS();
}

TEST invalid_creation_and_mark_errors_are_reported(void)
{
    mt_tproxy_t *p = (mt_tproxy_t *)(uintptr_t)1;
    ASSERT_EQ(MT_ERR_INVAL, mt_tproxy_create(NULL, NULL, NULL, NULL, &p)); ASSERT(p == NULL);
    ASSERT(mt_tproxy_set_mark(-1, 123) != MT_OK);
    mt_tproxy_stats_t stats = {.active = 5}; mt_tproxy_stats(NULL, &stats);
    ASSERT_EQ(0u, stats.active); ASSERT_EQ(0u, mt_tproxy_port(NULL));
    PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(client_hello_inspection_is_incremental_and_bounded);
    RUN_TEST(tls_holds_application_bytes_until_server_response);
    RUN_TEST(post_and_early_data_are_never_replayed);
    RUN_TEST(any_server_bytes_end_retry_but_alerts_do_not_teach_routes);
    RUN_TEST(fragmented_server_hello_teaches_once_after_header);
    RUN_TEST(partial_writes_preserve_every_byte_and_apply_backpressure);
    RUN_TEST(half_closes_flush_request_then_response);
    RUN_TEST(connection_limit_pauses_and_resumes_listener);
    RUN_TEST(authorization_and_failed_attempts_leave_no_flows);
    RUN_TEST(resource_failure_pauses_listener_and_destruction_cancels_retry);
    RUN_TEST(client_hangup_frees_blocked_flow_and_its_deadline);
    RUN_TEST(connect_failure_retries_without_breaking_client_socket);
    RUN_TEST(silent_tls_replays_only_client_hello_over_alternative);
    RUN_TEST(invalid_creation_and_mark_errors_are_reported);
    GREATEST_MAIN_END();
}
