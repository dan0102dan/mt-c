/* Inspect only the private state machine; network/lifetime behavior is tested
 * by test_dns_stream. No extra production entry points or clock sleeps. */
#include "greatest.h"
#include "../../src/api/system.c"

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
    mt_dns_query_log_t *log = mt_dns_query_log_create_sized(1000); ASSERT(log);
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
    mt_dns_query_log_t *log = mt_dns_query_log_create_sized(1000); ASSERT(log);
    overrun(log); dns_stream_t stream = {.log = log};
    ASSERT(advance_to_begin(&stream));
    stream.snapshot_deadline = dns_now_ms() - 1;
    size_t len = 0; char *text = dns_stream_next(&stream, &len);
    ASSERT(text == NULL); ASSERT_EQ(SIZE_MAX, len);
    mt_dns_query_log_destroy(log); PASS();
}
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(continuous_eviction_cannot_restart_one_snapshot_forever);
    RUN_TEST(snapshot_deadline_is_not_extended_by_write_progress);
    GREATEST_MAIN_END();
}
