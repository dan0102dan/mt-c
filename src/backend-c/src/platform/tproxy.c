/* Linux transparent TCP relay, deliberately isolated from policy ownership. */
#define _GNU_SOURCE /* NOLINT(bugprone-reserved-identifier) */

#include "magitrickle/tproxy.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef IP_TRANSPARENT
#define IP_TRANSPARENT 19
#endif
#ifndef IPV6_TRANSPARENT
#define IPV6_TRANSPARENT 75
#endif

/* Two fixed relay buffers per admitted flow; no unbounded request queues.
 * Oversized TLS ClientHellos are relayed, but cannot be replayed. */
#define MT_TPROXY_BUFFER 16384u
#define MT_TPROXY_ACCEPT_BATCH 32u

typedef enum hello_result {
    HELLO_MORE,
    HELLO_SAFE,
    HELLO_OPAQUE
} hello_result_t;

typedef enum tls_state {
    TLS_OFF,
    TLS_INSPECT,
    TLS_HELLO
} tls_state_t;

typedef struct relay_buffer {
    uint8_t bytes[MT_TPROXY_BUFFER];
    size_t len;
    size_t off;
} relay_buffer_t;

typedef struct tproxy_flow tproxy_flow_t;
struct tproxy_flow {
    mt_tproxy_t *proxy;
    tproxy_flow_t *next;
    tproxy_flow_t **prev;
    mt_tproxy_endpoint_t client;
    mt_tproxy_endpoint_t destination;
    int client_fd;
    int upstream_fd;
    bool client_watched;
    bool upstream_watched;
    uint32_t client_events;
    uint32_t upstream_events;
    bool connecting;
    bool alternate;
    bool server_seen;
    bool learned;
    bool client_eof;
    bool upstream_eof;
    bool client_write_closed;
    bool upstream_write_closed;
    bool sent_payload;
    tls_state_t tls;
    size_t hello_len;
    uint8_t server_prefix[9];
    size_t server_prefix_len;
    relay_buffer_t to_server;
    relay_buffer_t to_client;
    uint64_t active_at;
    uint64_t connect_deadline;
    uint64_t tls_deadline;
    uint64_t timer_deadline;
    int timer_id;
};

struct mt_tproxy {
    mt_loop_t *loop;
    mt_tproxy_config_t config;
    mt_tproxy_ops_t ops;
    void *ctx;
    int listener4;
    int listener6;
    bool listener4_watched;
    bool listener6_watched;
    int listener_retry_id;
    bool stopping;
    tproxy_flow_t *flows;
    mt_tproxy_stats_t stats;
};

static void flow_event(mt_loop_t *loop, int fd, uint32_t events, void *ud);
static void flow_timer(mt_loop_t *loop, void *ud);
static void listener_event(mt_loop_t *loop, int fd, uint32_t events, void *ud);
static void listener_retry(mt_loop_t *loop, void *ud);
static bool flow_update(tproxy_flow_t *f);

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) { return 0; }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint16_t read16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* Validate enough of ClientHello to rule out TLS 1.3 early_data. An unknown
 * extension is fine: only this handshake is sent, later records are held.
 * The transcript may span multiple handshake records, but no record may
 * include another handshake after ClientHello. */
static bool client_hello_safe(const uint8_t *p, size_t n)
{
    if (n < 38 || p[0] != 1 || p[4] != 3 || p[5] > 3) { return false; }
    size_t at = 38; /* handshake header + version + random */
    if (at >= n) { return false; }
    size_t session = p[at++];
    if (session > 32 || session > n - at) { return false; }
    at += session;
    if (n - at < 2) { return false; }
    size_t ciphers = read16(p + at); at += 2;
    if (!ciphers || (ciphers & 1u) || ciphers > n - at) { return false; }
    at += ciphers;
    if (at >= n) { return false; }
    size_t compression = p[at++];
    if (!compression || compression > n - at) { return false; }
    at += compression;
    if (at == n) { return true; } /* TLS 1.2 without extensions */
    if (n - at < 2) { return false; }
    size_t extensions = read16(p + at); at += 2;
    if (extensions != n - at) { return false; }
    while (at < n) {
        if (n - at < 4) { return false; }
        uint16_t kind = read16(p + at);
        size_t len = read16(p + at + 2); at += 4;
        if (kind == 42 || len > n - at) { return false; } /* early_data */
        at += len;
    }
    return true;
}

