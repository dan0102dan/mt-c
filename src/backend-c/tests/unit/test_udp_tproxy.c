/* Ordinary loopback UDP sockets exercise the production relay. Only the
 * transparent socket privilege and the original destination's well-known
 * port are mapped here; production has no test switch or privilege bypass. */
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

static uint16_t fixture_target_port;
static int fixture_reply_fd = -1;
static int fixture_block_fd = -1;
static int fixture_send_error_fd = -1;
static unsigned fixture_send_errors;
static int fixture_recv_error_fd = -1;
static unsigned fixture_recv_errors;
static bool fixture_fail_transparent;
static bool fixture_fail_socket;

static int fixture_setsockopt(int fd, int level, int option, const void *value, socklen_t length)
{
    if ((level == SOL_IP && option == IP_TRANSPARENT) ||
        (level == SOL_IPV6 && option == IPV6_TRANSPARENT)) {
        if (fixture_fail_transparent) { errno = EPERM; return -1; }
        return 0;
    }
    if (level == SOL_SOCKET && option == SO_MARK) {
        if (length != sizeof(uint32_t) || *(const uint32_t *)value != 0) { abort(); }
        return 0;
    }
    return setsockopt(fd, level, option, value, length);
}

static int fixture_bind(int fd, const struct sockaddr *address, socklen_t length)
{
    if (address->sa_family == AF_INET && ((const struct sockaddr_in *)address)->sin_port == htons(443)) {
        struct sockaddr_in mapped = *(const struct sockaddr_in *)address;
        mapped.sin_port = 0; fixture_reply_fd = fd;
        return bind(fd, (const struct sockaddr *)&mapped, sizeof(mapped));
    }
    return bind(fd, address, length);
}

static int fixture_connect(int fd, const struct sockaddr *address, socklen_t length)
{
    if (fixture_target_port && address->sa_family == AF_INET &&
        ((const struct sockaddr_in *)address)->sin_port == htons(443)) {
        struct sockaddr_in mapped = *(const struct sockaddr_in *)address;
        mapped.sin_port = htons(fixture_target_port);
        mapped.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return connect(fd, (const struct sockaddr *)&mapped, sizeof(mapped));
    }
    return connect(fd, address, length);
}

static int fixture_socket(int family, int type, int protocol)
{
    if (fixture_fail_socket) { fixture_fail_socket = false; errno = EMFILE; return -1; }
    return socket(family, type, protocol);
}

static ssize_t fixture_send(int fd, const void *bytes, size_t length, int flags)
{
    if (fd == fixture_send_error_fd && fixture_send_errors) {
        fixture_send_errors--; errno = ECONNREFUSED; return -1;
    }
    if (fd == fixture_block_fd) { errno = EAGAIN; return -1; }
    return send(fd, bytes, length, flags);
}

static ssize_t fixture_recv(int fd, void *bytes, size_t length, int flags)
{
    if (fd == fixture_recv_error_fd && fixture_recv_errors) {
        fixture_recv_errors--; errno = ECONNREFUSED; return -1;
    }
    return recv(fd, bytes, length, flags);
}

#define setsockopt fixture_setsockopt
#define bind fixture_bind
#define connect fixture_connect
#define socket fixture_socket
#define send fixture_send
#define recv fixture_recv
#include "../../src/platform/udp_tproxy.c"
#undef setsockopt
#undef bind
#undef connect
#undef socket
#undef send
#undef recv

struct probe_ctx {
    uint16_t direct_port;
    uint16_t alternate_port;
    unsigned direct_prepares;
    unsigned alternate_prepares;
    unsigned direct_learned;
    unsigned alternate_learned;
    unsigned pin_calls;
    unsigned unpin_calls;
    uint32_t next_token;
    int pin_fds[32];
    bool unpin_before_close;
    bool reject;
    bool reject_direct;
    bool reject_all;
    bool zero_token;
    bool fail_outgoing_socket;
    mt_err_t pin_error;
};

static bool probe_authorize(void *userdata, const mt_tproxy_endpoint_t *client,
                             const mt_tproxy_endpoint_t *destination)
{
    (void)client; (void)destination;
    return !((struct probe_ctx *)userdata)->reject;
}

static mt_err_t probe_prepare(void *userdata, int fd, const mt_tproxy_endpoint_t *client,
                               const mt_tproxy_endpoint_t *destination, bool alternate)
{
    (void)fd; (void)client; (void)destination;
    struct probe_ctx *ctx = userdata;
    if (alternate) { ctx->alternate_prepares++; } else { ctx->direct_prepares++; }
    fixture_target_port = alternate ? ctx->alternate_port : ctx->direct_port;
    return ctx->reject_all || (!alternate && ctx->reject_direct) ? MT_ERR_UPSTREAM : MT_OK;
}

static void probe_learned(void *userdata, const mt_tproxy_endpoint_t *client,
                           const mt_tproxy_endpoint_t *destination, bool alternate)
{
    (void)client; (void)destination;
    struct probe_ctx *ctx = userdata;
    if (alternate) { ctx->alternate_learned++; } else { ctx->direct_learned++; }
}

