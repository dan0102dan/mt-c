/* Include the actual transport response/request handlers. Socket delivery is
 * captured; all parsing, filtering, framing and cleanup use production code. */
#include "../../src/dns/proxy.c"
#include "greatest.h"

mt_err_t __real_mt_dns_msg_pack(const mt_dns_msg_t *msg, uint8_t **out, size_t *len);
mt_err_t __real_mt_dns_msg_parse(const uint8_t *data, size_t len, mt_dns_msg_t **out);
mt_err_t __wrap_mt_dns_msg_pack(const mt_dns_msg_t *msg, uint8_t **out, size_t *len);
mt_err_t __wrap_mt_dns_msg_parse(const uint8_t *data, size_t len, mt_dns_msg_t **out);
mt_err_t __real_mt_loop_add_timer(mt_loop_t *loop, uint64_t initial_ms,
                                 uint64_t interval_ms, mt_timer_cb cb,
                                 void *ud, int *out_id);
mt_err_t __real_mt_loop_add_reusable_timer(mt_loop_t *loop, uint64_t initial_ms,
                                          uint64_t interval_ms, mt_timer_cb cb,
                                          void *ud, int *out_id);
mt_err_t __wrap_mt_loop_add_timer(mt_loop_t *loop, uint64_t initial_ms,
                                 uint64_t interval_ms, mt_timer_cb cb,
                                 void *ud, int *out_id);
mt_err_t __wrap_mt_loop_add_reusable_timer(mt_loop_t *loop, uint64_t initial_ms,
                                          uint64_t interval_ms, mt_timer_cb cb,
                                          void *ud, int *out_id);