static hello_result_t inspect_hello(const uint8_t *bytes, size_t n, size_t *wire_len)
{
    uint8_t handshake[MT_TPROXY_BUFFER];
    size_t pos = 0, used = 0, need = 0;
    while (pos < n) {
        if (bytes[pos] != 22) { return HELLO_OPAQUE; }
        if (n - pos < 5) { return HELLO_MORE; }
        if (bytes[pos + 1] != 3 || bytes[pos + 2] > 3) { return HELLO_OPAQUE; }
        size_t record = read16(bytes + pos + 3);
        if (!record || record > MT_TPROXY_BUFFER || record > sizeof(handshake) - used) {
            return HELLO_OPAQUE;
        }
        if (record > n - pos - 5) { return HELLO_MORE; }
        memcpy(handshake + used, bytes + pos + 5, record);
        used += record;
        pos += 5 + record;
        if (used >= 4 && !need) {
            if (handshake[0] != 1) { return HELLO_OPAQUE; }
            need = 4u + ((size_t)handshake[1] << 16) +
                   ((size_t)handshake[2] << 8) + handshake[3];
            if (need > sizeof(handshake) || need < 42) { return HELLO_OPAQUE; }
        }
        if (need && used >= need) {
            if (used != need || !client_hello_safe(handshake, used)) { return HELLO_OPAQUE; }
            *wire_len = pos;
            return HELLO_SAFE;
        }
    }
    return HELLO_MORE;
}

static bool endpoint_from_sockaddr(const struct sockaddr_storage *sa, socklen_t len,
                                   mt_tproxy_endpoint_t *out)
{
    memset(out, 0, sizeof(*out));
    if (len < sizeof(sa->ss_family) || len > sizeof(*sa)) { return false; }
    if (sa->ss_family == AF_INET) {
        if (len < sizeof(struct sockaddr_in)) { return false; }
        const struct sockaddr_in *s = (const struct sockaddr_in *)sa;
        out->family = AF_INET;
        memcpy(out->addr, &s->sin_addr, 4);
        out->port = ntohs(s->sin_port);
        return true;
    }
    if (sa->ss_family == AF_INET6) {
        if (len < sizeof(struct sockaddr_in6)) { return false; }
        const struct sockaddr_in6 *s = (const struct sockaddr_in6 *)sa;
        if (IN6_IS_ADDR_V4MAPPED(&s->sin6_addr)) { return false; }
        out->family = AF_INET6;
        memcpy(out->addr, &s->sin6_addr, 16);
        out->port = ntohs(s->sin6_port);
        return true;
    }
    return false;
}

static socklen_t endpoint_to_sockaddr(const mt_tproxy_endpoint_t *ep,
                                      struct sockaddr_storage *sa)
{
    memset(sa, 0, sizeof(*sa));
    if (ep->family == AF_INET) {
        struct sockaddr_in *s = (struct sockaddr_in *)sa;
        s->sin_family = AF_INET; s->sin_port = htons(ep->port);
        memcpy(&s->sin_addr, ep->addr, 4);
        return sizeof(*s);
    }
    struct sockaddr_in6 *s = (struct sockaddr_in6 *)sa;
    s->sin6_family = AF_INET6; s->sin6_port = htons(ep->port);
    memcpy(&s->sin6_addr, ep->addr, 16);
    return sizeof(*s);
}

static void close_watched(mt_loop_t *loop, int *fd, bool *watched)
{
    if (*fd < 0) { return; }
    if (*watched) { (void)mt_loop_del_fd(loop, *fd); *watched = false; }
    close(*fd); *fd = -1;
}