static mt_err_t probe_pin(void *userdata, const mt_tproxy_endpoint_t *client,
                           const mt_tproxy_endpoint_t *destination, uint32_t *token)
{
    (void)client; (void)destination;
    struct probe_ctx *ctx = userdata;
    ctx->pin_calls++;
    if (ctx->pin_error != MT_OK) { return ctx->pin_error; }
    if (ctx->zero_token) { return MT_OK; }
    *token = ++ctx->next_token;
    if (*token >= sizeof(ctx->pin_fds) / sizeof(ctx->pin_fds[0])) { abort(); }
    ctx->pin_fds[*token] = fixture_reply_fd;
    if (ctx->fail_outgoing_socket) { fixture_fail_socket = true; }
    return MT_OK;
}

static void probe_unpin(void *userdata, uint32_t token)
{
    struct probe_ctx *ctx = userdata;
    ctx->unpin_calls++;
    if (token >= sizeof(ctx->pin_fds) / sizeof(ctx->pin_fds[0]) ||
        fcntl(ctx->pin_fds[token], F_GETFD) >= 0 || errno != EBADF) { ctx->unpin_before_close = true; }
}

static const mt_udp_tproxy_ops_t probe_ops = {.authorize = probe_authorize, .prepare = probe_prepare,
    .learned = probe_learned, .pin = probe_pin, .unpin = probe_unpin};

static int local_udp(uint16_t *port)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { return -1; }
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t length = sizeof(sa);
    if (bind(fd, (const struct sockaddr *)&sa, length) != 0 ||
        getsockname(fd, (struct sockaddr *)&sa, &length) != 0) { close(fd); return -1; }
    *port = ntohs(sa.sin_port); return fd;
}

struct fixture {
    mt_loop_t *loop;
    mt_udp_tproxy_t *proxy;
    struct probe_ctx ctx;
    int client;
    int servers[2];
    mt_tproxy_endpoint_t client_endpoint;
    mt_tproxy_endpoint_t destination;
};

static int fixture_init(struct fixture *h)
{
    memset(h, 0, sizeof(*h));
    fixture_target_port = 0; fixture_reply_fd = fixture_block_fd = -1;
    fixture_send_error_fd = fixture_recv_error_fd = -1;
    fixture_send_errors = fixture_recv_errors = 0;
    fixture_fail_transparent = fixture_fail_socket = false;
    ASSERT_EQ(MT_OK, mt_loop_create(&h->loop));
    h->servers[0] = local_udp(&h->ctx.direct_port); ASSERT(h->servers[0] >= 0);
    h->servers[1] = local_udp(&h->ctx.alternate_port); ASSERT(h->servers[1] >= 0);
    uint16_t client_port = 0; h->client = local_udp(&client_port); ASSERT(h->client >= 0);
    h->client_endpoint = (mt_tproxy_endpoint_t){.family = AF_INET, .addr = {127,0,0,1}, .port = client_port};
    h->destination = (mt_tproxy_endpoint_t){.family = AF_INET, .addr = {127,0,0,2}, .port = 443};
    mt_udp_tproxy_config_t config = {.ipv4 = true, .response_timeout_ms = 40,
        .idle_timeout_ms = 1000, .max_connections = 4};
    ASSERT_EQ(MT_OK, mt_udp_tproxy_create(h->loop, &config, &probe_ops, &h->ctx, &h->proxy));
    return GREATEST_TEST_RES_PASS;
}

static int fixture_done(struct fixture *h)
{
    mt_udp_tproxy_destroy(h->proxy); h->proxy = NULL;
    ASSERT_EQ(h->ctx.next_token, h->ctx.unpin_calls); ASSERT(!h->ctx.unpin_before_close);
    close(h->client); close(h->servers[0]); close(h->servers[1]); mt_loop_destroy(h->loop);
    fixture_target_port = 0; fixture_block_fd = -1;
    return GREATEST_TEST_RES_PASS;
}

static const uint8_t original_cid[8] = {0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37};
static const uint8_t client_cid[8] = {0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67};
static const uint8_t server_cid[8] = {0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97};

static size_t quic_packet(uint8_t *out, uint32_t version, mt_quic_packet_type_t type,
                           const uint8_t *dcid, const uint8_t *scid, bool token, size_t size)
{
    unsigned wire = type == MT_QUIC_INITIAL ? 0u : type == MT_QUIC_ZERO_RTT ? 1u : 2u;
    if (version == MT_QUIC_VERSION_2) { wire++; }
    out[0] = (uint8_t)(0xc0u | (wire << 4));
    out[1] = (uint8_t)(version >> 24); out[2] = (uint8_t)(version >> 16);
    out[3] = (uint8_t)(version >> 8); out[4] = (uint8_t)version;
    size_t pos = 5; out[pos++] = 8; memcpy(out + pos, dcid, 8); pos += 8;
    out[pos++] = 8; memcpy(out + pos, scid, 8); pos += 8;
    if (type == MT_QUIC_INITIAL) { out[pos++] = token ? 1 : 0; if (token) { out[pos++] = 0x55; } }
    size_t payload = size - pos - 2;
    out[pos++] = (uint8_t)(0x40u | (payload >> 8)); out[pos++] = (uint8_t)payload;
    memset(out + pos, 0xaa, payload); return size;
}

static size_t quic_initial(uint8_t *out, uint32_t version)
{
    return quic_packet(out, version, MT_QUIC_INITIAL, original_cid, client_cid, false, 1200);
}