ssize_t __wrap_sendmsg(int fd, const struct msghdr *msg, int flags);
static unsigned packs, parses, callbacks, original_aaaa;
static unsigned ordinary_timers, reusable_timers;
static unsigned fail_ordinary_at, fail_reusable_at;
static bool fail_pack;
static uint8_t delivered[MT_DNS_MAX_MSG + 2];
static size_t delivered_len;
mt_err_t __wrap_mt_dns_msg_pack(const mt_dns_msg_t *msg, uint8_t **out, size_t *len)
{
    packs++;
    if (fail_pack) { return MT_ERR_NOMEM; }
    return __real_mt_dns_msg_pack(msg, out, len);
}
mt_err_t __wrap_mt_dns_msg_parse(const uint8_t *data, size_t len, mt_dns_msg_t **out)
{ parses++; return __real_mt_dns_msg_parse(data, len, out); }
mt_err_t __wrap_mt_loop_add_timer(mt_loop_t *loop, uint64_t initial_ms,
                                 uint64_t interval_ms, mt_timer_cb cb,
                                 void *ud, int *out_id)
{
    ordinary_timers++;
    if (ordinary_timers == fail_ordinary_at) { return MT_ERR_NOMEM; }
    return __real_mt_loop_add_timer(loop, initial_ms, interval_ms, cb, ud, out_id);
}
mt_err_t __wrap_mt_loop_add_reusable_timer(mt_loop_t *loop, uint64_t initial_ms,
                                          uint64_t interval_ms, mt_timer_cb cb,
                                          void *ud, int *out_id)
{
    reusable_timers++;
    if (reusable_timers == fail_reusable_at) { return MT_ERR_NOMEM; }
    return __real_mt_loop_add_reusable_timer(loop, initial_ms, interval_ms, cb,
                                            ud, out_id);
}
ssize_t __wrap_sendmsg(int fd, const struct msghdr *msg, int flags)
{
    (void)fd; (void)flags;
    if (msg->msg_iovlen != 1 || msg->msg_iov[0].iov_len > sizeof(delivered)) { abort(); }
    delivered_len = msg->msg_iov[0].iov_len;
    memcpy(delivered, msg->msg_iov[0].iov_base, delivered_len);
    return (ssize_t)delivered_len;
}
static void response_hook(const mt_dns_msg_t *msg, const char *client,
                           const char *network, void *ud)
{
    (void)client; (void)network; (void)ud; callbacks++;
    for (size_t i = 0; i < msg->n_answers; i++) {
        if (msg->answers[i].rtype == MT_DNS_TYPE_AAAA) { original_aaaa++; }
    }
}
static void reset(void)
{
    packs = parses = callbacks = original_aaaa = 0;
    ordinary_timers = reusable_timers = fail_ordinary_at = fail_reusable_at = 0;
    delivered_len = 0; fail_pack = false;
}
static size_t response(uint8_t *buf, bool aaaa, bool additional, bool trailing)
{
    static const uint8_t a[] = {
        0x12,0x34,0x81,0x80,0,1,0,1,0,0,0,0,
        1,'a',4,'t','e','s','t',0, 0,1,0,1,
        0xc0,12,0,1,0,1,0,0,0,60,0,4,192,0,2,1
    };
    memcpy(buf, a, sizeof(a)); size_t n = sizeof(a);
    if (aaaa) {
        static const uint8_t v6[] = {0xc0,12,0,28,0,1,0,0,0,60,0,16,0x20,1,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,1};
        memcpy(buf + n, v6, sizeof(v6)); n += sizeof(v6);
        if (additional) { buf[11] = 1; } else { buf[7] = 2; }
    }
    if (trailing) { buf[n++] = 0xde; buf[n++] = 0xad; }
    return n;
}
static exchange_t *exchange(mt_dnsproxy_t *p, bool tcp)
{
    exchange_t *ex = calloc(1, sizeof(*ex)); if (!ex) { abort(); }
    ex->p = p; ex->is_tcp = tcp; ex->upstream_fd = ex->client_fd = -1;
    p->exchanges = ex; atomic_store(&p->inflight, 1); return ex;
}
static int run_response(const uint8_t *buf, size_t len, bool tcp, bool disabled,
                        bool expect_drop, unsigned expected_packs)
{
    mt_loop_t *loop = NULL; ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    mt_dnsproxy_t p = {.loop = loop, .cb = response_hook, .udp_fd = -1};
    p.cfg.disable_drop_aaaa = disabled;
    exchange_t *ex = exchange(&p, tcp);
    deliver_response(ex, buf, len);
    ASSERT_EQ(expected_packs, packs); ASSERT_EQ(1u, parses);
    if (tcp && !expect_drop) {
        ASSERT(p.exchanges == ex); ASSERT(ex->out_len >= 2);
        ASSERT_EQ(ex->out_len - 2, ((size_t)ex->out[0] << 8) | ex->out[1]);
        delivered_len = ex->out_len - 2; memcpy(delivered, ex->out + 2, delivered_len);
        exchange_finish(ex, false);
    }
    ASSERT(p.exchanges == NULL); ASSERT_EQ(0u, mt_dnsproxy_inflight(&p));
    if (expect_drop) { ASSERT_EQ(0u, delivered_len); }
    else { ASSERT(delivered_len >= MT_DNS_HEADER_LEN); }
    mt_loop_destroy(loop); return GREATEST_TEST_RES_PASS;
}
static greatest_test_res assert_baseline_pack(const uint8_t *buf, size_t len)
{
    mt_dns_msg_t *parsed = NULL;
    ASSERT_EQ(MT_OK, __real_mt_dns_msg_parse(buf, len, &parsed));
    uint8_t *packed = NULL;
    size_t packed_len = 0;
    ASSERT_EQ(MT_OK, __real_mt_dns_msg_pack(parsed, &packed, &packed_len));
    ASSERT_EQ(packed_len, delivered_len);
    ASSERT_EQ(0, memcmp(packed, delivered, packed_len));
    free(packed); mt_dns_msg_free(parsed);
    PASS();
}
TEST unchanged_udp_is_verbatim_and_tcp_retains_baseline_filtering_pack(void)
{
    for (unsigned tcp = 0; tcp < 2; tcp++) {
        for (unsigned disabled = 0; disabled < 2; disabled++) {
            for (unsigned trailing = 0; trailing < 2; trailing++) {
                uint8_t buf[128]; size_t n = response(buf, false, false, trailing != 0);
                bool normalized = tcp && !disabled;
                reset(); fail_pack = !normalized;
                ASSERT_EQ(GREATEST_TEST_RES_PASS,
                    run_response(buf, n, tcp != 0, disabled != 0, false, normalized ? 1u : 0u));
                ASSERT_EQ(1u, callbacks);
                if (normalized) {
                    CHECK_CALL(assert_baseline_pack(buf, n));
                    ASSERT(delivered_len <= n); /* preserve compression when repacking */
                } else {
                    ASSERT_EQ(n, delivered_len); ASSERT_EQ(0, memcmp(buf, delivered, n));
                }
            }
        }
    }
    PASS();
}
TEST answer_filter_keeps_original_hook_and_additional_records(void)
{
    for (unsigned tcp = 0; tcp < 2; tcp++) {
        for (unsigned disabled = 0; disabled < 2; disabled++) {
            for (unsigned additional = 0; additional < 2; additional++) {
                uint8_t buf[128]; size_t n = response(buf, true, additional != 0, false);
                reset(); bool modified = !disabled && !additional;
                bool normalized = !disabled && (tcp || !additional);
                ASSERT_EQ(GREATEST_TEST_RES_PASS, run_response(buf, n, tcp != 0, disabled != 0, false, normalized ? 1u : 0u));
                ASSERT_EQ(1u, callbacks); ASSERT_EQ(additional ? 0u : 1u, original_aaaa);
                mt_dns_msg_t *parsed = NULL;
                ASSERT_EQ(MT_OK, __real_mt_dns_msg_parse(delivered, delivered_len, &parsed));
                ASSERT_EQ(additional || modified ? 1u : 2u, parsed->n_answers);
                ASSERT_EQ(additional ? 1u : 0u, parsed->n_additional);
                ASSERT_EQ(60u, parsed->answers[0].ttl); ASSERT_EQ(1u, parsed->answers[0].rclass);
                if (!modified && normalized) { CHECK_CALL(assert_baseline_pack(buf, n)); }
                if (!normalized) { ASSERT_EQ(n, delivered_len); ASSERT_EQ(0, memcmp(buf, delivered, n)); }
                mt_dns_msg_free(parsed);
            }
        }
    }
    PASS();
}
TEST compressed_cname_multi_a_and_authority_preserve_transport_policy(void)
{
    /* CNAME target at offset 40, authority NS target at offset 93. Both
     * answer owners and the additional AAAA owner use compression pointers. */
    static const uint8_t buf[] = {
        0x12,0x34,0x81,0x80,0,1,0,3,0,1,0,1,
        5,'a','l','i','a','s',4,'t','e','s','t',0,0,1,0,1,
        0xc0,12,0,5,0,1,0,0,0,60,0,9,6,'t','a','r','g','e','t',0xc0,18,
        0xc0,40,0,1,0,1,0,0,0,30,0,4,192,0,2,1,
        0xc0,40,0,1,0,1,0,0,0,15,0,4,192,0,2,2,
        0xc0,18,0,2,0,1,0,0,0,90,0,5,2,'n','s',0xc0,18,
        0xc0,93,0,28,0,1,0,0,0,90,0,16,
        0x20,1,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,1
    };
    for (unsigned tcp = 0; tcp < 2; tcp++) {
      for (unsigned disabled = 0; disabled < 2; disabled++) {
        bool normalized = tcp && !disabled;
        reset(); fail_pack = !normalized;
        ASSERT_EQ(GREATEST_TEST_RES_PASS,
            run_response(buf, sizeof(buf), tcp != 0, disabled != 0, false, normalized ? 1u : 0u));
        ASSERT_EQ(1u, callbacks); ASSERT_EQ(0u, original_aaaa);
        if (normalized) {
            CHECK_CALL(assert_baseline_pack(buf, sizeof(buf)));
        } else {
            ASSERT_EQ(sizeof(buf), delivered_len);
            ASSERT_EQ(0, memcmp(buf, delivered, sizeof(buf)));
        }
        mt_dns_msg_t *parsed = NULL;
        ASSERT_EQ(MT_OK, __real_mt_dns_msg_parse(delivered, delivered_len, &parsed));
        ASSERT_EQ(3u, parsed->n_answers); ASSERT_EQ(1u, parsed->n_authority);
        ASSERT_EQ(1u, parsed->n_additional);
        ASSERT_EQ(MT_DNS_TYPE_CNAME, parsed->answers[0].rtype);
        ASSERT_EQ(MT_DNS_TYPE_A, parsed->answers[1].rtype);
        ASSERT_EQ(MT_DNS_TYPE_A, parsed->answers[2].rtype);
        ASSERT_EQ(MT_DNS_TYPE_NS, parsed->authority[0].rtype);
        ASSERT_EQ(MT_DNS_TYPE_AAAA, parsed->additional[0].rtype);
        ASSERT_EQ(30u, parsed->answers[1].ttl); ASSERT_EQ(15u, parsed->answers[2].ttl);
        ASSERT_EQ(1u, parsed->answers[1].rdata[3]); ASSERT_EQ(2u, parsed->answers[2].rdata[3]);
        mt_dns_msg_free(parsed);
      }
    }
    PASS();
}

