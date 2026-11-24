/* Linux UDP transparent relay. QUIC remains encrypted end to end; only its
 * public packet framing is inspected. Policy and pin ownership live outside
 * this module. All state belongs to the event-loop thread. */
#define _GNU_SOURCE /* NOLINT(bugprone-reserved-identifier) */

#include "magitrickle/udp_tproxy.h"
#include "magitrickle/quic.h"

#include <arpa/inet.h>
#include <errno.h>
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
#ifndef IP_RECVORIGDSTADDR
#define IP_RECVORIGDSTADDR 20
#endif
#ifndef IPV6_RECVORIGDSTADDR
#define IPV6_RECVORIGDSTADDR 74
#endif

#define UDP_DATAGRAM_MAX 65535u
#define UDP_REPLAY_BYTES 16384u
#define UDP_QUEUE_BYTES 65535u
#define UDP_QUEUE_PACKETS 16u
#define UDP_BATCH 32u

typedef struct udp_packet udp_packet_t;
struct udp_packet {
    udp_packet_t *next;
    size_t length;
    uint8_t bytes[];
};

typedef struct udp_queue {
    udp_packet_t *head;
    udp_packet_t *tail;
    size_t bytes;
    unsigned count;
} udp_queue_t;

typedef struct udp_flow udp_flow_t;
struct udp_flow {
    mt_udp_tproxy_t *proxy;
    udp_flow_t *next;
    udp_flow_t **prev;
    mt_tproxy_endpoint_t client;
    mt_tproxy_endpoint_t destination;
    int client_fd;
    int upstream_fd;
    bool client_watched;
    bool upstream_watched;
    uint32_t client_events;
    uint32_t upstream_events;
    uint32_t pin_token;
    bool alternate;
    bool server_seen;
    bool retry_eligible;
    bool initial_seen;
    bool learned;
    unsigned progress;
    mt_quic_packet_t initial;
    uint8_t server_cid[MT_QUIC_MAX_CID];
    uint8_t server_cid_len;
    udp_queue_t replay;
    udp_queue_t to_server;
    udp_queue_t to_client;
    uint64_t active_at;
    uint64_t retry_deadline;
    uint64_t timer_deadline;
    int timer_id;
};

struct mt_udp_tproxy {
    mt_loop_t *loop;
    mt_udp_tproxy_config_t config;
    mt_udp_tproxy_ops_t ops;
    void *ctx;
    int listener4;
    int listener6;
    bool listener4_watched;
    bool listener6_watched;
    int listener_retry_id;
    bool stopping;
    udp_flow_t *flows;
    mt_tproxy_stats_t stats;
    /* Callbacks are not reentrant; one full-size receive buffer is enough
     * for every flow. Neither truncated UDP payloads nor partial writes are
     * forwarded. The two pending queues and replay queue are bounded. */
    uint8_t scratch[UDP_DATAGRAM_MAX];
};

static void udp_listener_event(mt_loop_t *loop, int fd, uint32_t events, void *ud);
static void udp_flow_event(mt_loop_t *loop, int fd, uint32_t events, void *ud);
static void udp_flow_timer(mt_loop_t *loop, void *ud);
static bool udp_flow_update(udp_flow_t *f);
static mt_err_t udp_listeners_update(mt_udp_tproxy_t *p);

static uint64_t udp_now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) { return 0; }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static socklen_t udp_sockaddr(const mt_tproxy_endpoint_t *endpoint,
                              struct sockaddr_storage *out)
{
    memset(out, 0, sizeof(*out));
    if (endpoint->family == AF_INET) {
        struct sockaddr_in *sa = (struct sockaddr_in *)out;
        sa->sin_family = AF_INET; sa->sin_port = htons(endpoint->port);
        memcpy(&sa->sin_addr, endpoint->addr, 4);
        return sizeof(*sa);
    }
    struct sockaddr_in6 *sa = (struct sockaddr_in6 *)out;
    sa->sin6_family = AF_INET6; sa->sin6_port = htons(endpoint->port);
    memcpy(&sa->sin6_addr, endpoint->addr, 16);
    return sizeof(*sa);
}