static ssize_t server_receive(struct fixture *h, bool alternate, uint8_t *bytes, size_t length,
                               struct sockaddr_storage *peer, socklen_t *peer_length)
{
    return recvfrom(h->servers[alternate ? 1 : 0], bytes, length, 0,
                     (struct sockaddr *)peer, peer_length);
}

static void force_response_deadline(udp_flow_t *f)
{
    if (f->timer_id) { (void)mt_loop_del_timer(f->proxy->loop, f->timer_id); f->timer_id = 0; }
    f->retry_deadline = udp_now();
    udp_flow_timer(f->proxy->loop, f);
}

TEST listeners_bind_loopback_and_creation_failures_close_resources(void)
{
    struct fixture h; CHECK_CALL(fixture_init(&h));
    struct sockaddr_storage sa = {0}; socklen_t length = sizeof(sa);
    ASSERT_EQ(0, getsockname(h.proxy->listener4, (struct sockaddr *)&sa, &length));
    mt_tproxy_endpoint_t endpoint; ASSERT(udp_endpoint(&sa, length, &endpoint));
    ASSERT_EQ(127, endpoint.addr[0]); ASSERT_EQ(1, endpoint.addr[3]);
    ASSERT_EQ(endpoint.port, mt_udp_tproxy_port(h.proxy)); ASSERT(endpoint.port != 0);
    mt_udp_tproxy_t *other = (mt_udp_tproxy_t *)(uintptr_t)1;
    ASSERT_EQ(MT_ERR_INVAL, mt_udp_tproxy_create(NULL, NULL, NULL, NULL, &other)); ASSERT(other == NULL);
    mt_udp_tproxy_config_t config = {.ipv6 = true, .response_timeout_ms = 20,
        .idle_timeout_ms = 100, .max_connections = 1};
    ASSERT_EQ(MT_OK, mt_udp_tproxy_create(h.loop, &config, &probe_ops, &h.ctx, &other));
    length = sizeof(sa); ASSERT_EQ(0, getsockname(other->listener6, (struct sockaddr *)&sa, &length));
    ASSERT(udp_endpoint(&sa, length, &endpoint)); ASSERT_EQ(AF_INET6, endpoint.family);
    ASSERT_EQ(1, endpoint.addr[15]); mt_udp_tproxy_destroy(other);
    fixture_fail_transparent = true;
    ASSERT_EQ(MT_ERR_IO, mt_udp_tproxy_create(h.loop, &config, &probe_ops, &h.ctx, &other));
    ASSERT(other == NULL); fixture_fail_transparent = false;
    mt_tproxy_stats_t stats = {.active = 1}; mt_udp_tproxy_stats(NULL, &stats);
    ASSERT_EQ(0u, stats.active); ASSERT_EQ(0u, mt_udp_tproxy_port(NULL));
    CHECK_CALL(fixture_done(&h)); PASS();
}

TEST original_destination_ancillary_data_is_required_and_validated(void)
{
    union { struct cmsghdr alignment; uint8_t bytes[CMSG_SPACE(sizeof(struct sockaddr_in6))]; } control = {0};
    struct msghdr message = {.msg_control = control.bytes, .msg_controllen = sizeof(control.bytes)};
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
    ASSERT(cmsg != NULL);
    cmsg->cmsg_level = SOL_IP; cmsg->cmsg_type = IP_RECVORIGDSTADDR;
    cmsg->cmsg_len = CMSG_LEN(sizeof(struct sockaddr_in));
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(443)};
    ASSERT_EQ(1, inet_pton(AF_INET, "203.0.113.10", &sa.sin_addr));
    memcpy(CMSG_DATA(cmsg), &sa, sizeof(sa));
    mt_tproxy_endpoint_t endpoint;
    ASSERT(udp_original_destination(&message, &endpoint));
    ASSERT_EQ(AF_INET, endpoint.family); ASSERT_EQ(443, endpoint.port); ASSERT_EQ(203, endpoint.addr[0]);
    cmsg->cmsg_len--; ASSERT(!udp_original_destination(&message, &endpoint));
    cmsg->cmsg_len = sizeof(control.bytes) + 1;
    ASSERT(!udp_original_destination(&message, &endpoint));
    cmsg->cmsg_type = IP_PKTINFO; ASSERT(!udp_original_destination(&message, &endpoint));
    message.msg_controllen = 0; ASSERT(!udp_original_destination(&message, &endpoint));
    PASS();
}

TEST initial_only_timeout_retries_once_and_keeps_reply_socket(void)
{
    for (unsigned v2 = 0; v2 < 2; v2++) {
        struct fixture h; CHECK_CALL(fixture_init(&h));
        uint8_t initial[1200], received[2400]; size_t n = quic_initial(initial, v2 ? MT_QUIC_VERSION_2 : MT_QUIC_VERSION_1);
        udp_accept_datagram(h.proxy, &h.client_endpoint, &h.destination, initial, n);
        ASSERT_EQ(1u, h.proxy->stats.active); udp_flow_t *f = h.proxy->flows;
        ASSERT(f->retry_eligible); ASSERT_EQ(1u, f->replay.count); int reply = f->client_fd;
        struct sockaddr_storage peer; socklen_t peer_length = sizeof(peer);
        ASSERT_EQ((ssize_t)n, server_receive(&h, false, received, sizeof(received), &peer, &peer_length));
        ASSERT_EQ(0, memcmp(initial, received, n));
        force_response_deadline(f);
        ASSERT_EQ(1u, h.proxy->stats.active); ASSERT(h.proxy->flows == f); ASSERT_EQ(reply, f->client_fd);
        ASSERT(f->alternate); ASSERT(!f->retry_eligible); ASSERT_EQ(0u, f->replay.count);
        ASSERT_EQ(1u, h.ctx.direct_prepares); ASSERT_EQ(1u, h.ctx.alternate_prepares);
        peer_length = sizeof(peer);
        ASSERT_EQ((ssize_t)n, server_receive(&h, true, received, sizeof(received), &peer, &peer_length));
        ASSERT_EQ(0, memcmp(initial, received, n)); ASSERT(!udp_retry(f));
        CHECK_CALL(fixture_done(&h));
    }
    PASS();
}