static size_t repeated_answer_response(uint8_t *buf, size_t answer_count,
                                       bool add_aaaa)
{
    size_t p = 0;
    const uint16_t count = (uint16_t)(answer_count + (add_aaaa ? 1u : 0u));
    uint8_t header[] = {0x12,0x34,0x81,0x80,0,1,
                        (uint8_t)(count >> 8), (uint8_t)count,0,0,0,0};
    memcpy(buf + p, header, sizeof(header)); p += sizeof(header);
    static const uint8_t qname[] = {
        18,'l','o','n','g','-','s','e','r','v','i','c','e','-','l','a','b','e','l',
        7,'e','x','a','m','p','l','e',3,'c','o','m',0
    };
    memcpy(buf + p, qname, sizeof(qname)); p += sizeof(qname);
    buf[p++] = 0; buf[p++] = 1; buf[p++] = 0; buf[p++] = 1;
    static const uint8_t answer[] = {
        0xc0,12,0,1,0,1,0,0,0,60,0,4,192,0,2,1
    };
    for (size_t i = 0; i < answer_count; i++) {
        memcpy(buf + p, answer, sizeof(answer));
        buf[p + 15] = (uint8_t)(i + 1);
        p += sizeof(answer);
    }
    if (add_aaaa) {
        static const uint8_t v6[] = {
            0xc0,12,0,28,0,1,0,0,0,60,0,16,0x20,1,0x0d,0xb8,
            0,0,0,0,0,0,0,0,0,0,0,1
        };
        memcpy(buf + p, v6, sizeof(v6)); p += sizeof(v6);
    }
    return p;
}
TEST compressed_large_replies_survive_real_udp_filter_and_tcp_repacking(void)
{
    uint8_t *buf = malloc(33000);
    ASSERT(buf != NULL);
    for (unsigned tcp = 0; tcp < 2; tcp++) {
        const size_t count = tcp ? 2000 : 20;
        size_t n = repeated_answer_response(buf, count, tcp == 0);
        ASSERT(n < 33000);
        reset();
        ASSERT_EQ(GREATEST_TEST_RES_PASS,
                  run_response(buf, n, tcp != 0, false, false, 1));
        ASSERT_EQ(1u, callbacks);
        ASSERT_EQ(tcp ? 0u : 1u, original_aaaa);
        if (!tcp) { ASSERT(delivered_len <= 512); }
        mt_dns_msg_t *parsed = NULL;
        ASSERT_EQ(MT_OK, __real_mt_dns_msg_parse(delivered, delivered_len, &parsed));
        ASSERT_EQ(count, parsed->n_answers);
        ASSERT_EQ(MT_DNS_TYPE_A, parsed->answers[count - 1].rtype);
        ASSERT_EQ((uint8_t)count, parsed->answers[count - 1].rdata[3]);
        mt_dns_msg_free(parsed);
    }
    free(buf);
    PASS();
}

