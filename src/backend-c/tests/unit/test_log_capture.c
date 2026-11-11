/* Include the production formatter/journal with test-local symbol names and
 * allocation faults. No testing hooks or wrappers enter the daemon binary. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static _Atomic size_t allocations;
#include <stdbool.h>
static bool fail_malloc, fail_realloc;
static void *checked_malloc(size_t n) {
    allocations++;
    if (fail_malloc) { fail_malloc = false; return NULL; }
    return malloc(n);
}
static void *checked_realloc(void *p, size_t n) {
    if (fail_realloc) { fail_realloc = false; return NULL; }
    return realloc(p, n);
}
#define malloc checked_malloc
#define realloc checked_realloc
#define mt_log capture_test_log
#define mt_log_set_level capture_test_level_set
#define mt_log_level capture_test_level
#define mt_log_set_fd capture_test_fd
#define mt_log_level_from_str capture_test_level_from_str
#define mt_log_history_enable capture_test_enable
#define mt_log_history_state capture_test_state
#define mt_log_history_read capture_test_read
#define mt_log_history_subscribe capture_test_subscribe
#define mt_log_history_ack capture_test_ack
#define mt_log_history_unsubscribe capture_test_unsubscribe
#include "../../src/logging/log.c"
#undef malloc
#undef realloc

static mt_log_history_state_t state(void) {
    mt_log_history_state_t s;
    mt_log_history_state(&s);
    return s;
}
static void fresh(void) {
    assert(mt_log_history_enable(false));
    assert(mt_log_history_enable(true));
    mt_log_set_level(MT_LOG_TRACE);
}
static void test_default_off(void) {
    assert(!state().enabled && state().count == 0);
    size_t before = allocations;
    MT_INFO("not collected");
    assert(allocations == before && state().count == 0);
}
static void test_full_messages(void) {
    fresh();
    const size_t n = 128 * 1024;
    char *message = malloc(n + 1);
    assert(message);
    memset(message, 'x', n);
    memcpy(message + n - 8, "THE-END!", 8);
    message[n] = 0;
    errno = EDOM;
    MT_WARN("%s", message);
    assert(errno == EDOM);
    mt_log_history_state_t s = state();
    assert(s.count == 1 && strlen(s.session) == 32);
    char *out = malloc(n + 128);
    assert(out);
    size_t pos = 0;
    mt_log_history_piece_t part = {0};
    while (mt_log_history_read(s.session, 1, pos, out + pos, 17, &part)) pos += part.copied;
    assert(pos == 25 + n + 1 && pos == part.length);
    assert(part.level == MT_LOG_WARN);
    assert(memcmp(out + 25, message, n) == 0 && out[pos - 1] == '\n');
    free(out); free(message);
    mt_log(MT_LOG_INFO, "A%cB", 0);
    char bytes[128];
    assert(mt_log_history_read(s.session, 2, 0, bytes, sizeof(bytes), &part));
    assert(part.length == 29 && bytes[25] == 'A' && bytes[26] == 0 && bytes[27] == 'B');
}
static void test_sessions_and_no_quota(void) {
    fresh();
    char old[MT_LOG_SESSION_SIZE];
    strcpy(old, state().session);
    for (int i = 0; i < 11000; i++) MT_INFO("record=%d", i);
    assert(state().count == 11000);
    char row[128]; mt_log_history_piece_t p;
    assert(mt_log_history_read(old, 1, 0, row, sizeof(row), &p));
    assert(memcmp(row + 25, "record=0\n", 9) == 0);
    assert(mt_log_history_enable(true)); /* Never clear on an idempotent Start. */
    assert(state().count == 11000 && strcmp(old, state().session) == 0);
    assert(mt_log_history_enable(false));
    assert(history_rows == NULL && history_capacity == 0 && state().count == 0);
    assert(!mt_log_history_read(old, 1, 0, row, sizeof(row), &p));
    MT_ERROR("not collected after stop");
    assert(state().count == 0);
    assert(mt_log_history_enable(true));
    assert(strcmp(old, state().session) != 0);
}
static void test_failures_and_filtering(void) {
    fresh();
    fail_malloc = true;
    MT_INFO("allocation failure");
    assert(state().count == 0 && state().dropped == 1);
    fail_realloc = true;
    MT_INFO("index failure");
    assert(state().count == 0 && state().dropped == 2);
    MT_INFO("recovered");
    assert(state().count == 1);
    mt_log_set_level(MT_LOG_ERROR);
    MT_INFO("filtered");
    assert(state().count == 1);
}
static void test_notifications(void) {
    fresh();
    int p[2]; assert(pipe(p) == 0);
    assert(fcntl(p[0], F_SETFL, O_NONBLOCK) == 0);
    assert(fcntl(p[1], F_SETFL, O_NONBLOCK) == 0);
    int sub = mt_log_history_subscribe(p[1]); assert(sub > 0);
    for (int i = 0; i < 100; i++) MT_INFO("burst");
    char data[128]; assert(read(p[0], data, sizeof(data)) == 1);
    mt_log_history_ack(sub);
    MT_INFO("after ack");
    assert(read(p[0], data, sizeof(data)) == 1);
    mt_log_history_unsubscribe(sub);
    MT_INFO("after unsubscribe");
    assert(read(p[0], data, sizeof(data)) < 0 && errno == EAGAIN);
    int slots[MT_LOG_HISTORY_SUBSCRIBERS];
    for (int i = 0; i < MT_LOG_HISTORY_SUBSCRIBERS; i++) {
        slots[i] = mt_log_history_subscribe(p[1]); assert(slots[i] > 0);
    }
    assert(mt_log_history_subscribe(p[1]) == 0);
    for (int i = 0; i < MT_LOG_HISTORY_SUBSCRIBERS; i++) mt_log_history_unsubscribe(slots[i]);
    close(p[1]); close(p[0]);
}
static void *writer(void *unused) {
    (void)unused;
    for (int i = 0; i < 1000; i++) MT_INFO("thread message %d", i);
    return NULL;
}
static void test_concurrent(void) {
    fresh();
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) assert(pthread_create(&threads[i], NULL, writer, NULL) == 0);
    for (int i = 0; i < 4; i++) assert(pthread_join(threads[i], NULL) == 0);
    assert(state().count == 4000);
    /* Concurrent Stop/Start and bounded reads must not retain freed pointers. */
    for (int i = 0; i < 4; i++) assert(pthread_create(&threads[i], NULL, writer, NULL) == 0);
    for (int i = 0; i < 100; i++) {
        mt_log_history_state_t s = state();
        char out[32]; mt_log_history_piece_t p;
        (void)mt_log_history_read(s.session, 1, 0, out, sizeof(out), &p);
        fresh();
    }
    for (int i = 0; i < 4; i++) assert(pthread_join(threads[i], NULL) == 0);
}
int main(void) {
    int fd = open("/dev/null", O_WRONLY); assert(fd >= 0);
    mt_log_set_fd(fd);
    test_default_off();
    test_full_messages();
    test_sessions_and_no_quota();
    test_failures_and_filtering();
    test_notifications();
    test_concurrent();
    assert(mt_log_history_enable(false));
    mt_log_set_fd(STDOUT_FILENO); close(fd);
    puts("application log capture: 6 test groups passed");
    return 0;
}