static mt_err_t set_watch(mt_loop_t *loop, int fd, bool *watched, uint32_t *previous,
                          uint32_t events, mt_fd_cb cb, void *ud)
{
    if (!events) {
        if (*watched) { (void)mt_loop_del_fd(loop, fd); *watched = false; }
        *previous = 0;
        return MT_OK;
    }
    mt_err_t err = MT_OK;
    if (!*watched) { err = mt_loop_add_fd(loop, fd, events, cb, ud); }
    else if (*previous != events) { err = mt_loop_mod_fd(loop, fd, events); }
    if (err == MT_OK) { *watched = true; *previous = events; }
    return err;
}

static mt_err_t listeners_update(mt_tproxy_t *p)
{
    bool at_limit = p->stats.active >= p->config.max_connections;
    bool enable = !p->stopping && !p->listener_retry_id && !at_limit;
    if (at_limit && !p->stopping && (p->listener4_watched || p->listener6_watched)) {
        p->stats.over_limit++;
    }
    int fds[] = {p->listener4, p->listener6};
    bool *watched[] = {&p->listener4_watched, &p->listener6_watched};
    for (size_t i = 0; i < 2; i++) {
        if (fds[i] < 0) { continue; }
        if (enable && !*watched[i]) {
            mt_err_t err = mt_loop_add_fd(p->loop, fds[i], EPOLLIN, listener_event, p);
            if (err != MT_OK) { return err; }
            *watched[i] = true;
        } else if (!enable && *watched[i]) {
            (void)mt_loop_del_fd(p->loop, fds[i]); *watched[i] = false;
        }
    }
    return MT_OK;
}

static void listener_retry(mt_loop_t *loop, void *ud)
{
    (void)loop;
    mt_tproxy_t *p = ud;
    p->listener_retry_id = 0;
    (void)listeners_update(p);
}

static void listener_backoff(mt_tproxy_t *p)
{
    /* A readable backlog plus EMFILE/ENFILE/ENOMEM would otherwise make
     * epoll spin. Stop admission and retry later. Failure to allocate even a
     * timer remains fail-closed until an active flow frees resources. */
    p->stats.failures++;
    if (!p->listener_retry_id) {
        (void)mt_loop_add_reusable_timer(p->loop, 100, 0, listener_retry, p,
                                         &p->listener_retry_id);
    }
    if (p->listener4_watched) {
        (void)mt_loop_del_fd(p->loop, p->listener4); p->listener4_watched = false;
    }
    if (p->listener6_watched) {
        (void)mt_loop_del_fd(p->loop, p->listener6); p->listener6_watched = false;
    }
}

/* The caller removes the flow from its owner before releasing its resources. */
static void flow_release(tproxy_flow_t *f)
{
    mt_tproxy_t *p = f->proxy;
    if (f->timer_id) { (void)mt_loop_del_timer(p->loop, f->timer_id); }
    close_watched(p->loop, &f->client_fd, &f->client_watched);
    close_watched(p->loop, &f->upstream_fd, &f->upstream_watched);
    free(f);
}

static void flow_free(tproxy_flow_t *f, bool failed)
{
    mt_tproxy_t *p = f->proxy;
    if (failed) { p->stats.failures++; }
    *f->prev = f->next;
    if (f->next) { f->next->prev = f->prev; }
    p->stats.active--;
    flow_release(f);
    (void)listeners_update(p);
}

static void buffer_compact(relay_buffer_t *b)
{
    if (!b->off) { return; }
    memmove(b->bytes, b->bytes + b->off, b->len - b->off);
    b->len -= b->off; b->off = 0;
}

static size_t upstream_pending(const tproxy_flow_t *f)
{
    if (f->tls == TLS_INSPECT) { return 0; }
    size_t end = f->tls == TLS_HELLO ? f->hello_len : f->to_server.len;
    return end - f->to_server.off;
}