TEST malformed_and_pack_failure_drop_and_release_exchange(void)
{
    for (unsigned tcp = 0; tcp < 2; tcp++) {
        uint8_t buf[128]; size_t n = response(buf, true, false, false);
        reset(); fail_pack = true;
        ASSERT_EQ(GREATEST_TEST_RES_PASS, run_response(buf, n, tcp != 0, false, true, 1));
        ASSERT_EQ(1u, callbacks); ASSERT_EQ(1u, original_aaaa);
        reset();
        ASSERT_EQ(GREATEST_TEST_RES_PASS, run_response(buf, n - 1, tcp != 0, false, true, 0));
        ASSERT_EQ(0u, callbacks);
        reset(); buf[12] = 0xc0; buf[13] = 12; /* compression cycle */
        ASSERT_EQ(GREATEST_TEST_RES_PASS, run_response(buf, n, tcp != 0, false, true, 0));
        ASSERT_EQ(0u, callbacks);
    }
    uint8_t a[128]; size_t n = response(a, false, false, false);
    reset(); fail_pack = true;
    ASSERT_EQ(GREATEST_TEST_RES_PASS, run_response(a, n, true, false, true, 1));
    ASSERT_EQ(1u, callbacks); ASSERT_EQ(0u, original_aaaa);
    PASS();
}
TEST header_only_response_retains_historical_normalization(void)
{
    uint8_t buf[12] = {0x12,0x34,0x81,0x80,0,1,0,1,0,1,0,1};
    for (unsigned tcp = 0; tcp < 2; tcp++) {
        reset(); ASSERT_EQ(GREATEST_TEST_RES_PASS, run_response(buf, sizeof(buf), tcp != 0, false, false, 1));
        ASSERT_EQ(12u, delivered_len);
        for (size_t i = 4; i < 12; i++) { ASSERT_EQ(0u, delivered[i]); }
    }
    PASS();
}
static int bound_endpoint(int type, struct sockaddr_in *address)
{
    int fd = socket(AF_INET, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { return -1; }
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = (socklen_t)sizeof(*address);
    if (bind(fd, (struct sockaddr *)address, len) != 0 ||
        getsockname(fd, (struct sockaddr *)address, &len) != 0 ||
        (type == SOCK_STREAM && listen(fd, 4) != 0)) {
        close(fd); return -1;
    }
    return fd;
}
static void set_upstream(mt_dnsproxy_t *p, const struct sockaddr_in *address)
{
    p->family = AF_INET;
    memcpy(&p->upstream_sa, address, sizeof(*address));
    p->upstream_sa_len = (socklen_t)sizeof(*address);
    p->cfg.timeout_ms = 60000;
    p->cfg.max_concurrent = 8;
}
TEST upstream_deadline_selects_reuse_only_for_udp_and_releases_on_failure(void)
{
    static const uint8_t query[] = {0x12,0x34,1,0,0,1,0,0,0,0,0,0,1,'a',0,0,1,0,1};
    for (unsigned tcp = 0; tcp < 2; tcp++) {
        for (unsigned fail = 0; fail < 2; fail++) {
            struct sockaddr_in address;
            int upstream = bound_endpoint(tcp ? SOCK_STREAM : SOCK_DGRAM, &address);
            ASSERT(upstream >= 0);
            mt_loop_t *loop = NULL; ASSERT_EQ(MT_OK, mt_loop_create(&loop));
            mt_dnsproxy_t p = {.loop = loop, .udp_fd = -1};
            set_upstream(&p, &address);
            exchange_t *ex = exchange(&p, tcp != 0);
            ex->req = malloc(sizeof(query)); ASSERT(ex->req != NULL);
            memcpy(ex->req, query, sizeof(query)); ex->req_len = sizeof(query);
            reset();
            if (fail) {
                if (tcp) { fail_ordinary_at = 1; } else { fail_reusable_at = 1; }
            }
            start_upstream(&p, ex);
            ASSERT_EQ(tcp ? 1u : 0u, ordinary_timers);
            ASSERT_EQ(tcp ? 0u : 1u, reusable_timers);
            if (!fail) {
                ASSERT(p.exchanges == ex); ASSERT(ex->timer_id > 0);
                ASSERT(ex->upstream_fd >= 0);
                exchange_finish(ex, false);
            }
            ASSERT(p.exchanges == NULL); ASSERT_EQ(0u, mt_dnsproxy_inflight(&p));
            close(upstream); mt_loop_destroy(loop);
        }
    }
    PASS();
}
static greatest_test_res check_tcp_deadlines(bool ptr, unsigned fail_at)
{
    struct sockaddr_in upstream_address, client_address;
    int upstream = bound_endpoint(SOCK_STREAM, &upstream_address);
    int listener = bound_endpoint(SOCK_STREAM, &client_address);
    ASSERT(upstream >= 0); ASSERT(listener >= 0);
    int client = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT(client >= 0);
    ASSERT_EQ(0, connect(client, (struct sockaddr *)&client_address,
                         (socklen_t)sizeof(client_address)));
    mt_loop_t *loop = NULL; ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    mt_dnsproxy_t p = {.loop = loop, .udp_fd = -1};
    set_upstream(&p, &upstream_address);
    reset(); fail_ordinary_at = fail_at;
    on_tcp_accept(loop, listener, EPOLLIN, &p);
    ASSERT_EQ(1u, ordinary_timers); ASSERT_EQ(0u, reusable_timers);
    if (fail_at == 1) {
        ASSERT(p.exchanges == NULL); ASSERT_EQ(0u, mt_dnsproxy_inflight(&p));
    } else {
        exchange_t *ex = p.exchanges; ASSERT(ex != NULL);
        ASSERT(ex->timer_id > 0); int read_timer = ex->timer_id;
        uint8_t frame[] = {0,19,0x12,0x34,1,0,0,1,0,0,0,0,0,0,1,'a',0,0,1,0,1};
        if (ptr) { frame[18] = MT_DNS_TYPE_PTR; }
        ASSERT_EQ(1, send(client, frame, 1, MSG_NOSIGNAL));
        on_tcp_client_read(loop, ex->client_fd, EPOLLIN, ex);
        ASSERT(p.exchanges == ex); ASSERT_EQ(read_timer, ex->timer_id);
        ASSERT_EQ(1u, ordinary_timers); ASSERT_EQ(0u, reusable_timers);
        ASSERT_EQ((ssize_t)(sizeof(frame) - 1),
                  send(client, frame + 1, sizeof(frame) - 1, MSG_NOSIGNAL));
        on_tcp_client_read(loop, ex->client_fd, EPOLLIN, ex);
        ASSERT_EQ(2u, ordinary_timers); ASSERT_EQ(0u, reusable_timers);
        if (fail_at == 2) {
            ASSERT(p.exchanges == NULL);
        } else {
            ASSERT(p.exchanges == ex); ASSERT(ex->timer_id != read_timer);
            ASSERT(ex->timer_id > 0);
            if (ptr) { ASSERT(ex->out_len > 2); ASSERT_EQ(-1, ex->upstream_fd); }
            else { ASSERT(ex->upstream_fd >= 0); ASSERT_EQ(0u, ex->out_len); }
            exchange_finish(ex, false);
        }
        ASSERT(p.exchanges == NULL); ASSERT_EQ(0u, mt_dnsproxy_inflight(&p));
    }
    close(client); close(listener); close(upstream); mt_loop_destroy(loop);
    PASS();
}
TEST tcp_accept_partial_read_and_processing_keep_ordinary_deadlines(void)
{
    CHECK_CALL(check_tcp_deadlines(false, 0));
    CHECK_CALL(check_tcp_deadlines(true, 0));
    PASS();
}
TEST tcp_read_and_processing_deadline_failures_release_exchange(void)
{
    CHECK_CALL(check_tcp_deadlines(false, 1));
    CHECK_CALL(check_tcp_deadlines(false, 2));
    PASS();
}
TEST parsed_request_decides_ptr_without_a_second_detection_parse(void)
{
    mt_loop_t *loop = NULL; ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    mt_dnsproxy_t p = {.loop = loop};
    uint8_t query[] = {0x12,0x34,1,0,0,1,0,0,0,0,0,0,1,'a',0,0,1,0,1};
    exchange_t ex = {.p = &p, .req = query, .req_len = sizeof(query),
                     .is_tcp = true, .client_fd = -1, .upstream_fd = -1};
    reset(); ASSERT_EQ(REQ_FORWARD, handle_request_common(&p, &ex)); ASSERT_EQ(1u, parses);
    query[16] = 12; p.cfg.disable_fake_ptr = true;
    reset(); ASSERT_EQ(REQ_FORWARD, handle_request_common(&p, &ex)); ASSERT_EQ(1u, parses);
    p.cfg.disable_fake_ptr = false; reset();
    ASSERT_EQ(REQ_REPLIED_ASYNC, handle_request_common(&p, &ex));
    mt_dns_msg_t *ptr = NULL; ASSERT(ex.out_len > 2);
    ASSERT_EQ(MT_OK, __real_mt_dns_msg_parse(ex.out + 2, ex.out_len - 2, &ptr));
    ASSERT_EQ(3u, ptr->flags & 15u); ASSERT(ptr->flags & MT_DNS_FLAG_RA);
    ASSERT_EQ(1u, ptr->n_questions); ASSERT_EQ(12u, ptr->questions[0].qtype);
    free(ex.out); ex.out = NULL; mt_dns_msg_free(ptr);
    ex.req_len--; reset(); ASSERT_EQ(REQ_DROP, handle_request_common(&p, &ex));
    ASSERT_EQ(1u, parses); mt_loop_destroy(loop); PASS();
}
TEST ptr_eligibility_preserves_qr_class_and_question_count(void)
{
    mt_loop_t *loop = NULL; ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    mt_dnsproxy_t p = {.loop = loop, .udp_fd = -1};
    for (unsigned tcp = 0; tcp < 2; tcp++) {
        for (unsigned qr = 0; qr < 2; qr++) {
            for (unsigned non_in = 0; non_in < 2; non_in++) {
                uint8_t query[] = {0x12,0x34,1,0,0,1,0,0,0,0,0,0,1,'a',0,0,12,0,1};
                query[2] = qr ? 0x81 : 1;
                query[18] = non_in ? 3 : 1;
                exchange_t ex = {.p = &p, .req = query, .req_len = sizeof(query),
                                 .is_tcp = tcp != 0, .client_fd = -1, .upstream_fd = -1};
                reset();
                ASSERT_EQ(tcp ? REQ_REPLIED_ASYNC : REQ_DROP, handle_request_common(&p, &ex));
                ASSERT_EQ(1u, parses);
                ASSERT_EQ(0u, ordinary_timers); ASSERT_EQ(0u, reusable_timers);
                const uint8_t *wire = tcp ? ex.out + 2 : delivered;
                size_t len = tcp ? ex.out_len - 2 : delivered_len;
                mt_dns_msg_t *ptr = NULL;
                ASSERT_EQ(MT_OK, __real_mt_dns_msg_parse(wire, len, &ptr));
                ASSERT_EQ(MT_DNS_FLAG_QR | MT_DNS_FLAG_RA | MT_DNS_RCODE_NXDOMAIN, ptr->flags);
                ASSERT_EQ(1u, ptr->n_questions); ASSERT_EQ(MT_DNS_TYPE_PTR, ptr->questions[0].qtype);
                ASSERT_EQ(non_in ? 3u : 1u, ptr->questions[0].qclass);
                mt_dns_msg_free(ptr); free(ex.out);
            }
        }
        uint8_t query[26] = {0x12,0x34,1,0,0,2,0,0,0,0,0,0,1,'a',0,0,12,0,1};
        memcpy(query + 19, query + 12, 7);
        exchange_t ex = {.p = &p, .req = query, .req_len = sizeof(query),
                         .is_tcp = tcp != 0, .client_fd = -1, .upstream_fd = -1};
        reset(); ASSERT_EQ(REQ_FORWARD, handle_request_common(&p, &ex));
        ASSERT_EQ(1u, parses); ASSERT(ex.out == NULL); ASSERT_EQ(0u, delivered_len);
        query[5] = 1; ex.req_len = 12;
        reset(); ASSERT_EQ(REQ_FORWARD, handle_request_common(&p, &ex));
        ASSERT_EQ(1u, parses); ASSERT(ex.out == NULL); ASSERT_EQ(0u, delivered_len);
        ex.req_len = 18; /* incomplete QCLASS still drops, before PTR handling */
        reset(); ASSERT_EQ(REQ_DROP, handle_request_common(&p, &ex));
        ASSERT_EQ(1u, parses); ASSERT(ex.out == NULL); ASSERT_EQ(0u, delivered_len);
    }
    mt_loop_destroy(loop); PASS();
}
TEST idle_socket_pool_grows_only_on_demand_for_large_limit(void)
{
    sock_pool_t p = {0};
    pool_init(&p, UINT32_MAX);
    ASSERT(p.fds == NULL);
    ASSERT_EQ(0u, p.allocated);
    int sockets[9];
    for (size_t i = 0; i < 9; i++) {
        sockets[i] = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        ASSERT(sockets[i] >= 0);
        pool_put(&p, sockets[i]);
    }
    ASSERT_EQ(9u, p.len);
    ASSERT(p.allocated >= 9u && p.allocated < 32u);
    for (size_t i = 9; i > 0; i--) {
        ASSERT_EQ(sockets[i - 1], pool_get(&p));
        close(sockets[i - 1]);
    }
    pool_clear(&p);
    ASSERT_EQ(0u, p.allocated);
    pool_init(&p, 0);
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    ASSERT(fd >= 0);
    pool_put(&p, fd);
    ASSERT_EQ(0u, p.len);
    ASSERT(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    pool_clear(&p);
    PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(idle_socket_pool_grows_only_on_demand_for_large_limit);
    RUN_TEST(unchanged_udp_is_verbatim_and_tcp_retains_baseline_filtering_pack);
    RUN_TEST(answer_filter_keeps_original_hook_and_additional_records);
    RUN_TEST(compressed_cname_multi_a_and_authority_preserve_transport_policy);
    RUN_TEST(compressed_large_replies_survive_real_udp_filter_and_tcp_repacking);
    RUN_TEST(malformed_and_pack_failure_drop_and_release_exchange);
    RUN_TEST(header_only_response_retains_historical_normalization);
    RUN_TEST(upstream_deadline_selects_reuse_only_for_udp_and_releases_on_failure);
    RUN_TEST(tcp_accept_partial_read_and_processing_keep_ordinary_deadlines);
    RUN_TEST(tcp_read_and_processing_deadline_failures_release_exchange);
    RUN_TEST(parsed_request_decides_ptr_without_a_second_detection_parse);
    RUN_TEST(ptr_eligibility_preserves_qr_class_and_question_count);
    GREATEST_MAIN_END();
}