TEST queued_server_datagram_wins_over_simultaneous_timeout(void)
{
    struct fixture h; CHECK_CALL(fixture_init(&h));
    uint8_t initial[1200], received[1200]; size_t n = quic_initial(initial, MT_QUIC_VERSION_1);
    udp_accept_datagram(h.proxy, &h.client_endpoint, &h.destination, initial, n);
    udp_flow_t *f = h.proxy->flows; ASSERT(f != NULL);
    struct sockaddr_storage peer; socklen_t peer_length = sizeof(peer);
    ASSERT_EQ((ssize_t)n, server_receive(&h, false, received, sizeof(received), &peer, &peer_length));
    ASSERT_EQ(5, sendto(h.servers[0], "retry", 5, 0, (const struct sockaddr *)&peer, peer_length));
    force_response_deadline(f);
    ASSERT(f->server_seen); ASSERT(!f->alternate); ASSERT(!f->retry_eligible);
    ASSERT_EQ(0u, h.ctx.alternate_prepares); ASSERT_EQ(0u, h.ctx.direct_learned);
    ASSERT_EQ(5, recv(h.client, received, sizeof(received), 0)); ASSERT_EQ(0, memcmp(received, "retry", 5));
    CHECK_CALL(fixture_done(&h)); PASS();
}

TEST queued_response_prevents_send_error_retry_even_with_pending_error_and_backpressure(void)
{
    for (unsigned variant = 0; variant < 4; variant++) {
        bool empty = (variant & 1u) != 0, full = (variant & 2u) != 0;
        struct fixture h; CHECK_CALL(fixture_init(&h));
        uint8_t initial[1200], received[1200]; quic_initial(initial, MT_QUIC_VERSION_1);
        udp_accept_datagram(h.proxy, &h.client_endpoint, &h.destination, initial, sizeof(initial));
        udp_flow_t *f = h.proxy->flows; ASSERT(f != NULL);
        int upstream = f->upstream_fd, reply_fd = f->client_fd;
        struct sockaddr_storage peer; socklen_t peer_length = sizeof(peer);
        ASSERT_EQ(1200, server_receive(&h, false, received, sizeof(received), &peer, &peer_length));
        size_t response_length = empty ? 0u : 5u;
        ASSERT_EQ((ssize_t)response_length, sendto(h.servers[0], "reply", response_length, 0,
            (const struct sockaddr *)&peer, peer_length));
        if (full) {
            for (unsigned i = 0; i < UDP_QUEUE_PACKETS; i++) {
                ASSERT(udp_queue_push(&f->to_client, (const uint8_t *)"queued", 6, UDP_QUEUE_BYTES));
            }
            fixture_block_fd = f->client_fd;
        }
        /* Dispatch the client's retransmission first, despite the queued
         * upstream response. The failed send is followed by a pending error
         * on recv, then by the real datagram still waiting in the socket. */
        fixture_send_error_fd = upstream; fixture_send_errors = 1;
        fixture_recv_error_fd = upstream; fixture_recv_errors = 1;
        struct sockaddr_storage reply; socklen_t reply_length = sizeof(reply);
        ASSERT_EQ(0, getsockname(reply_fd, (struct sockaddr *)&reply, &reply_length));
        ASSERT_EQ(1200, sendto(h.client, initial, sizeof(initial), 0,
            (const struct sockaddr *)&reply, reply_length));
        udp_flow_event(h.loop, reply_fd, EPOLLIN, f);
        ASSERT(h.proxy->flows == f); ASSERT(f->server_seen); ASSERT(!f->retry_eligible);
        ASSERT(!f->alternate); ASSERT_EQ(upstream, f->upstream_fd); ASSERT_EQ(reply_fd, f->client_fd);
        ASSERT_EQ(0u, fixture_send_errors); ASSERT_EQ(0u, fixture_recv_errors);
        ASSERT_EQ(0u, h.ctx.alternate_prepares); ASSERT_EQ(0u, f->replay.count);
        if (full) {
            ASSERT_EQ(UDP_QUEUE_PACKETS, f->to_client.count);
            fixture_block_fd = -1; udp_flow_event(h.loop, reply_fd, EPOLLOUT, f);
            for (unsigned i = 0; i < UDP_QUEUE_PACKETS; i++) {
                ASSERT_EQ(6, recv(h.client, received, sizeof(received), 0));
            }
        }
        udp_flow_event(h.loop, upstream, EPOLLIN, f);
        ASSERT_EQ((ssize_t)response_length, recv(h.client, received, sizeof(received), 0));
        ASSERT_EQ(0, memcmp(received, "reply", response_length));
        ASSERT_EQ(0u, h.ctx.alternate_prepares); ASSERT_EQ(1u, h.proxy->stats.active);
        CHECK_CALL(fixture_done(&h));
    }
    PASS();
}