static bool udp_endpoint(const struct sockaddr_storage *sa, socklen_t length,
                          mt_tproxy_endpoint_t *out)
{
    memset(out, 0, sizeof(*out));
    if (length < sizeof(sa->ss_family) || length > sizeof(*sa)) { return false; }
    if (sa->ss_family == AF_INET && length >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *address = (const struct sockaddr_in *)sa;
        out->family = AF_INET; out->port = ntohs(address->sin_port);
        memcpy(out->addr, &address->sin_addr, 4);
        return true;
    }
    if (sa->ss_family == AF_INET6 && length >= sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *address = (const struct sockaddr_in6 *)sa;
        if (IN6_IS_ADDR_V4MAPPED(&address->sin6_addr) || address->sin6_scope_id) { return false; }
        out->family = AF_INET6; out->port = ntohs(address->sin6_port);
        memcpy(out->addr, &address->sin6_addr, 16);
        return true;
    }
    return false;
}

static bool udp_endpoint_equal(const mt_tproxy_endpoint_t *a, const mt_tproxy_endpoint_t *b)
{
    return a->family == b->family && a->port == b->port &&
        memcmp(a->addr, b->addr, a->family == AF_INET ? 4u : 16u) == 0;
}

static bool udp_cid_equal(const uint8_t *a, uint8_t alen, const uint8_t *b, uint8_t blen)
{
    return alen == blen && memcmp(a, b, alen) == 0;
}

static void udp_queue_clear(udp_queue_t *q)
{
    while (q->head) {
        udp_packet_t *packet = q->head;
        q->head = packet->next;
        free(packet);
    }
    memset(q, 0, sizeof(*q));
}

static bool udp_queue_push(udp_queue_t *q, const uint8_t *bytes, size_t length,
                           size_t maximum_bytes)
{
    if (q->count >= UDP_QUEUE_PACKETS || length > maximum_bytes ||
        q->bytes > maximum_bytes - length) { return false; }
    udp_packet_t *packet = malloc(sizeof(*packet) + length);
    if (!packet) { return false; }
    packet->next = NULL; packet->length = length;
    if (length) { memcpy(packet->bytes, bytes, length); }
    if (q->tail) { q->tail->next = packet; } else { q->head = packet; }
    q->tail = packet; q->bytes += length; q->count++;
    return true;
}

static void udp_close(mt_loop_t *loop, int *fd, bool *watched)
{
    if (*fd < 0) { return; }
    if (*watched) { (void)mt_loop_del_fd(loop, *fd); *watched = false; }
    close(*fd); *fd = -1;
}

static void udp_flow_release(udp_flow_t *f)
{
    mt_udp_tproxy_t *p = f->proxy;
    if (f->timer_id) { (void)mt_loop_del_timer(p->loop, f->timer_id); }
    /* A stale kernel pin is inert as soon as this connected transparent
     * socket is closed. Retire its ownership token only afterwards. */
    udp_close(p->loop, &f->client_fd, &f->client_watched);
    udp_close(p->loop, &f->upstream_fd, &f->upstream_watched);
    if (f->pin_token) { p->ops.unpin(p->ctx, f->pin_token); }
    udp_queue_clear(&f->replay);
    udp_queue_clear(&f->to_server);
    udp_queue_clear(&f->to_client);
    free(f);
}

static void udp_flow_free(udp_flow_t *f, bool failed)
{
    mt_udp_tproxy_t *p = f->proxy;
    if (failed) { p->stats.failures++; }
    *f->prev = f->next;
    if (f->next) { f->next->prev = f->prev; }
    p->stats.active--;
    udp_flow_release(f);
    (void)udp_listeners_update(p);
}

static void udp_no_retry(udp_flow_t *f)
{
    f->retry_eligible = false;
    f->retry_deadline = 0;
    udp_queue_clear(&f->replay);
}

static void udp_inspect_client(udp_flow_t *f, const uint8_t *bytes, size_t length)
{
    mt_quic_datagram_t datagram;
    bool parsed = mt_quic_parse(bytes, length, &datagram);
    if (parsed && !f->initial_seen && !f->server_seen && datagram.count &&
        datagram.packets[0].type == MT_QUIC_INITIAL) {
        f->initial = datagram.packets[0]; f->initial_seen = true;
    }
    if (!f->retry_eligible) { return; }
    if (!parsed || !datagram.initial_only || !f->initial_seen) { udp_no_retry(f); return; }
    const mt_quic_packet_t *first = &datagram.packets[0];
    if (first->version != f->initial.version ||
        !udp_cid_equal(first->dcid, first->dcid_len, f->initial.dcid, f->initial.dcid_len) ||
        !udp_cid_equal(first->scid, first->scid_len, f->initial.scid, f->initial.scid_len) ||
        !udp_queue_push(&f->replay, bytes, length, UDP_REPLAY_BYTES)) { udp_no_retry(f); }
}