static mt_err_t start_upstream(tproxy_flow_t *f)
{
    mt_tproxy_t *p = f->proxy;
    f->upstream_fd = socket(f->destination.family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (f->upstream_fd < 0) { return mt_err_from_errno(errno); }
    mt_err_t err = p->ops.prepare(p->ctx, f->upstream_fd, &f->client, &f->destination, f->alternate);
    if (err != MT_OK) { close_watched(p->loop, &f->upstream_fd, &f->upstream_watched); return err; }
    struct sockaddr_storage sa;
    socklen_t len = endpoint_to_sockaddr(&f->destination, &sa);
    int result = connect(f->upstream_fd, (const struct sockaddr *)&sa, len);
    if (result != 0 && errno != EINPROGRESS) {
        err = mt_err_from_errno(errno);
        close_watched(p->loop, &f->upstream_fd, &f->upstream_watched);
        return err;
    }
    f->connecting = result != 0;
    f->connect_deadline = monotonic_ms() + p->config.connect_timeout_ms;
    f->tls_deadline = 0;
    f->upstream_eof = false;
    f->upstream_write_closed = false;
    f->upstream_events = 0;
    return MT_OK;
}

static bool can_retry(const tproxy_flow_t *f)
{
    return !f->alternate && !f->server_seen &&
           (!f->sent_payload || f->tls == TLS_HELLO);
}

static bool retry_upstream(tproxy_flow_t *f)
{
    if (!can_retry(f)) { return false; }
    close_watched(f->proxy->loop, &f->upstream_fd, &f->upstream_watched);
    f->alternate = true;
    /* Only inspected ClientHello bytes may have left this buffer. Ordinary
     * application writes make can_retry() false, even if the peer stayed silent. */
    if (f->tls == TLS_HELLO) { f->to_server.off = 0; }
    f->sent_payload = false;
    return start_upstream(f) == MT_OK && f->upstream_fd >= 0;
}

static bool flow_deadline(tproxy_flow_t *f)
{
    uint64_t due = f->active_at + f->proxy->config.idle_timeout_ms;
    if (f->connecting && f->connect_deadline < due) { due = f->connect_deadline; }
    if (f->tls_deadline && f->tls_deadline < due) { due = f->tls_deadline; }
    if (f->timer_id && f->timer_deadline <= due) { return true; }
    if (f->timer_id) { (void)mt_loop_del_timer(f->proxy->loop, f->timer_id); f->timer_id = 0; }
    uint64_t now = monotonic_ms();
    f->timer_deadline = due;
    return mt_loop_add_reusable_timer(f->proxy->loop, due > now ? due - now : 1,
                                      0, flow_timer, f, &f->timer_id) == MT_OK;
}

/* Returns false only after freeing f. */
static bool flow_update(tproxy_flow_t *f)
{
    if (f->tls == TLS_OFF) { buffer_compact(&f->to_server); }
    buffer_compact(&f->to_client);
    if (f->client_eof && f->upstream_eof && !upstream_pending(f) && !f->to_client.len) {
        flow_free(f, !f->server_seen); return false;
    }
    uint32_t ce = 0, ue = 0;
    if (!f->client_eof && f->to_server.len < sizeof(f->to_server.bytes)) { ce |= EPOLLIN | EPOLLRDHUP; }
    if (f->to_client.len) { ce |= EPOLLOUT; }
    if (f->connecting) { ue = EPOLLOUT; }
    else {
        if (!f->upstream_eof && f->to_client.len < sizeof(f->to_client.bytes)) { ue |= EPOLLIN | EPOLLRDHUP; }
        if (upstream_pending(f)) { ue |= EPOLLOUT; }
    }
    if (set_watch(f->proxy->loop, f->client_fd, &f->client_watched, &f->client_events,
                  ce, flow_event, f) != MT_OK ||
        set_watch(f->proxy->loop, f->upstream_fd, &f->upstream_watched, &f->upstream_events,
                  ue, flow_event, f) != MT_OK || !flow_deadline(f)) {
        flow_free(f, true); return false;
    }
    return true;
}

static void learned_response(tproxy_flow_t *f, const uint8_t *bytes, size_t n)
{
    if (f->learned) { return; }
    size_t copy = sizeof(f->server_prefix) - f->server_prefix_len;
    if (copy > n) { copy = n; }
    memcpy(f->server_prefix + f->server_prefix_len, bytes, copy);
    f->server_prefix_len += copy;
    if (f->destination.port == 443) {
        if (f->server_prefix_len < sizeof(f->server_prefix)) { return; }
        const uint8_t *b = f->server_prefix;
        /* A TLS alert, arbitrary block page or incomplete response does not
         * teach an alternative route. Certificate validation stays at client. */
        size_t hello_size = ((size_t)b[6] << 16) | ((size_t)b[7] << 8) | b[8];
        if (b[0] != 22 || b[1] != 3 || b[2] > 3 || read16(b + 3) < 4 ||
            b[5] != 2 || hello_size < 38 || hello_size > 65535) { return; }
    }
    f->learned = true;
    if (f->alternate) { f->proxy->stats.alternate_success++; }
    else { f->proxy->stats.direct_success++; }
    if (f->proxy->ops.learned) {
        f->proxy->ops.learned(f->proxy->ctx, &f->client, &f->destination, f->alternate);
    }
}

static bool read_client(tproxy_flow_t *f)
{
    relay_buffer_t *b = &f->to_server;
    if (f->tls == TLS_OFF) { buffer_compact(b); }
    if (f->client_eof || b->len == sizeof(b->bytes)) { return true; }
    ssize_t n = recv(f->client_fd, b->bytes + b->len, sizeof(b->bytes) - b->len, 0);
    if (n < 0) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
    if (!n) { f->client_eof = true; }
    else { b->len += (size_t)n; f->active_at = monotonic_ms(); }
    if (f->tls == TLS_INSPECT) {
        hello_result_t result = inspect_hello(b->bytes, b->len, &f->hello_len);
        if (result == HELLO_SAFE) { f->tls = TLS_HELLO; }
        else if (result == HELLO_OPAQUE || b->len == sizeof(b->bytes) || f->client_eof) {
            f->tls = TLS_OFF;
        }
    }
    return true;
}

static bool read_upstream(tproxy_flow_t *f)
{
    relay_buffer_t *b = &f->to_client;
    buffer_compact(b);
    if (f->upstream_eof || b->len == sizeof(b->bytes)) { return true; }
    ssize_t n = recv(f->upstream_fd, b->bytes + b->len, sizeof(b->bytes) - b->len, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) { return true; }
        return retry_upstream(f);
    }
    if (!n) {
        if (!f->server_seen && can_retry(f)) { return retry_upstream(f); }
        f->upstream_eof = true;
        return true;
    }
    f->server_seen = true; /* prohibits all retries, even before client write */
    f->tls = TLS_OFF;
    f->tls_deadline = 0;
    buffer_compact(&f->to_server);
    learned_response(f, b->bytes + b->len, (size_t)n);
    b->len += (size_t)n; f->active_at = monotonic_ms();
    return true;
}

