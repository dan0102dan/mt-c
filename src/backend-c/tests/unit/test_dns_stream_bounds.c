/* Inspect only the private state machine; network/lifetime behavior is tested
 * by test_dns_stream. No extra production entry points or clock sleeps. */
#include "greatest.h"
#include "../../src/api/system.c"

mt_err_t __real_mt_loop_add_timer(mt_loop_t *loop, uint64_t initial_ms,
    uint64_t interval_ms, mt_timer_cb cb, void *ud, int *out_id);
mt_err_t __wrap_mt_loop_add_timer(mt_loop_t *loop, uint64_t initial_ms,
    uint64_t interval_ms, mt_timer_cb cb, void *ud, int *out_id);
static bool fail_client_timer;
mt_err_t __wrap_mt_loop_add_timer(mt_loop_t *loop, uint64_t initial_ms,
    uint64_t interval_ms, mt_timer_cb cb, void *ud, int *out_id) {
    if (fail_client_timer && interval_ms == MT_DNS_CLIENT_REFRESH_MS) { return MT_ERR_SYS; }
    return __real_mt_loop_add_timer(loop, initial_ms, interval_ms, cb, ud, out_id);
}

static bool advance_to_begin(dns_stream_t *stream) {
    for (unsigned i = 0; i < 10; i++) {
        size_t len = 0;
        char *text = dns_stream_next(stream, &len);
        if (!text) return false;
        bool begin = strstr(text, "event: snapshot_begin\n") != NULL;
        free(text);
        if (begin) return true;
    }
    return false;
}
static void overrun(mt_dns_query_log_t *log) {
    for (size_t i = 0; i <= mt_dns_query_log_capacity(log); i++)
        mt_dns_query_log_append(log, "10.0.0.1", "churn.example", 1, false, 0);
}
TEST continuous_eviction_cannot_restart_one_snapshot_forever(void) {
    mt_dns_query_log_t *log = mt_dns_query_log_create_budget(32768); ASSERT(log);
    ASSERT(mt_dns_query_log_set_enabled(log, true));
    overrun(log);
    dns_stream_t stream = {.log = log};
    ASSERT(advance_to_begin(&stream));
    uint64_t deadline = stream.snapshot_deadline;
    for (unsigned retry = 0; retry < DNS_SNAPSHOT_MAX_RESTARTS; retry++) {
        overrun(log);
        ASSERT(advance_to_begin(&stream));
        ASSERT_EQ(deadline, stream.snapshot_deadline);
    }
    overrun(log);
    size_t len = 0; char *text = dns_stream_next(&stream, &len);
    ASSERT(text == NULL); ASSERT_EQ(SIZE_MAX, len);
    mt_dns_query_log_destroy(log); PASS();
}
TEST snapshot_deadline_is_not_extended_by_write_progress(void) {
    mt_dns_query_log_t *log = mt_dns_query_log_create_budget(32768); ASSERT(log);
    ASSERT(mt_dns_query_log_set_enabled(log, true));
    overrun(log); dns_stream_t stream = {.log = log};
    ASSERT(advance_to_begin(&stream));
    stream.snapshot_deadline = dns_now_ms() - 1;
    size_t len = 0; char *text = dns_stream_next(&stream, &len);
    ASSERT(text == NULL); ASSERT_EQ(SIZE_MAX, len);
    mt_dns_query_log_destroy(log); PASS();
}
static mt_err_t count_client_fetches(mt_dns_client_t **out, size_t *count,
    const atomic_bool *stop, void *ud) {
    (void)stop;
    atomic_fetch_add((atomic_uint *)ud, 1);
    *out = NULL; *count = 0;
    return MT_OK;
}
static void client_changed(void *ud) { (void)ud; }
TEST unavailable_metadata_timer_preserves_dns_stream_and_can_retry(void) {
    mt_loop_t *loop = NULL;
    mt_dns_clients_t *clients = NULL;
    mt_dns_query_log_t *log = NULL;
    atomic_uint fetches;
    atomic_init(&fetches, 0);
    int subscriptions[MT_DNS_CLIENT_SUBSCRIBERS] = {0};
    char *text = NULL;
    bool ok = true;
    dns_stream_t stream = {0};
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "metadata timer check failed at %d: %s\n", __LINE__, #expr); ok = false; goto cleanup; } } while (0)
    CHECK(mt_loop_create(&loop) == MT_OK);
    CHECK(mt_dns_clients_create(loop, count_client_fetches, &fetches, &clients) == MT_OK);
    log = mt_dns_query_log_create_budget(32768);
    CHECK(log && mt_dns_query_log_set_enabled(log, true));
    mt_dns_query_log_append(log, "192.0.2.1", "snapshot.example", 1, false, 0);
    stream.log = log; stream.available_clients = clients;

    fail_client_timer = true;
    CHECK(advance_to_begin(&stream));
    CHECK(stream.clients_subscription == 0 && stream.clients == NULL);
    size_t len = 0;
    text = dns_stream_next(&stream, &len);
    CHECK(text && strstr(text, "event: snapshot_chunk\n") &&
          strstr(text, "192.0.2.1") && strstr(text, "snapshot.example") &&
          !strstr(text, "client_name"));
    free(text); text = dns_stream_next(&stream, &len);
    CHECK(text && strstr(text, "event: snapshot_end\n"));
    free(text); text = NULL;
    mt_dns_query_log_append(log, "192.0.2.2", "live.example", 28, false, 1);
    text = dns_stream_next(&stream, &len);
    CHECK(text && strstr(text, "event: query\n") && strstr(text, "live.example"));
    free(text); text = NULL;
    mt_dns_clients_refresh(clients); mt_dns_clients_invalidate(clients);
    CHECK(atomic_load(&fetches) == 0 && mt_dns_clients_count(clients) == 0);

    /* Failed admission must not consume any slot or leave discovery active. */
    fail_client_timer = false;
    for (size_t i = 0; i < MT_DNS_CLIENT_SUBSCRIBERS; i++) {
        subscriptions[i] = mt_dns_clients_subscribe(clients, client_changed, NULL);
        CHECK(subscriptions[i] > 0);
    }
    CHECK(mt_dns_clients_subscribe(clients, client_changed, NULL) == 0);
cleanup:
    fail_client_timer = false;
    free(text);
    mt_dns_clients_unsubscribe(clients, stream.clients_subscription);
    for (size_t i = 0; i < MT_DNS_CLIENT_SUBSCRIBERS; i++) {
        mt_dns_clients_unsubscribe(clients, subscriptions[i]);
    }
    mt_dns_clients_destroy(clients);
    mt_dns_query_log_destroy(log);
    mt_loop_destroy(loop);
    ASSERT(ok);
    PASS();
#undef CHECK
}
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(continuous_eviction_cannot_restart_one_snapshot_forever);
    RUN_TEST(snapshot_deadline_is_not_extended_by_write_progress);
    RUN_TEST(unavailable_metadata_timer_preserves_dns_stream_and_can_retry);
    GREATEST_MAIN_END();
}