/* Record client progress only after this datagram has actually been sent,
 * not merely enqueued behind EAGAIN. Initial decryption is intentionally not
 * attempted: this is a reachability heuristic, never TLS authentication. */
static void udp_client_sent(udp_flow_t *f, const uint8_t *bytes, size_t length)
{
    if (f->progress != 1 || !f->initial_seen || f->learned) { return; }
    mt_quic_datagram_t datagram;
    if (!mt_quic_parse(bytes, length, &datagram)) { return; }
    for (size_t i = 0; i < datagram.count; i++) {
        const mt_quic_packet_t *packet = &datagram.packets[i];
        if (packet->type == MT_QUIC_HANDSHAKE && packet->version == f->initial.version &&
            udp_cid_equal(packet->dcid, packet->dcid_len, f->server_cid, f->server_cid_len) &&
            udp_cid_equal(packet->scid, packet->scid_len, f->initial.scid, f->initial.scid_len)) {
            f->progress = 2; return;
        }
    }
}

static void udp_server_received(udp_flow_t *f, const uint8_t *bytes, size_t length)
{
    f->server_seen = true;
    udp_no_retry(f);
    if (!f->initial_seen || f->learned) { return; }
    mt_quic_datagram_t datagram;
    if (!mt_quic_parse(bytes, length, &datagram)) { return; }
    for (size_t i = 0; i < datagram.count; i++) {
        const mt_quic_packet_t *packet = &datagram.packets[i];
        bool long_match = (packet->type == MT_QUIC_INITIAL || packet->type == MT_QUIC_HANDSHAKE) &&
            packet->version == f->initial.version && !packet->has_token &&
            udp_cid_equal(packet->dcid, packet->dcid_len, f->initial.scid, f->initial.scid_len);
        if (!f->progress && long_match) {
            f->server_cid_len = packet->scid_len;
            memcpy(f->server_cid, packet->scid, packet->scid_len);
            f->progress = 1;
        } else if (f->progress == 2) {
            bool handshake = long_match && packet->type == MT_QUIC_HANDSHAKE &&
                udp_cid_equal(packet->scid, packet->scid_len, f->server_cid, f->server_cid_len);
            bool short_header = packet->type == MT_QUIC_SHORT &&
                packet->length >= 1u + f->initial.scid_len + 20u &&
                memcmp(bytes + packet->offset + 1, f->initial.scid, f->initial.scid_len) == 0;
            if (handshake || short_header) {
                f->learned = true;
                if (f->alternate) { f->proxy->stats.alternate_success++; }
                else { f->proxy->stats.direct_success++; }
                if (f->proxy->ops.learned) {
                    f->proxy->ops.learned(f->proxy->ctx, &f->client, &f->destination, f->alternate);
                }
                return;
            }
        }
    }
}

static mt_err_t udp_send(int fd, const uint8_t *bytes, size_t length)
{
    if (fd < 0) { return MT_ERR_STATE; }
    ssize_t n = send(fd, bytes, length, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) { return MT_ERR_AGAIN; }
        return MT_ERR_IO;
    }
    return (size_t)n == length ? MT_OK : MT_ERR_IO;
}

static bool udp_flush(udp_flow_t *f, bool upstream)
{
    udp_queue_t *q = upstream ? &f->to_server : &f->to_client;
    int fd = upstream ? f->upstream_fd : f->client_fd;
    for (unsigned i = 0; q->head && i < UDP_BATCH; i++) {
        udp_packet_t *packet = q->head;
        mt_err_t err = udp_send(fd, packet->bytes, packet->length);
        if (err == MT_ERR_AGAIN) { return true; }
        if (err != MT_OK) { return false; }
        if (upstream) { udp_client_sent(f, packet->bytes, packet->length); }
        f->active_at = udp_now();
        q->head = packet->next;
        q->bytes -= packet->length; q->count--;
        if (!q->head) { q->tail = NULL; }
        free(packet);
    }
    return true;
}