static bool write_upstream(tproxy_flow_t *f)
{
    size_t pending = upstream_pending(f);
    if (f->connecting || !pending || f->upstream_write_closed) { return true; }
    ssize_t n = send(f->upstream_fd, f->to_server.bytes + f->to_server.off, pending, MSG_NOSIGNAL);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) { return true; }
        return retry_upstream(f);
    }
    if (!n) { return false; }
    f->sent_payload = true;
    f->to_server.off += (size_t)n;
    f->active_at = monotonic_ms();
    if (f->tls == TLS_HELLO && f->to_server.off == f->hello_len && !f->tls_deadline) {
        f->tls_deadline = f->active_at + f->proxy->config.tls_timeout_ms;
    }
    return true;
}

static bool write_client(tproxy_flow_t *f)
{
    relay_buffer_t *b = &f->to_client;
    if (b->len == b->off || f->client_write_closed) { return true; }
    ssize_t n = send(f->client_fd, b->bytes + b->off, b->len - b->off, MSG_NOSIGNAL);
    if (n < 0) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
    if (!n) { return false; }
    b->off += (size_t)n; f->active_at = monotonic_ms();
    return true;
}

static bool propagate_half_closes(tproxy_flow_t *f)
{
    if (f->client_eof && !f->connecting && !f->upstream_write_closed &&
        f->to_server.off == f->to_server.len) {
        if (shutdown(f->upstream_fd, SHUT_WR) != 0 && errno != ENOTCONN) { return false; }
        f->upstream_write_closed = true;
    }
    if (f->upstream_eof && !f->client_write_closed && f->to_client.off == f->to_client.len) {
        if (shutdown(f->client_fd, SHUT_WR) != 0 && errno != ENOTCONN) { return false; }
        f->client_write_closed = true;
    }
    return true;
}

