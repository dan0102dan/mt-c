/* Compare response batching with the historical per-record stream, and
 * exercise the actual daemon sink's failure budget. No kernel privileges. */
#include "greatest.h"

int mt_pipeline_test_main(int argc, char **argv);
#define main mt_pipeline_test_main
#include "../../src/main/main.c"
#undef main

enum { MAX_ACTIONS = 4096 };
typedef struct saved_action {
    mt_id_t group;
    uint8_t addr[16], len, attempts;
    uint32_t ttl;
    char domain[1100], matched[1100], kind[8];
} saved_action_t;
typedef struct capture {
    saved_action_t rows[MAX_ACTIONS];
    size_t n, represented;
    bool overflow;
} capture_t;
static capture_t actual, reference;

static void capture_action(const mt_match_action_t *a, void *ud)
{
    capture_t *c = ud;
    if (c->n == MAX_ACTIONS) { c->overflow = true; return; }
    saved_action_t *s = &c->rows[c->n++];
    s->group = a->group_id; s->len = a->addr_len;
    memcpy(s->addr, a->addr, a->addr_len);
    s->ttl = a->ttl_seconds; s->attempts = a->attempts;
    snprintf(s->domain, sizeof(s->domain), "%s", a->record_domain);
    snprintf(s->matched, sizeof(s->matched), "%s", a->matched_name);
    snprintf(s->kind, sizeof(s->kind), "%s", a->record_kind);
    c->represented += a->attempts;
}
static void reset_capture(capture_t *c)
{
    c->n = c->represented = 0; c->overflow = false;
}
static size_t make_name(uint8_t *out, const char *name)
{
    size_t pos = 0;
    while (*name) {
        const char *dot = strchr(name, '.');
        size_t n = dot ? (size_t)(dot - name) : strlen(name);
        if (!n || n > 63 || pos + n + 2 > MT_DNS_MAX_NAME) { abort(); }
        out[pos++] = (uint8_t)n;
        memcpy(out + pos, name, n); pos += n;
        name += n; if (*name == '.') { name++; }
    }
    out[pos++] = 0; return pos;
}
static mt_dns_rr_t rr(const char *name, uint16_t type, uint32_t ttl,
                       uint8_t *data, size_t len)
{
    mt_dns_rr_t r = {.rtype = type, .rclass = 1, .ttl = ttl,
                     .rdata = data, .rdata_len = len};
    r.name_len = make_name(r.name, name); return r;
}
static mt_ruleset_snapshot_t *snapshot(size_t groups, const char *pattern)
{
    mt_ruleset_snapshot_t *s = calloc(1, sizeof(*s));
    if (!s) { abort(); }
    s->groups = calloc(groups ? groups : 1, sizeof(*s->groups));
    if (!s->groups) { abort(); }
    for (size_t i = 0; i < groups; i++) {
        mt_group_snapshot_t *g = calloc(1, sizeof(*g));
        if (!g) { abort(); }
        g->id.b[3] = (uint8_t)(i + 1);
        g->name = strdup("group"); g->matcher = mt_matcher_new();
        if (!g->name || !g->matcher ||
            mt_matcher_add(g->matcher, MT_RULE_WILDCARD, pattern) != MT_OK) { abort(); }
        s->groups[s->n_groups++] = g;
    }
    return s;
}
static mt_dns_pipeline_t *pipeline(mt_cache_t **cache, capture_t *sink,
                                    size_t groups, uint32_t extra)
{
    *cache = mt_cache_create(0);
    mt_dns_pipeline_t *p = mt_dns_pipeline_create(*cache, extra,
                                                 sink ? capture_action : NULL, sink);
    if (!*cache || !p) { abort(); }
    mt_dns_pipeline_set_snapshot(p, snapshot(groups, "*"));
    return p;
}
static void stream_records(mt_dns_pipeline_t *p, const mt_dns_msg_t *msg, int64_t now)
{
    for (size_t i = 0; i < msg->n_answers; i++) {
        mt_dns_msg_t one = *msg;
        one.answers = &msg->answers[i]; one.n_answers = 1;
        mt_dns_pipeline_handle_message(p, &one, now);
    }
}
static bool same_key(const saved_action_t *a, const saved_action_t *b)
{
    return mt_id_equal(a->group, b->group) && a->len == b->len &&
           memcmp(a->addr, b->addr, a->len) == 0;
}
static const saved_action_t *last_key(const capture_t *c, const saved_action_t *key)
{
    for (size_t i = c->n; i > 0; i--) {
        if (same_key(&c->rows[i - 1], key)) { return &c->rows[i - 1]; }
    }
    return NULL;
}
static int equivalent(void)
{
    ASSERT(!actual.overflow && !reference.overflow);
    ASSERT_EQ(reference.n, actual.represented);
    ASSERT(actual.n <= reference.n);
    for (size_t i = 0; i < reference.n; i++) {
        const saved_action_t *r = &reference.rows[i];
        if (last_key(&reference, r) != r) { continue; }
        const saved_action_t *a = last_key(&actual, r);
        ASSERT(a); ASSERT_EQ(r->ttl, a->ttl);
        ASSERT_STR_EQ(r->domain, a->domain);
        ASSERT_STR_EQ(r->matched, a->matched); ASSERT_STR_EQ(r->kind, a->kind);
    }
    for (size_t i = 0; i < actual.n; i++) {
        ASSERT(last_key(&reference, &actual.rows[i]));
        ASSERT(actual.rows[i].attempts >= 1 && actual.rows[i].attempts <= 32);
    }
    return GREATEST_TEST_RES_PASS;
}

