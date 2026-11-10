#include "greatest.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "magitrickle/log.h"

static ssize_t capture(int level_to_log, const char *msg, char *out,
                       size_t out_len)
{
    int fds[2];
    if (pipe(fds) != 0) {
        return -1;
    }
    mt_log_set_fd(fds[1]);
    mt_log((mt_log_level_t)level_to_log, "%s", msg);
    mt_log_set_fd(STDOUT_FILENO);
    close(fds[1]);
    ssize_t n = read(fds[0], out, out_len - 1);
    close(fds[0]);
    if (n < 0) {
        n = 0;
    }
    out[n] = '\0';
    return n;
}

TEST level_filtering(void)
{
    char buf[256];
    mt_log_set_level(MT_LOG_WARN);
    ASSERT_EQ(0, capture(MT_LOG_INFO, "hidden", buf, sizeof(buf)));
    ASSERT(capture(MT_LOG_ERROR, "shown", buf, sizeof(buf)) > 0);
    ASSERT(strstr(buf, "ERR") != NULL);
    ASSERT(strstr(buf, "shown") != NULL);
    mt_log_set_level(MT_LOG_INFO);
    PASS();
}

TEST line_format_has_timestamp_and_tag(void)
{
    char buf[256];
    mt_log_set_level(MT_LOG_TRACE);
    ASSERT(capture(MT_LOG_INFO, "fmt-check", buf, sizeof(buf)) > 0);
    /* "YYYY-MM-DDTHH:MM:SSZ INF fmt-check\n" */
    ASSERT_EQ('2', buf[0]);
    ASSERT_EQ('T', buf[10]);
    ASSERT_EQ('Z', buf[19]);
    ASSERT(strstr(buf, " INF fmt-check\n") != NULL);
    mt_log_set_level(MT_LOG_INFO);
    PASS();
}

TEST level_parsing_matches_go_config_values(void)
{
    ASSERT_EQ(MT_LOG_TRACE, mt_log_level_from_str("trace"));
    ASSERT_EQ(MT_LOG_DEBUG, mt_log_level_from_str("debug"));
    ASSERT_EQ(MT_LOG_INFO, mt_log_level_from_str("info"));
    ASSERT_EQ(MT_LOG_WARN, mt_log_level_from_str("warn"));
    ASSERT_EQ(MT_LOG_ERROR, mt_log_level_from_str("error"));
    ASSERT_EQ(MT_LOG_FATAL, mt_log_level_from_str("fatal"));
    ASSERT_EQ(MT_LOG_PANIC, mt_log_level_from_str("panic"));
    ASSERT_EQ(MT_LOG_NOLEVEL, mt_log_level_from_str("nolevel"));
    ASSERT_EQ(MT_LOG_DISABLED, mt_log_level_from_str("disabled"));
    /* unknown falls back to info, like Go setupLogging */
    ASSERT_EQ(MT_LOG_INFO, mt_log_level_from_str("bogus"));
    ASSERT_EQ(MT_LOG_INFO, mt_log_level_from_str(NULL));
    PASS();
}

TEST disabled_level_suppresses_everything(void)
{
    char buf[256];
    mt_log_set_level(MT_LOG_DISABLED);
    ASSERT_EQ(0, capture(MT_LOG_ERROR, "nope", buf, sizeof(buf)));
    mt_log_set_level(MT_LOG_INFO);
    PASS();
}


/* Capture large writes in a regular file rather than a pipe: a pipe with no
 * concurrent reader can deadlock on a valid 128 KiB log record. */
static int log_capture_file(void)
{
    char name[] = "/tmp/mt-log-test-XXXXXX";
    int fd = mkstemp(name);
    if (fd >= 0) unlink(name);
    return fd;
}