TEST token_early_data_and_opaque_datagrams_are_forwarded_once_without_retry(void)
{
    for (unsigned kind = 0; kind < 5; kind++) {
        struct fixture h; CHECK_CALL(fixture_init(&h));
        uint8_t bytes[2400], received[2400]; size_t n = 1200;
        quic_initial(bytes, MT_QUIC_VERSION_1);
        if (kind == 0) { quic_packet(bytes, MT_QUIC_VERSION_1, MT_QUIC_INITIAL, original_cid, client_cid, true, n); }
        else if (kind == 1) {
            quic_packet(bytes + 1200, MT_QUIC_VERSION_1, MT_QUIC_ZERO_RTT, original_cid, client_cid, false, 1200);
            n = 2400;
        } else if (kind == 2) { bytes[1] = 0x55; }
        else if (kind == 3) { bytes[0] = 0x40; }
        else { memcpy(bytes, "POST /change", 12); n = 12; }
        udp_accept_datagram(h.proxy, &h.client_endpoint, &h.destination, bytes, n);
        udp_flow_t *f = h.proxy->flows; ASSERT(f != NULL); ASSERT(!f->retry_eligible);
        ASSERT_EQ(0u, f->retry_deadline); ASSERT(!udp_retry(f));
        ASSERT_EQ((ssize_t)n, recv(h.servers[0], received, sizeof(received), 0));
        ASSERT_EQ(0, memcmp(bytes, received, n));
        ASSERT_EQ(-1, recv(h.servers[1], received, sizeof(received), 0));
        ASSERT_EQ(0u, h.ctx.alternate_prepares);
        CHECK_CALL(fixture_done(&h));
    }
    PASS();
}

TEST later_early_data_and_changed_cids_permanently_disable_retry(void)
{
    for (unsigned changed = 0; changed < 2; changed++) {
        struct fixture h; CHECK_CALL(fixture_init(&h));
        uint8_t bytes[1200]; quic_initial(bytes, MT_QUIC_VERSION_1);
        udp_accept_datagram(h.proxy, &h.client_endpoint, &h.destination, bytes, sizeof(bytes));
        udp_flow_t *f = h.proxy->flows; ASSERT(f != NULL); ASSERT(f->retry_eligible);
        if (changed) { bytes[6] ^= 1; }
        else { quic_packet(bytes, MT_QUIC_VERSION_1, MT_QUIC_ZERO_RTT, original_cid, client_cid, false, sizeof(bytes)); }
        udp_accept_datagram(h.proxy, &h.client_endpoint, &h.destination, bytes, sizeof(bytes));
        ASSERT(!f->retry_eligible); ASSERT_EQ(0u, f->replay.count); ASSERT(!udp_retry(f));
        quic_initial(bytes, MT_QUIC_VERSION_1);
        udp_accept_datagram(h.proxy, &h.client_endpoint, &h.destination, bytes, sizeof(bytes));
        ASSERT(!f->retry_eligible); ASSERT_EQ(0u, h.ctx.alternate_prepares);
        CHECK_CALL(fixture_done(&h));
    }
    PASS();
}

TEST replay_budget_exhaustion_keeps_the_direct_flow(void)
{
    struct fixture h; CHECK_CALL(fixture_init(&h));
    uint8_t initial[1200], received[1200]; quic_initial(initial, MT_QUIC_VERSION_1);
    for (unsigned i = 0; i < 14; i++) {
        udp_accept_datagram(h.proxy, &h.client_endpoint, &h.destination, initial, sizeof(initial));
        ASSERT_EQ((ssize_t)sizeof(initial), recv(h.servers[0], received, sizeof(received), 0));
        ASSERT_EQ(1u, h.proxy->stats.active);
    }
    udp_flow_t *f = h.proxy->flows;
    ASSERT(!f->retry_eligible); ASSERT_EQ(0u, f->replay.bytes); ASSERT_EQ(0u, f->retry_deadline);
    ASSERT(!udp_retry(f)); ASSERT_EQ(0u, h.ctx.alternate_prepares);
    CHECK_CALL(fixture_done(&h)); PASS();
}

TEST pin_failure_prevents_upstream_io_and_release_closes_before_unpin(void)
{
    struct fixture h; CHECK_CALL(fixture_init(&h));
    uint8_t initial[1200]; quic_initial(initial, MT_QUIC_VERSION_1);
    h.ctx.pin_error = MT_ERR_AGAIN;
    udp_accept_datagram(h.proxy, &h.client_endpoint, &h.destination, initial, sizeof(initial));
    ASSERT(h.proxy->flows == NULL); ASSERT_EQ(0u, h.ctx.direct_prepares); ASSERT_EQ(1u, h.ctx.pin_calls);
    ASSERT_EQ(-1, fcntl(fixture_reply_fd, F_GETFD)); ASSERT_EQ(EBADF, errno);
    h.ctx.pin_error = MT_OK; h.ctx.zero_token = true;
    udp_accept_datagram(h.proxy, &h.client_endpoint, &h.destination, initial, sizeof(initial));
    ASSERT(h.proxy->flows == NULL); ASSERT_EQ(0u, h.ctx.direct_prepares);
    h.ctx.zero_token = false; h.ctx.reject_all = true;
    udp_accept_datagram(h.proxy, &h.client_endpoint, &h.destination, initial, sizeof(initial));
    ASSERT(h.proxy->flows == NULL); ASSERT_EQ(1u, h.ctx.direct_prepares); ASSERT_EQ(1u, h.ctx.alternate_prepares);
    ASSERT_EQ(1u, h.ctx.unpin_calls); ASSERT(!h.ctx.unpin_before_close);
    CHECK_CALL(fixture_done(&h)); PASS();
}