static bool udp_forward(udp_flow_t *f, bool upstream, const uint8_t *bytes, size_t length)
{
    udp_queue_t *q = upstream ? &f->to_server : &f->to_client;
    if (!q->head) {
        mt_err_t err = udp_send(upstream ? f->upstream_fd : f->client_fd, bytes, length);
        if (err == MT_OK) {
            if (upstream) { udp_client_sent(f, bytes, length); }
            return true;
        }
        if (err != MT_ERR_AGAIN) { return false; }
    }
    if (!udp_queue_push(q, bytes, length, UDP_QUEUE_BYTES)) {
        /* UDP backpressure is bounded loss. Never send a truncated datagram
         * or allocate an unbounded queue, and never retry after losing part
         * of the inspected initial flight locally. */
        f->proxy->stats.failures++;
        udp_no_retry(f);
    }
    return true;
}

static mt_err_t udp_start_upstream(udp_flow_t *f)
{
    f->upstream_fd = socket(f->destination.family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (f->upstream_fd < 0) { return MT_ERR_IO; }
    mt_udp_tproxy_t *p = f->proxy;
    mt_err_t err = p->ops.prepare(p->ctx, f->upstream_fd, &f->client, &f->destination, f->alternate);
    if (err == MT_OK) {
        struct sockaddr_storage sa;
        socklen_t length = udp_sockaddr(&f->destination, &sa);
        if (connect(f->upstream_fd, (const struct sockaddr *)&sa, length) != 0) { err = MT_ERR_IO; }
    }
    if (err != MT_OK) { udp_close(p->loop, &f->upstream_fd, &f->upstream_watched); }
    return err;
}

static bool udp_retry(udp_flow_t *f)
{
    if (f->alternate || f->server_seen || !f->retry_eligible || !f->replay.head) { return false; }
    if (f->upstream_fd >= 0) {
        /* Every retry cause passes here, including send/receive errors. An
         * asynchronous socket error can precede an already queued response;
         * consume a bounded number of errors before deciding the queue is
         * empty. PEEK also observes an empty datagram without consuming it.
         * Pin immediately, even when client backpressure prevents reading
         * the response. A later readable event forwards it on this path. */
        for (unsigned i = 0; i < UDP_BATCH; i++) {
            uint8_t byte;
            ssize_t n = recv(f->upstream_fd, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT | MSG_TRUNC);
            if (n >= 0) {
                f->server_seen = true;
                udp_no_retry(f);
                return true;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) { break; }
            if (errno == EBADF || errno == ENOTSOCK || errno == EINVAL || i + 1 == UDP_BATCH) {
                return false;
            }
        }
    }
    udp_close(f->proxy->loop, &f->upstream_fd, &f->upstream_watched);
    f->upstream_events = 0;
    udp_queue_clear(&f->to_server);
    f->alternate = true; f->retry_eligible = false; f->retry_deadline = 0;
    f->to_server = f->replay;
    memset(&f->replay, 0, sizeof(f->replay));
    return udp_start_upstream(f) == MT_OK && f->upstream_fd >= 0 && udp_flush(f, true);
}

static bool udp_read_server(udp_flow_t *f)
{
    mt_udp_tproxy_t *p = f->proxy;
    for (unsigned i = 0; i < UDP_BATCH; i++) {
        if (f->to_client.count >= UDP_QUEUE_PACKETS || f->to_client.bytes >= UDP_QUEUE_BYTES) { return true; }
        ssize_t n = recv(f->upstream_fd, p->scratch, sizeof(p->scratch), MSG_DONTWAIT | MSG_TRUNC);
        if (n < 0) {
            if (errno == EINTR) { continue; }
            return errno == EAGAIN || errno == EWOULDBLOCK;
        }
        if ((size_t)n > sizeof(p->scratch)) { return false; }
        f->active_at = udp_now();
        udp_server_received(f, p->scratch, (size_t)n);
        if (!udp_forward(f, false, p->scratch, (size_t)n)) { return false; }
    }
    return true;
}

static bool udp_read_client(udp_flow_t *f)
{
    mt_udp_tproxy_t *p = f->proxy;
    for (unsigned i = 0; i < UDP_BATCH; i++) {
        if (f->to_server.count >= UDP_QUEUE_PACKETS || f->to_server.bytes >= UDP_QUEUE_BYTES) { return true; }
        ssize_t n = recv(f->client_fd, p->scratch, sizeof(p->scratch), MSG_DONTWAIT | MSG_TRUNC);
        if (n < 0) {
            if (errno == EINTR) { continue; }
            return errno == EAGAIN || errno == EWOULDBLOCK;
        }
        if ((size_t)n > sizeof(p->scratch)) { return false; }
        f->active_at = udp_now();
        udp_inspect_client(f, p->scratch, (size_t)n);
        if (!udp_forward(f, true, p->scratch, (size_t)n) && !udp_retry(f)) { return false; }
    }
    return true;
}

static mt_err_t udp_watch(mt_loop_t *loop, int fd, bool *watched, uint32_t *previous,
                          uint32_t events, udp_flow_t *f)
{
    if (!events) {
        if (*watched) { (void)mt_loop_del_fd(loop, fd); *watched = false; }
        *previous = 0; return MT_OK;
    }
    mt_err_t err = MT_OK;
    if (!*watched) { err = mt_loop_add_fd(loop, fd, events, udp_flow_event, f); }
    else if (*previous != events) { err = mt_loop_mod_fd(loop, fd, events); }
    if (err == MT_OK) { *watched = true; *previous = events; }
    return err;
}

static bool udp_deadline(udp_flow_t *f)
{
    uint64_t due = f->active_at + f->proxy->config.idle_timeout_ms;
    if (f->retry_deadline && f->retry_deadline < due) { due = f->retry_deadline; }
    if (f->timer_id && f->timer_deadline <= due) { return true; }
    if (f->timer_id) { (void)mt_loop_del_timer(f->proxy->loop, f->timer_id); f->timer_id = 0; }
    uint64_t now = udp_now();
    f->timer_deadline = due;
    return mt_loop_add_reusable_timer(f->proxy->loop, due > now ? due - now : 1,
        0, udp_flow_timer, f, &f->timer_id) == MT_OK;
}

/* False means f has been released. */
static bool udp_flow_update(udp_flow_t *f)
{
    uint32_t client_events = f->to_client.head ? EPOLLOUT : 0;
    uint32_t upstream_events = f->to_server.head ? EPOLLOUT : 0;
    if (f->to_server.count < UDP_QUEUE_PACKETS && f->to_server.bytes < UDP_QUEUE_BYTES) { client_events |= EPOLLIN; }
    if (f->to_client.count < UDP_QUEUE_PACKETS && f->to_client.bytes < UDP_QUEUE_BYTES) { upstream_events |= EPOLLIN; }
    if (udp_watch(f->proxy->loop, f->client_fd, &f->client_watched, &f->client_events,
                   client_events, f) != MT_OK ||
        udp_watch(f->proxy->loop, f->upstream_fd, &f->upstream_watched, &f->upstream_events,
                   upstream_events, f) != MT_OK || !udp_deadline(f)) {
        udp_flow_free(f, true); return false;
    }
    return true;
}

static void udp_flow_event(mt_loop_t *loop, int fd, uint32_t events, void *ud)
{
    (void)loop;
    udp_flow_t *f = ud;
    bool client = fd == f->client_fd;
    bool ok = true;
    if (events & EPOLLHUP) { udp_flow_free(f, true); return; }
    if (events & (EPOLLIN | EPOLLERR)) {
        if (client) { ok = udp_read_client(f); }
        else { ok = udp_read_server(f); if (!ok) { ok = udp_retry(f); } }
    }
    if (ok) { ok = udp_flush(f, true); if (!ok) { ok = udp_retry(f); } }
    if (ok) { ok = udp_flush(f, false); }
    if (!ok) { udp_flow_free(f, true); return; }
    (void)udp_flow_update(f);
}

static void udp_flow_timer(mt_loop_t *loop, void *ud)
{
    (void)loop;
    udp_flow_t *f = ud;
    f->timer_id = 0;
    uint64_t now = udp_now();
    if (f->retry_deadline && now >= f->retry_deadline) {
        /* A timer and a readable upstream can be returned in the same epoll
         * batch. Observe queued server datagrams before abandoning this path. */
        bool received = udp_read_server(f);
        if ((!received || !f->server_seen) && !udp_retry(f)) { udp_flow_free(f, true); return; }
    }
    if (now >= f->active_at + f->proxy->config.idle_timeout_ms) {
        udp_flow_free(f, !f->server_seen); return;
    }
    (void)udp_flow_update(f);
}

static mt_err_t udp_transparent(int fd, int family)
{
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0 ||
        (family == AF_INET6 && setsockopt(fd, SOL_IPV6, IPV6_V6ONLY, &one, sizeof(one)) != 0) ||
        setsockopt(fd, family == AF_INET ? SOL_IP : SOL_IPV6,
                   family == AF_INET ? IP_TRANSPARENT : IPV6_TRANSPARENT, &one, sizeof(one)) != 0) {
        return MT_ERR_IO;
    }
    return MT_OK;
}