static void flow_event(mt_loop_t *loop, int fd, uint32_t events, void *ud)
{
    (void)loop;
    tproxy_flow_t *f = ud;
    bool from_client = fd == f->client_fd;
    if (from_client && (events & (EPOLLERR | EPOLLHUP))) {
        /* RDHUP is the supported half-close. HUP means the client has gone;
         * retaining its watch with full buffers would busy-loop on HUP. */
        flow_free(f, !f->server_seen); return;
    }
    if (!from_client && f->connecting) {
        int error = 0; socklen_t len = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) != 0 || error) {
            if (!retry_upstream(f)) { flow_free(f, true); return; }
            (void)flow_update(f); return;
        }
        f->connecting = false;
    }
    bool ok = true;
    if (from_client && (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP))) { ok = read_client(f); }
    else if (!from_client && !f->connecting && (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR))) {
        ok = read_upstream(f);
    }
    if (ok) { ok = write_upstream(f); }
    if (ok) { ok = write_client(f); }
    if (ok) { ok = propagate_half_closes(f); }
    if (!ok) { flow_free(f, !f->server_seen); return; }
    (void)flow_update(f);
}

static void flow_timer(mt_loop_t *loop, void *ud)
{
    (void)loop;
    tproxy_flow_t *f = ud;
    f->timer_id = 0;
    uint64_t now = monotonic_ms();
    if ((f->connecting && now >= f->connect_deadline) ||
        (f->tls_deadline && now >= f->tls_deadline)) {
        if (!retry_upstream(f)) { flow_free(f, !f->server_seen); return; }
    } else if (now >= f->active_at + f->proxy->config.idle_timeout_ms) {
        flow_free(f, !f->server_seen); return;
    }
    (void)flow_update(f);
}

/* Takes client_fd on every path. Kept private so unprivileged socket tests can
 * exercise the relay without weakening IP_TRANSPARENT in production. */
static void accept_flow(mt_tproxy_t *p, int client_fd,
                         const mt_tproxy_endpoint_t *client,
                         const mt_tproxy_endpoint_t *destination)
{
    if (p->stats.active >= p->config.max_connections ||
        !p->ops.authorize(p->ctx, client, destination)) {
        close(client_fd); p->stats.failures++; return;
    }
    tproxy_flow_t *f = calloc(1, sizeof(*f));
    if (!f) { close(client_fd); p->stats.failures++; return; }
    f->proxy = p; f->client = *client; f->destination = *destination;
    f->client_fd = client_fd; f->upstream_fd = -1;
    f->tls = destination->port == 443 ? TLS_INSPECT : TLS_OFF;
    f->active_at = monotonic_ms();
    f->next = p->flows; f->prev = &p->flows;
    if (f->next) { f->next->prev = &f->next; }
    p->flows = f;
    p->stats.active++; p->stats.accepted++;
    if (start_upstream(f) != MT_OK && !retry_upstream(f)) { flow_free(f, true); return; }
    if (flow_update(f)) { (void)listeners_update(p); }
}

static void listener_event(mt_loop_t *loop, int fd, uint32_t events, void *ud)
{
    (void)loop; (void)events;
    mt_tproxy_t *p = ud;
    for (unsigned i = 0; i < MT_TPROXY_ACCEPT_BATCH; i++) {
        if (p->stats.active >= p->config.max_connections) { (void)listeners_update(p); return; }
        struct sockaddr_storage peer = {0}, destination = {0};
        socklen_t peer_len = sizeof(peer), destination_len = sizeof(destination);
        int client = accept4(fd, (struct sockaddr *)&peer, &peer_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR) { continue; }
            if (errno != EAGAIN && errno != EWOULDBLOCK) { listener_backoff(p); }
            return;
        }
        mt_tproxy_endpoint_t cep, dep;
        if (getsockname(client, (struct sockaddr *)&destination, &destination_len) != 0 ||
            !endpoint_from_sockaddr(&peer, peer_len, &cep) ||
            !endpoint_from_sockaddr(&destination, destination_len, &dep) ||
            cep.family != dep.family) {
            close(client); p->stats.failures++; continue;
        }
        accept_flow(p, client, &cep, &dep);
    }
}