TEST outgoing_creation_or_prepare_failure_uses_one_alternate_attempt(void)
{
    for (unsigned failure = 0; failure < 2; failure++) {
        struct fixture h; CHECK_CALL(fixture_init(&h));
        h.ctx.reject_direct = failure == 0; h.ctx.fail_outgoing_socket = failure != 0;
        uint8_t initial[1200], received[1200]; quic_initial(initial, MT_QUIC_VERSION_1);
        udp_accept_datagram(h.proxy, &h.client_endpoint, &h.destination, initial, sizeof(initial));
        ASSERT(h.proxy->flows != NULL); ASSERT(h.proxy->flows->alternate);
        ASSERT_EQ(1u, h.ctx.alternate_prepares); ASSERT_EQ(1u, h.ctx.pin_calls);
        ASSERT_EQ((ssize_t)sizeof(initial), recv(h.servers[1], received, sizeof(received), 0));
        ASSERT_EQ(0, memcmp(initial, received, sizeof(initial)));
        CHECK_CALL(fixture_done(&h));
    }
    PASS();
}

TEST learning_requires_client_progress_and_correlated_server_response(void)
{
    struct fixture h; CHECK_CALL(fixture_init(&h));
    uint8_t bytes[1200]; quic_initial(bytes, MT_QUIC_VERSION_1);
    udp_accept_datagram(h.proxy, &h.client_endpoint, &h.destination, bytes, sizeof(bytes));
    udp_flow_t *f = h.proxy->flows; ASSERT(f != NULL);
    quic_packet(bytes, MT_QUIC_VERSION_1, MT_QUIC_INITIAL, client_cid, server_cid, false, 1200);
    udp_server_received(f, bytes, 1200);
    ASSERT_EQ(1u, f->progress); ASSERT_EQ(0u, h.ctx.direct_learned); ASSERT(!f->retry_eligible);
    udp_server_received(f, bytes, 1200); ASSERT_EQ(0u, h.ctx.direct_learned);
    quic_packet(bytes, MT_QUIC_VERSION_1, MT_QUIC_HANDSHAKE, original_cid, client_cid, false, 100);
    udp_client_sent(f, bytes, 100); ASSERT_EQ(1u, f->progress);
    quic_packet(bytes, MT_QUIC_VERSION_2, MT_QUIC_HANDSHAKE, server_cid, client_cid, false, 100);
    udp_client_sent(f, bytes, 100); ASSERT_EQ(1u, f->progress);
    quic_packet(bytes, MT_QUIC_VERSION_1, MT_QUIC_HANDSHAKE, server_cid, client_cid, false, 100);
    udp_client_sent(f, bytes, 100); ASSERT_EQ(2u, f->progress);
    quic_packet(bytes, MT_QUIC_VERSION_1, MT_QUIC_HANDSHAKE, original_cid, server_cid, false, 100);
    udp_server_received(f, bytes, 100); ASSERT_EQ(0u, h.ctx.direct_learned);
    quic_packet(bytes, MT_QUIC_VERSION_2, MT_QUIC_HANDSHAKE, client_cid, server_cid, false, 100);
    udp_server_received(f, bytes, 100); ASSERT_EQ(0u, h.ctx.direct_learned);
    quic_packet(bytes, MT_QUIC_VERSION_1, MT_QUIC_INITIAL, client_cid, server_cid, false, 1200);
    udp_server_received(f, bytes, 1200); ASSERT_EQ(0u, h.ctx.direct_learned);
    quic_packet(bytes, MT_QUIC_VERSION_1, MT_QUIC_HANDSHAKE, client_cid, server_cid, false, 100);
    udp_server_received(f, bytes, 100); ASSERT_EQ(1u, h.ctx.direct_learned);
    udp_server_received(f, bytes, 100); ASSERT_EQ(1u, h.ctx.direct_learned);
    ASSERT_EQ(1u, h.proxy->stats.direct_success);
    CHECK_CALL(fixture_done(&h)); PASS();
}