static mt_err_t udp_reply_socket(udp_flow_t *f)
{
    int family = f->destination.family;
    f->client_fd = socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (f->client_fd < 0) { return MT_ERR_IO; }
    uint32_t mark = 0;
    if (udp_transparent(f->client_fd, family) != MT_OK ||
        setsockopt(f->client_fd, SOL_SOCKET, SO_MARK, &mark, sizeof(mark)) != 0) { return MT_ERR_IO; }
    struct sockaddr_storage sa;
    socklen_t length = udp_sockaddr(&f->destination, &sa);
    if (bind(f->client_fd, (const struct sockaddr *)&sa, length) != 0) { return MT_ERR_IO; }
    length = udp_sockaddr(&f->client, &sa);
    if (connect(f->client_fd, (const struct sockaddr *)&sa, length) != 0) { return MT_ERR_IO; }
    return MT_OK;
}

static udp_flow_t *udp_find(mt_udp_tproxy_t *p, const mt_tproxy_endpoint_t *client,
                            const mt_tproxy_endpoint_t *destination)
{
    for (udp_flow_t *f = p->flows; f; f = f->next) {
        if (udp_endpoint_equal(&f->client, client) && udp_endpoint_equal(&f->destination, destination)) { return f; }
    }
    return NULL;
}