static mt_err_t bind_listener(mt_tproxy_t *p, int family, int *out)
{
    int fd = socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { return mt_err_from_errno(errno); }
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0 ||
        (family == AF_INET6 && setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one)) != 0) ||
        setsockopt(fd, family == AF_INET ? SOL_IP : SOL_IPV6,
                   family == AF_INET ? IP_TRANSPARENT : IPV6_TRANSPARENT, &one, sizeof(one)) != 0) {
        mt_err_t err = mt_err_from_errno(errno); close(fd); return err;
    }
    mt_tproxy_endpoint_t endpoint = {.family = family, .port = p->config.port};
    struct sockaddr_storage sa;
    socklen_t len = endpoint_to_sockaddr(&endpoint, &sa);
    if (bind(fd, (struct sockaddr *)&sa, len) != 0 || listen(fd, 128) != 0) {
        mt_err_t err = mt_err_from_errno(errno); close(fd); return err;
    }
    if (!p->config.port) {
        len = sizeof(sa);
        if (getsockname(fd, (struct sockaddr *)&sa, &len) != 0 ||
            !endpoint_from_sockaddr(&sa, len, &endpoint)) {
            close(fd); return MT_ERR_IO;
        }
        p->config.port = endpoint.port;
    }
    *out = fd;
    return MT_OK;
}

mt_err_t mt_tproxy_create(mt_loop_t *loop, const mt_tproxy_config_t *config,
                          const mt_tproxy_ops_t *ops, void *ctx, mt_tproxy_t **out)
{
    if (out) { *out = NULL; }
    if (!loop || !config || !ops || !ops->authorize || !ops->prepare || !out ||
        (!config->ipv4 && !config->ipv6) || !config->connect_timeout_ms ||
        !config->tls_timeout_ms || !config->idle_timeout_ms || !config->max_connections ||
        config->max_connections > 4096) { return MT_ERR_INVAL; }
    mt_tproxy_t *p = calloc(1, sizeof(*p));
    if (!p) { return MT_ERR_NOMEM; }
    p->loop = loop; p->config = *config; p->ops = *ops; p->ctx = ctx;
    p->listener4 = p->listener6 = -1;
    mt_err_t err = config->ipv4 ? bind_listener(p, AF_INET, &p->listener4) : MT_OK;
    if (err == MT_OK && config->ipv6) { err = bind_listener(p, AF_INET6, &p->listener6); }
    if (err == MT_OK) { err = listeners_update(p); }
    if (err != MT_OK) { mt_tproxy_destroy(p); return err; }
    *out = p;
    return MT_OK;
}

void mt_tproxy_destroy(mt_tproxy_t *p)
{
    if (!p) { return; }
    p->stopping = true;
    if (p->listener_retry_id) { (void)mt_loop_del_timer(p->loop, p->listener_retry_id); }
    close_watched(p->loop, &p->listener4, &p->listener4_watched);
    close_watched(p->loop, &p->listener6, &p->listener6_watched);
    while (p->flows) {
        tproxy_flow_t *f = p->flows;
        p->flows = f->next;
        if (p->flows) { p->flows->prev = &p->flows; }
        flow_release(f);
    }
    free(p);
}

uint16_t mt_tproxy_port(const mt_tproxy_t *p)
{
    return p ? p->config.port : 0;
}

void mt_tproxy_stats(const mt_tproxy_t *p, mt_tproxy_stats_t *out)
{
    if (out) { *out = p ? p->stats : (mt_tproxy_stats_t){0}; }
}

mt_err_t mt_tproxy_set_mark(int fd, uint32_t mark)
{
    if (setsockopt(fd, SOL_SOCKET, SO_MARK, &mark, sizeof(mark)) != 0) {
        return mt_err_from_errno(errno);
    }
    return MT_OK;
}