TEST warm_cname_last_ttl_and_independent_groups(void)
{
    mt_cache_t *ca, *cr;
    mt_dns_pipeline_t *pa = pipeline(&ca, &actual, 2, 3600);
    mt_dns_pipeline_t *pr = pipeline(&cr, &reference, 2, 3600);
    uint8_t middle[256], target[256], ip[4][4] = {{192,0,2,1},{192,0,2,2},{192,0,2,3},{192,0,2,4}};
    size_t ml = make_name(middle, "middle.test"), tl = make_name(target, "target.test");
    mt_dns_rr_t records[6] = {rr("alias.test", 5, 300, middle, ml), rr("middle.test", 5, 300, target, tl)};
    for (size_t i = 0; i < 4; i++) { records[i + 2] = rr("target.test", 1, 300, ip[i], 4); }
    mt_dns_msg_t msg = {.answers = records, .n_answers = 6};
    for (size_t pass = 0; pass < 3; pass++) {
        if (pass == 1) { for (size_t i = 2; i < 6; i++) { records[i].ttl = 60; } }
        if (pass == 2) {
            mt_dns_pipeline_set_additional_ttl(pa, 0); mt_dns_pipeline_set_additional_ttl(pr, 0);
            for (size_t i = 2; i < 6; i++) { records[i].ttl = 0; }
        }
        reset_capture(&actual); reset_capture(&reference);
        mt_dns_pipeline_handle_message(pa, &msg, 1000);
        stream_records(pr, &msg, 1000);
        CHECK_CALL(equivalent());
        ASSERT_EQ(8u, actual.n);
        ASSERT_EQ(pass ? 24u : 8u, reference.n);
        for (size_t i = 0; i < actual.n; i++) {
            ASSERT_EQ(pass == 0 ? 3900u : pass == 1 ? 3660u : 0u, actual.rows[i].ttl);
        }
    }
    mt_dns_pipeline_destroy(pa); mt_dns_pipeline_destroy(pr);
    mt_cache_destroy(ca); mt_cache_destroy(cr); PASS();
}

TEST boundaries_are_lossless_and_own_transient_names(void)
{
    const size_t sizes[] = {1, 31, 32, 33, 65, 100};
    for (size_t long_names = 0; long_names < 2; long_names++) {
        for (size_t n = 0; n < sizeof(sizes) / sizeof(sizes[0]); n++) {
            mt_cache_t *ca, *cr;
            mt_dns_pipeline_t *pa = pipeline(&ca, &actual, 2, 0);
            mt_dns_pipeline_t *pr = pipeline(&cr, &reference, 2, 0);
            mt_dns_rr_t records[100]; uint8_t ip[100][16];
            for (size_t i = 0; i < sizes[n]; i++) {
                char name[256];
                if (long_names) {
                    memset(name, 'a', 202); name[50] = name[101] = name[152] = '.'; name[202] = 0;
                    name[0] = (char)('a' + (int)(i % 26));
                } else { snprintf(name, sizeof(name), "n%zu.test", i % 8); }
                memset(ip[i], 0, sizeof(ip[i])); ip[i][0] = 192; ip[i][3] = (uint8_t)(i % 40);
                size_t len = i % 3 ? 4 : 16;
                records[i] = rr(name, len == 4 ? 1 : 28, (uint32_t)(100 - i), ip[i], len);
            }
            mt_dns_msg_t msg = {.answers = records, .n_answers = sizes[n]};
            reset_capture(&actual); reset_capture(&reference);
            mt_dns_pipeline_handle_message(pa, &msg, 1000); stream_records(pr, &msg, 1000);
            CHECK_CALL(equivalent());
            mt_dns_pipeline_destroy(pa); mt_dns_pipeline_destroy(pr);
            mt_cache_destroy(ca); mt_cache_destroy(cr);
        }
    }
    PASS();
}