static void udp_accept_datagram(mt_udp_tproxy_t *p, const mt_tproxy_endpoint_t *client,
                                 const mt_tproxy_endpoint_t *destination,
                                 const uint8_t *bytes, size_t length)
{
    udp_flow_t *f = udp_find(p, client, destination);
    if (f) {
        f->active_at = udp_now();
        udp_inspect_client(f, bytes, length);
        if (!udp_forward(f, true, bytes, length) && !udp_retry(f)) { udp_flow_free(f, true); return; }
        (void)udp_flow_update(f); return;
    }
    if (p->stats.active >= p->config.max_connections || length > UDP_DATAGRAM_MAX ||
        client->family != destination->family || destination->port != 443 ||
        (client->family != AF_INET && client->family != AF_INET6) ||
        !p->ops.authorize(p->ctx, client, destination)) { p->stats.failures++; return; }
    f = calloc(1, sizeof(*f));
    if (!f) { p->stats.failures++; return; }
    f->proxy = p; f->client = *client; f->destination = *destination;
    f->client_fd = f->upstream_fd = -1;
    f->retry_eligible = true;
    f->active_at = udp_now();
    f->retry_deadline = f->active_at + p->config.response_timeout_ms;
    if (udp_reply_socket(f) != MT_OK ||
        p->ops.pin(p->ctx, client, destination, &f->pin_token) != MT_OK || !f->pin_token) {
        p->stats.failures++; udp_flow_release(f); return;
    }
    f->next = p->flows; f->prev = &p->flows;
    if (f->next) { f->next->prev = &f->next; }
    p->flows = f; p->stats.active++; p->stats.accepted++;
    udp_inspect_client(f, bytes, length);
    bool ok = udp_start_upstream(f) == MT_OK && f->upstream_fd >= 0;
    if (ok) { ok = udp_forward(f, true, bytes, length); }
    if (!ok && !udp_retry(f)) { udp_flow_free(f, true); return; }
    if (udp_flow_update(f)) { (void)udp_listeners_update(p); }
}