TEST established_client_socket_and_upstream_source_stay_pinned_after_learning(void)
{
    struct fixture h; CHECK_CALL(fixture_init(&h));
    uint8_t bytes[1200], received[1200]; quic_initial(bytes, MT_QUIC_VERSION_1);
    udp_accept_datagram(h.proxy, &h.client_endpoint, &h.destination, bytes, sizeof(bytes));
    udp_flow_t *f = h.proxy->flows; ASSERT(f != NULL);
    struct sockaddr_storage original_peer, peer; socklen_t original_length = sizeof(original_peer), peer_length;
    ASSERT_EQ(1200, server_receive(&h, false, received, sizeof(received), &original_peer, &original_length));
    quic_packet(bytes, MT_QUIC_VERSION_1, MT_QUIC_INITIAL, client_cid, server_cid, false, 1200);
    ASSERT_EQ(1200, sendto(h.servers[0], bytes, 1200, 0, (const struct sockaddr *)&original_peer, original_length));
    udp_flow_event(h.loop, f->upstream_fd, EPOLLIN, f);
    ASSERT_EQ(1200, recv(h.client, received, sizeof(received), 0)); ASSERT_EQ(1u, f->progress);
    struct sockaddr_storage reply = {0}; socklen_t reply_length = sizeof(reply);
    ASSERT_EQ(0, getsockname(f->client_fd, (struct sockaddr *)&reply, &reply_length));
    quic_packet(bytes, MT_QUIC_VERSION_1, MT_QUIC_HANDSHAKE, server_cid, client_cid, false, 100);
    ASSERT_EQ(100, sendto(h.client, bytes, 100, 0, (const struct sockaddr *)&reply, reply_length));
    udp_flow_event(h.loop, f->client_fd, EPOLLIN, f);
    ASSERT_EQ(2u, f->progress); peer_length = sizeof(peer);
    ASSERT_EQ(100, server_receive(&h, false, received, sizeof(received), &peer, &peer_length));
    ASSERT_EQ(original_length, peer_length); ASSERT_EQ(0, memcmp(&original_peer, &peer, peer_length));
    memset(bytes, 0xa5, 80); bytes[0] = 0x40; memcpy(bytes + 1, client_cid, 8);
    ASSERT_EQ(80, sendto(h.servers[0], bytes, 80, 0, (const struct sockaddr *)&peer, peer_length));
    udp_flow_event(h.loop, f->upstream_fd, EPOLLIN, f);
    ASSERT_EQ(80, recv(h.client, received, sizeof(received), 0)); ASSERT_EQ(1u, h.ctx.direct_learned);
    int upstream_fd = f->upstream_fd, client_fd = f->client_fd;
    ASSERT_EQ(12, sendto(h.client, "opaque bytes", 12, 0, (const struct sockaddr *)&reply, reply_length));
    udp_flow_event(h.loop, f->client_fd, EPOLLIN, f); peer_length = sizeof(peer);
    ASSERT_EQ(12, server_receive(&h, false, received, sizeof(received), &peer, &peer_length));
    ASSERT_EQ(0, memcmp(received, "opaque bytes", 12)); ASSERT_EQ(0, memcmp(&original_peer, &peer, peer_length));
    ASSERT_EQ(upstream_fd, f->upstream_fd); ASSERT_EQ(client_fd, f->client_fd);
    ASSERT_EQ(1u, h.ctx.direct_prepares); ASSERT_EQ(0u, h.ctx.alternate_prepares);
    CHECK_CALL(fixture_done(&h)); PASS();
}

TEST bounded_eagain_queue_keeps_datagram_boundaries_and_backpressure(void)
{
    struct fixture h; CHECK_CALL(fixture_init(&h));
    uint8_t initial[1200], received[1200]; quic_initial(initial, MT_QUIC_VERSION_1);
    udp_accept_datagram(h.proxy, &h.client_endpoint, &h.destination, initial, sizeof(initial));
    udp_flow_t *f = h.proxy->flows; ASSERT(f != NULL);
    ASSERT_EQ(1200, recv(h.servers[0], received, sizeof(received), 0));
    fixture_block_fd = f->upstream_fd;
    for (unsigned i = 0; i < UDP_QUEUE_PACKETS + 1; i++) {
        uint8_t bytes[31]; memset(bytes, (int)i, sizeof(bytes));
        udp_accept_datagram(h.proxy, &h.client_endpoint, &h.destination, bytes, sizeof(bytes));
    }
    ASSERT_EQ(UDP_QUEUE_PACKETS, f->to_server.count); ASSERT_EQ(UDP_QUEUE_PACKETS * 31u, f->to_server.bytes);
    ASSERT(!(f->client_events & EPOLLIN)); ASSERT(f->upstream_events & EPOLLOUT);
    ASSERT_EQ(1u, h.proxy->stats.failures); ASSERT(!f->retry_eligible);
    fixture_block_fd = -1; udp_flow_event(h.loop, f->upstream_fd, EPOLLOUT, f);
    ASSERT_EQ(0u, f->to_server.count); ASSERT(f->client_events & EPOLLIN);
    for (unsigned i = 0; i < UDP_QUEUE_PACKETS; i++) {
        ASSERT_EQ(31, recv(h.servers[0], received, sizeof(received), 0));
        for (size_t j = 0; j < 31; j++) { ASSERT_EQ(i, received[j]); }
    }
    CHECK_CALL(fixture_done(&h)); PASS();
}

static void stop_loop(mt_loop_t *loop, void *userdata)
{
    bool *done = userdata; *done = true; mt_loop_stop(loop);
}