static uint32_t rng = 0x7b317f23u;
static uint32_t next_random(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
TEST mixed_stream_reference_and_reload(void)
{
    mt_cache_t *ca, *cr;
    mt_dns_pipeline_t *pa = pipeline(&ca, &actual, 2, 10);
    mt_dns_pipeline_t *pr = pipeline(&cr, &reference, 2, 10);
    for (size_t round = 0; round < 150; round++) {
        if (round % 25 == 0) {
            const char *pattern = round % 50 ? "n1*" : "*";
            mt_dns_pipeline_set_snapshot(pa, snapshot(2, pattern));
            mt_dns_pipeline_set_snapshot(pr, snapshot(2, pattern));
        }
        mt_dns_rr_t records[80]; uint8_t data[80][256];
        size_t count = 2 + next_random() % 79;
        for (size_t i = 0; i < count; i++) {
            char name[32], target[32];
            snprintf(name, sizeof(name), "n%u.test", next_random() % 16);
            snprintf(target, sizeof(target), "n%u.test", next_random() % 16);
            uint32_t choice = next_random() % 6, ttl = next_random() % 121;
            memset(data[i], 0, sizeof(data[i])); data[i][0] = 192; data[i][3] = (uint8_t)(next_random() % 12);
            uint16_t type = choice == 0 ? 5 : choice == 1 ? 28 : choice == 2 ? 2 : 1;
            size_t len = type == 5 ? make_name(data[i], target) : type == 28 ? 16 : 4;
            if (choice == 5) { len = 3; } /* invalid A, ignored */
            records[i] = rr(name, type, ttl, data[i], len);
        }
        mt_dns_msg_t msg = {.answers = records, .n_answers = count,
                            .authority = records, .n_authority = count,
                            .additional = records, .n_additional = count,
                            .flags = round % 23 ? 0 : 3};
        reset_capture(&actual); reset_capture(&reference);
        int64_t now = 1000 + (int64_t)round;
        mt_dns_pipeline_handle_message(pa, &msg, now); stream_records(pr, &msg, now);
        CHECK_CALL(equivalent());
        for (unsigned d = 0; d < 16; d++) {
            char name[32]; snprintf(name, sizeof(name), "n%u.test", d);
            mt_cache_addr_t *aa = NULL, *ar = NULL; size_t na = 0, nr = 0;
            ASSERT_EQ(MT_OK, mt_cache_get_addresses(ca, name, now, &aa, &na));
            ASSERT_EQ(MT_OK, mt_cache_get_addresses(cr, name, now, &ar, &nr)); ASSERT_EQ(nr, na);
            for (size_t j = 0; j < na; j++) {
                ASSERT_EQ(ar[j].addr_len, aa[j].addr_len); ASSERT_EQ(ar[j].deadline, aa[j].deadline);
                ASSERT_EQ(0, memcmp(ar[j].addr, aa[j].addr, aa[j].addr_len));
            }
            free(aa); free(ar);
        }
    }
    mt_dns_pipeline_destroy(pa); mt_dns_pipeline_destroy(pr);
    mt_cache_destroy(ca); mt_cache_destroy(cr); PASS();
}

TEST cname_only_retains_cached_addresses_and_null_sink(void)
{
    mt_cache_t *cache;
    mt_dns_pipeline_t *p = pipeline(&cache, &actual, 1, 0);
    uint8_t ip[4] = {203,0,113,7}, target[256];
    mt_cache_add_address(cache, "target.test", ip, 4, 90, 1000);
    size_t len = make_name(target, "target.test");
    mt_dns_rr_t records[] = {rr("a.test", 5, 60, target, len), rr("b.test", 5, 60, target, len)};
    mt_dns_msg_t msg = {.answers = records, .n_answers = 2};
    reset_capture(&actual); mt_dns_pipeline_handle_message(p, &msg, 1010);
    ASSERT_EQ(1u, actual.n); ASSERT_EQ(2u, actual.represented); ASSERT_EQ(80u, actual.rows[0].ttl);
    mt_dns_pipeline_set_snapshot(p, snapshot(0, "*"));
    reset_capture(&actual); mt_dns_pipeline_handle_message(p, &msg, 1010); ASSERT_EQ(0u, actual.n);
    mt_dns_pipeline_destroy(p); mt_cache_destroy(cache);
    p = pipeline(&cache, NULL, 1, 0);
    records[0] = rr("target.test", 1, 60, ip, 4); msg.n_answers = 1;
    mt_dns_pipeline_handle_message(p, &msg, 1000);
    mt_cache_addr_t *a = NULL; size_t count = 0;
    ASSERT_EQ(MT_OK, mt_cache_get_addresses(cache, "target.test", 1000, &a, &count));
    ASSERT_EQ(1u, count); free(a); mt_dns_pipeline_destroy(p); mt_cache_destroy(cache); PASS();
}

TEST coalesced_writes_follow_the_order_of_their_last_occurrence(void)
{
    mt_cache_t *cache;
    mt_dns_pipeline_t *p = pipeline(&cache, &actual, 1, 0);
    uint8_t a[4] = {192,0,2,1}, b[4] = {192,0,2,2}, v6[16] = {192,0,2,1};
    mt_dns_rr_t records[] = {
        rr("early.test", 1, 100, a, 4),
        rr("middle.test", 1, 20, b, 4),
        rr("last.test", 1, 0, a, 4),
        rr("v6.test", 28, 15, v6, 16),
    };
    mt_dns_msg_t msg = {.answers = records, .n_answers = 4};
    reset_capture(&actual); mt_dns_pipeline_handle_message(p, &msg, 1000);
    ASSERT_EQ(3u, actual.n); ASSERT_EQ(4u, actual.represented);
    ASSERT_STR_EQ("middle.test", actual.rows[0].domain);
    ASSERT_EQ(20u, actual.rows[0].ttl); ASSERT_EQ(1u, actual.rows[0].attempts);
    ASSERT_STR_EQ("last.test", actual.rows[1].domain);
    ASSERT_EQ(0u, actual.rows[1].ttl); ASSERT_EQ(2u, actual.rows[1].attempts);
    ASSERT_EQ(4u, actual.rows[1].len);
    ASSERT_STR_EQ("v6.test", actual.rows[2].domain);
    ASSERT_EQ(16u, actual.rows[2].len); ASSERT_EQ(15u, actual.rows[2].ttl);
    ASSERT_EQ(1u, actual.rows[2].attempts);
    mt_dns_pipeline_destroy(p); mt_cache_destroy(cache); PASS();
}

TEST duplicate_run_preserves_the_full_attempt_budget_across_chunks(void)
{
    mt_cache_t *cache;
    mt_dns_pipeline_t *p = pipeline(&cache, &actual, 1, 0);
    uint8_t ip[4] = {192,0,2,1};
    mt_dns_rr_t records[65];
    for (size_t i = 0; i < 65; i++) {
        records[i] = rr("n.test", 1, (uint32_t)(64 - i), ip, 4);
    }
    mt_dns_msg_t msg = {.answers = records, .n_answers = 65};
    reset_capture(&actual); mt_dns_pipeline_handle_message(p, &msg, 1000);
    ASSERT_EQ(3u, actual.n); ASSERT_EQ(65u, actual.represented);
    ASSERT_EQ(32u, actual.rows[0].attempts); ASSERT_EQ(33u, actual.rows[0].ttl);
    ASSERT_EQ(32u, actual.rows[1].attempts); ASSERT_EQ(1u, actual.rows[1].ttl);
    ASSERT_EQ(1u, actual.rows[2].attempts); ASSERT_EQ(0u, actual.rows[2].ttl);
    mt_dns_pipeline_destroy(p); mt_cache_destroy(cache); PASS();
}

/* The retry assertions invoke main.c:on_match, not a parallel fake sink. */
static struct { unsigned attempted, committed, fail; uint32_t ttl; uint8_t family; bool absent; } kernel;
mt_ruleset_t *__wrap_mt_app_find_group_by_id(mt_app_t *app, mt_id_t id);
mt_ruleset_t *__wrap_mt_app_find_subscription_ruleset_by_id(mt_app_t *app, mt_id_t id);
mt_err_t __wrap_mt_ruleset_add_ipv4(mt_ruleset_t *rs, mt_ipv4_subnet_t s, const uint32_t *ttl);
mt_err_t __wrap_mt_ruleset_add_ipv6(mt_ruleset_t *rs, mt_ipv6_subnet_t s, const uint32_t *ttl);
mt_ruleset_t *__wrap_mt_app_find_group_by_id(mt_app_t *app, mt_id_t id)
{ (void)app; (void)id; return kernel.absent ? NULL : (mt_ruleset_t *)&kernel; }
mt_ruleset_t *__wrap_mt_app_find_subscription_ruleset_by_id(mt_app_t *app, mt_id_t id)
{ (void)app; (void)id; return NULL; }
static mt_err_t kernel_add(uint8_t family, const uint32_t *ttl)
{
    kernel.attempted++; kernel.family = family; kernel.ttl = *ttl;
    if (kernel.fail) { kernel.fail--; return MT_ERR_IO; }
    kernel.committed++; return MT_OK;
}
mt_err_t __wrap_mt_ruleset_add_ipv4(mt_ruleset_t *rs, mt_ipv4_subnet_t s, const uint32_t *ttl)
{ (void)rs; (void)s; return kernel_add(4, ttl); }
mt_err_t __wrap_mt_ruleset_add_ipv6(mt_ruleset_t *rs, mt_ipv6_subnet_t s, const uint32_t *ttl)
{ (void)rs; (void)s; return kernel_add(16, ttl); }
TEST real_sink_retries_final_value_and_future_responses(void)
{
    struct daemon d = {0}; uint8_t ip[16] = {192,0,2,1};
    mt_match_action_t action = {.addr = ip, .addr_len = 4, .ttl_seconds = 60, .attempts = 3,
        .record_domain = "a.test", .matched_name = "a.test", .record_kind = "A", .group_name = "g"};
    memset(&kernel, 0, sizeof(kernel)); kernel.fail = 2;
    on_match(&action, &d); ASSERT_EQ(3u, kernel.attempted); ASSERT_EQ(1u, kernel.committed); ASSERT_EQ(60u, kernel.ttl);
    memset(&kernel, 0, sizeof(kernel)); kernel.fail = 3;
    on_match(&action, &d); ASSERT_EQ(3u, kernel.attempted); ASSERT_EQ(0u, kernel.committed);
    on_match(&action, &d); ASSERT_EQ(4u, kernel.attempted); ASSERT_EQ(1u, kernel.committed);
    action.addr_len = 16; action.ttl_seconds = 0; action.attempts = 0;
    on_match(&action, &d); ASSERT_EQ(5u, kernel.attempted); ASSERT_EQ(2u, kernel.committed);
    ASSERT_EQ(16u, kernel.family); ASSERT_EQ(0u, kernel.ttl);
    kernel.absent = true; on_match(&action, &d); ASSERT_EQ(5u, kernel.attempted);
    memset(&kernel, 0, sizeof(kernel)); kernel.fail = 31; action.attempts = 32;
    on_match(&action, &d); ASSERT_EQ(32u, kernel.attempted); ASSERT_EQ(1u, kernel.committed);
    ASSERT_EQ(0u, kernel.ttl); ASSERT_EQ(16u, kernel.family);
    PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN(); mt_log_set_level(MT_LOG_ERROR);
    RUN_TEST(warm_cname_last_ttl_and_independent_groups);
    RUN_TEST(boundaries_are_lossless_and_own_transient_names);
    RUN_TEST(mixed_stream_reference_and_reload);
    RUN_TEST(cname_only_retains_cached_addresses_and_null_sink);
    RUN_TEST(coalesced_writes_follow_the_order_of_their_last_occurrence);
    RUN_TEST(duplicate_run_preserves_the_full_attempt_budget_across_chunks);
    RUN_TEST(real_sink_retries_final_value_and_future_responses);
    GREATEST_MAIN_END();
}