static void udp_listener_retry(mt_loop_t *loop, void *ud)
{
    (void)loop;
    mt_udp_tproxy_t *p = ud;
    p->listener_retry_id = 0;
    (void)udp_listeners_update(p);
}

static void udp_listener_backoff(mt_udp_tproxy_t *p)
{
    p->stats.failures++;
    if (!p->listener_retry_id) {
        (void)mt_loop_add_reusable_timer(p->loop, 100, 0, udp_listener_retry, p, &p->listener_retry_id);
    }
    if (p->listener4_watched) { (void)mt_loop_del_fd(p->loop, p->listener4); p->listener4_watched = false; }
    if (p->listener6_watched) { (void)mt_loop_del_fd(p->loop, p->listener6); p->listener6_watched = false; }
}

static mt_err_t udp_listeners_update(mt_udp_tproxy_t *p)
{
    bool at_limit = p->stats.active >= p->config.max_connections;
    bool enabled = !p->stopping && !p->listener_retry_id && !at_limit;
    if (at_limit && !p->stopping && (p->listener4_watched || p->listener6_watched)) { p->stats.over_limit++; }
    int fds[] = {p->listener4, p->listener6};
    bool *watched[] = {&p->listener4_watched, &p->listener6_watched};
    for (size_t i = 0; i < 2; i++) {
        if (fds[i] < 0) { continue; }
        if (enabled && !*watched[i]) {
            mt_err_t err = mt_loop_add_fd(p->loop, fds[i], EPOLLIN, udp_listener_event, p);
            if (err != MT_OK) { return err; }
            *watched[i] = true;
        } else if (!enabled && *watched[i]) {
            (void)mt_loop_del_fd(p->loop, fds[i]); *watched[i] = false;
        }
    }
    return MT_OK;
}

static bool udp_original_destination(const struct msghdr *message,
                                      mt_tproxy_endpoint_t *destination)
{
    for (const struct cmsghdr *cmsg = CMSG_FIRSTHDR(message); cmsg;
         cmsg = CMSG_NXTHDR((struct msghdr *)message, (struct cmsghdr *)cmsg)) {
        size_t offset = (size_t)((const uint8_t *)cmsg - (const uint8_t *)message->msg_control);
        if (offset > message->msg_controllen || cmsg->cmsg_len < CMSG_LEN(0) ||
            cmsg->cmsg_len > message->msg_controllen - offset) { return false; }
        int family = 0;
        size_t length = 0;
        if (cmsg->cmsg_level == SOL_IP && cmsg->cmsg_type == IP_RECVORIGDSTADDR) {
            family = AF_INET; length = sizeof(struct sockaddr_in);
        } else if (cmsg->cmsg_level == SOL_IPV6 && cmsg->cmsg_type == IPV6_RECVORIGDSTADDR) {
            family = AF_INET6; length = sizeof(struct sockaddr_in6);
        }
        if (family && cmsg->cmsg_len >= CMSG_LEN(length)) {
            struct sockaddr_storage sa = {0};
            memcpy(&sa, CMSG_DATA(cmsg), length);
            return udp_endpoint(&sa, (socklen_t)length, destination) && destination->family == family;
        }
    }
    return false;
}

static void udp_listener_event(mt_loop_t *loop, int fd, uint32_t events, void *ud)
{
    (void)loop; (void)events;
    mt_udp_tproxy_t *p = ud;
    for (unsigned i = 0; i < UDP_BATCH; i++) {
        if (p->stats.active >= p->config.max_connections) { (void)udp_listeners_update(p); return; }
        struct sockaddr_storage peer = {0};
        union { struct cmsghdr alignment; uint8_t bytes[CMSG_SPACE(sizeof(struct sockaddr_in6))]; } control = {0};
        struct iovec iov = {.iov_base = p->scratch, .iov_len = sizeof(p->scratch)};
        struct msghdr message = {.msg_name = &peer, .msg_namelen = sizeof(peer),
            .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes)};
        ssize_t n = recvmsg(fd, &message, MSG_DONTWAIT | MSG_TRUNC);
        if (n < 0) {
            if (errno == EINTR) { continue; }
            if (errno != EAGAIN && errno != EWOULDBLOCK) { udp_listener_backoff(p); }
            return;
        }
        mt_tproxy_endpoint_t client, destination;
        if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || (size_t)n > sizeof(p->scratch) ||
            !udp_endpoint(&peer, message.msg_namelen, &client) ||
            !udp_original_destination(&message, &destination)) { p->stats.failures++; continue; }
        udp_accept_datagram(p, &client, &destination, p->scratch, (size_t)n);
    }
}