TEST real_loop_deadline_switches_silent_initial_without_a_second_client_packet(void)
{
    struct fixture h; CHECK_CALL(fixture_init(&h));
    uint8_t bytes[1200], received[1200]; quic_initial(bytes, MT_QUIC_VERSION_1);
    udp_accept_datagram(h.proxy, &h.client_endpoint, &h.destination, bytes, sizeof(bytes));
    ASSERT_EQ(1200, recv(h.servers[0], received, sizeof(received), 0));
    bool done = false;
    ASSERT_EQ(MT_OK, mt_loop_add_timer(h.loop, 70, 0, stop_loop, &done, NULL));
    ASSERT_EQ(MT_OK, mt_loop_run(h.loop)); ASSERT(done);
    ASSERT_EQ(1u, h.proxy->stats.active); ASSERT(h.proxy->flows->alternate);
    ASSERT_EQ(1u, h.ctx.alternate_prepares);
    ASSERT_EQ(1200, recv(h.servers[1], received, sizeof(received), 0));
    ASSERT_EQ(0, memcmp(bytes, received, sizeof(bytes)));
    CHECK_CALL(fixture_done(&h)); PASS();
}

TEST limit_pauses_listener_and_multiflow_teardown_cancels_deadlines(void)
{
    struct fixture h; CHECK_CALL(fixture_init(&h)); h.proxy->config.max_connections = 2;
    uint8_t initial[1200]; quic_initial(initial, MT_QUIC_VERSION_1);
    udp_accept_datagram(h.proxy, &h.client_endpoint, &h.destination, initial, sizeof(initial));
    udp_flow_t *first = h.proxy->flows; ASSERT(first != NULL);
    uint16_t second_port = 0; int second = local_udp(&second_port); ASSERT(second >= 0);
    mt_tproxy_endpoint_t client = h.client_endpoint; client.port = second_port;
    udp_accept_datagram(h.proxy, &client, &h.destination, initial, sizeof(initial));
    ASSERT_EQ(2u, h.proxy->stats.active); ASSERT(!h.proxy->listener4_watched);
    ASSERT_EQ(1u, h.proxy->stats.over_limit);
    udp_flow_free(first, false); ASSERT_EQ(1u, h.proxy->stats.active); ASSERT(h.proxy->listener4_watched);
    udp_accept_datagram(h.proxy, &h.client_endpoint, &h.destination, initial, sizeof(initial));
    ASSERT_EQ(2u, h.proxy->stats.active);
    mt_udp_tproxy_destroy(h.proxy); h.proxy = NULL;
    bool done = false;
    ASSERT_EQ(MT_OK, mt_loop_add_timer(h.loop, 60, 0, stop_loop, &done, NULL));
    ASSERT_EQ(MT_OK, mt_loop_run(h.loop)); ASSERT(done);
    ASSERT_EQ(3u, h.ctx.unpin_calls); ASSERT(!h.ctx.unpin_before_close);
    close(second); CHECK_CALL(fixture_done(&h)); PASS();
}

TEST zero_length_udp_is_a_datagram_not_stream_eof(void)
{
    struct fixture h; CHECK_CALL(fixture_init(&h)); uint8_t bytes[1] = {0};
    udp_accept_datagram(h.proxy, &h.client_endpoint, &h.destination, bytes, 0);
    udp_flow_t *f = h.proxy->flows; ASSERT(f != NULL); ASSERT(!f->retry_eligible);
    struct sockaddr_storage peer; socklen_t length = sizeof(peer);
    ASSERT_EQ(0, server_receive(&h, false, bytes, sizeof(bytes), &peer, &length));
    ASSERT_EQ(0, sendto(h.servers[0], bytes, 0, 0, (const struct sockaddr *)&peer, length));
    udp_flow_event(h.loop, f->upstream_fd, EPOLLIN, f);
    ASSERT_EQ(0, recv(h.client, bytes, sizeof(bytes), 0)); ASSERT(f->server_seen);
    ASSERT_EQ(1u, h.proxy->stats.active); ASSERT_EQ(0u, h.ctx.direct_learned);
    CHECK_CALL(fixture_done(&h)); PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(listeners_bind_loopback_and_creation_failures_close_resources);
    RUN_TEST(original_destination_ancillary_data_is_required_and_validated);
    RUN_TEST(initial_only_timeout_retries_once_and_keeps_reply_socket);
    RUN_TEST(queued_server_datagram_wins_over_simultaneous_timeout);
    RUN_TEST(queued_response_prevents_send_error_retry_even_with_pending_error_and_backpressure);
    RUN_TEST(token_early_data_and_opaque_datagrams_are_forwarded_once_without_retry);
    RUN_TEST(later_early_data_and_changed_cids_permanently_disable_retry);
    RUN_TEST(replay_budget_exhaustion_keeps_the_direct_flow);
    RUN_TEST(pin_failure_prevents_upstream_io_and_release_closes_before_unpin);
    RUN_TEST(outgoing_creation_or_prepare_failure_uses_one_alternate_attempt);
    RUN_TEST(learning_requires_client_progress_and_correlated_server_response);
    RUN_TEST(established_client_socket_and_upstream_source_stay_pinned_after_learning);
    RUN_TEST(bounded_eagain_queue_keeps_datagram_boundaries_and_backpressure);
    RUN_TEST(real_loop_deadline_switches_silent_initial_without_a_second_client_packet);
    RUN_TEST(limit_pauses_listener_and_multiflow_teardown_cancels_deadlines);
    RUN_TEST(zero_length_udp_is_a_datagram_not_stream_eof);
    GREATEST_MAIN_END();
}