static ssize_t log_read_all(int fd, char *out, size_t len)
{
    if (lseek(fd, 0, SEEK_SET) < 0) return -1;
    size_t used = 0;
    while (used < len) {
        ssize_t n = read(fd, out + used, len - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        used += (size_t)n;
    }
    return (ssize_t)used;
}

TEST long_message_is_never_truncated(void)
{
    const size_t message_len = 128u * 1024u;
    char *message = malloc(message_len + 1);
    ASSERT(message != NULL);
    memset(message, 'x', message_len);
    memcpy(message + message_len - 9, "THE-END!!", 9);
    message[message_len] = '\0';
    int fd = log_capture_file();
    ASSERT(fd >= 0);
    mt_log_set_level(MT_LOG_INFO);
    mt_log_set_fd(fd);
    mt_log(MT_LOG_WARN, "context=%s;code=%d", message, 42);
    mt_log_set_fd(STDOUT_FILENO);
    off_t file_len = lseek(fd, 0, SEEK_END);
    ASSERT(file_len > 0);
    char *output = malloc((size_t)file_len + 1);
    ASSERT(output != NULL);
    ASSERT_EQ(file_len, log_read_all(fd, output, (size_t)file_len));
    output[file_len] = '\0';
    const char *header = " WRN context=";
    char *begin = strstr(output, header);
    ASSERT(begin != NULL);
    begin += strlen(header);
    ASSERT_EQ(0, memcmp(begin, message, message_len));
    ASSERT_EQ(0, strcmp(begin + message_len, ";code=42\n"));
    ASSERT_EQ((size_t)file_len,
              strlen("2000-01-01T00:00:00Z") + strlen(header) +
              message_len + strlen(";code=42\n"));
    free(output);
    free(message);
    close(fd);
    PASS();
}

enum { LOG_TEST_WORKERS = 4, LOG_TEST_LINES = 8, LOG_TEST_BODY = 8192 };
typedef struct { int worker; } log_test_writer_t;

static void *log_test_emit(void *ud)
{
    const log_test_writer_t *task = ud;
    char message[LOG_TEST_BODY + 1];
    memset(message, 'A' + task->worker, LOG_TEST_BODY);
    message[LOG_TEST_BODY] = '\0';
    for (int i = 0; i < LOG_TEST_LINES; i++) {
        mt_log(MT_LOG_INFO, "worker=%d number=%d %s END",
               task->worker, i, message);
    }
    return NULL;
}

TEST concurrent_long_messages_remain_intact(void)
{
    int fd = log_capture_file();
    ASSERT(fd >= 0);
    pthread_t threads[LOG_TEST_WORKERS];
    log_test_writer_t tasks[LOG_TEST_WORKERS];
    mt_log_set_level(MT_LOG_INFO);
    mt_log_set_fd(fd);
    for (int i = 0; i < LOG_TEST_WORKERS; i++) {
        tasks[i].worker = i;
        ASSERT_EQ(0, pthread_create(&threads[i], NULL, log_test_emit, &tasks[i]));
    }
    for (int i = 0; i < LOG_TEST_WORKERS; i++) {
        ASSERT_EQ(0, pthread_join(threads[i], NULL));
    }
    mt_log_set_fd(STDOUT_FILENO);
    off_t file_len = lseek(fd, 0, SEEK_END);
    ASSERT(file_len > 0);
    char *output = malloc((size_t)file_len + 1);
    ASSERT(output != NULL);
    ASSERT_EQ(file_len, log_read_all(fd, output, (size_t)file_len));
    output[file_len] = '\0';

    char expected[LOG_TEST_BODY];
    char *line = output;
    int seen = 0;
    while (*line) {
        char *end = strchr(line, '\n');
        ASSERT(end != NULL);
        char *tag = strstr(line, "worker=");
        ASSERT(tag != NULL && tag < end);
        int worker = -1, number = -1;
        ASSERT_EQ(2, sscanf(tag, "worker=%d number=%d", &worker, &number));
        ASSERT(worker >= 0 && worker < LOG_TEST_WORKERS);
        ASSERT(number >= 0 && number < LOG_TEST_LINES);
        char *body = strchr(tag, ' ');
        ASSERT(body != NULL && body < end);
        body = strchr(body + 1, ' ');
        ASSERT(body != NULL && body < end);
        body++;
        ASSERT_EQ((size_t)(end - body), (size_t)LOG_TEST_BODY + 4);
        memset(expected, 'A' + worker, sizeof(expected));
        ASSERT_EQ(0, memcmp(body, expected, sizeof(expected)));
        ASSERT_EQ(0, memcmp(body + LOG_TEST_BODY, " END", 4));
        seen++;
        line = end + 1;
    }
    ASSERT_EQ(LOG_TEST_WORKERS * LOG_TEST_LINES, seen);
    free(output);
    close(fd);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(level_filtering);
    RUN_TEST(line_format_has_timestamp_and_tag);
    RUN_TEST(level_parsing_matches_go_config_values);
    RUN_TEST(disabled_level_suppresses_everything);
    RUN_TEST(long_message_is_never_truncated);
    RUN_TEST(concurrent_long_messages_remain_intact);
    GREATEST_MAIN_END();
}