static mt_err_t udp_bind_listener(mt_udp_tproxy_t *p, int family, int *out)
{
    int fd = socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { return MT_ERR_IO; }
    int one = 1;
    mt_err_t err = udp_transparent(fd, family);
    if (err == MT_OK && setsockopt(fd, family == AF_INET ? SOL_IP : SOL_IPV6,
        family == AF_INET ? IP_RECVORIGDSTADDR : IPV6_RECVORIGDSTADDR, &one, sizeof(one)) != 0) { err = MT_ERR_IO; }
    mt_tproxy_endpoint_t endpoint = {.family = family, .port = p->config.port};
    if (family == AF_INET) { endpoint.addr[0] = 127; endpoint.addr[3] = 1; }
    else { endpoint.addr[15] = 1; }
    struct sockaddr_storage sa;
    socklen_t length = udp_sockaddr(&endpoint, &sa);
    if (err == MT_OK && bind(fd, (const struct sockaddr *)&sa, length) != 0) { err = MT_ERR_IO; }
    if (err == MT_OK && !p->config.port) {
        length = sizeof(sa);
        if (getsockname(fd, (struct sockaddr *)&sa, &length) != 0 ||
            !udp_endpoint(&sa, length, &endpoint)) { err = MT_ERR_IO; }
        else { p->config.port = endpoint.port; }
    }
    if (err != MT_OK) { close(fd); return err; }
    *out = fd; return MT_OK;
}

mt_err_t mt_udp_tproxy_create(mt_loop_t *loop, const mt_udp_tproxy_config_t *config,
                              const mt_udp_tproxy_ops_t *ops, void *ctx,
                              mt_udp_tproxy_t **out)
{
    if (out) { *out = NULL; }
    if (!loop || !config || !ops || !out || !ops->authorize || !ops->prepare || !ops->pin || !ops->unpin ||
        (!config->ipv4 && !config->ipv6) || !config->response_timeout_ms || !config->idle_timeout_ms ||
        !config->max_connections || config->max_connections > 4096) { return MT_ERR_INVAL; }
    mt_udp_tproxy_t *p = calloc(1, sizeof(*p));
    if (!p) { return MT_ERR_NOMEM; }
    p->loop = loop; p->config = *config; p->ops = *ops; p->ctx = ctx;
    p->listener4 = p->listener6 = -1;
    mt_err_t err = config->ipv4 ? udp_bind_listener(p, AF_INET, &p->listener4) : MT_OK;
    if (err == MT_OK && config->ipv6) { err = udp_bind_listener(p, AF_INET6, &p->listener6); }
    if (err == MT_OK) { err = udp_listeners_update(p); }
    if (err != MT_OK) { mt_udp_tproxy_destroy(p); return err; }
    *out = p; return MT_OK;
}

void mt_udp_tproxy_destroy(mt_udp_tproxy_t *p)
{
    if (!p) { return; }
    p->stopping = true;
    if (p->listener_retry_id) { (void)mt_loop_del_timer(p->loop, p->listener_retry_id); }
    udp_close(p->loop, &p->listener4, &p->listener4_watched);
    udp_close(p->loop, &p->listener6, &p->listener6_watched);
    while (p->flows) {
        udp_flow_t *f = p->flows;
        p->flows = f->next;
        if (p->flows) { p->flows->prev = &p->flows; }
        udp_flow_release(f);
    }
    free(p);
}

uint16_t mt_udp_tproxy_port(const mt_udp_tproxy_t *p)
{
    return p ? p->config.port : 0;
}

void mt_udp_tproxy_stats(const mt_udp_tproxy_t *p, mt_tproxy_stats_t *out)
{
    if (out) { *out = p ? p->stats : (mt_tproxy_stats_t){0}; }
}
