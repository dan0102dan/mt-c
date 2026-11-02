/* Production loop with narrow syscall seams: timer reuse must not change
 * callback lifetime, kernel cadence, or the identity of an epoll batch. */
#define _GNU_SOURCE
#include "greatest.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

struct probe {
    unsigned creates, sets, adds, dels, allocs, frees, closes, waits;
    bool fail_create, fail_set, fail_alloc;
    int fail_ctl;
    bool fake_timer_reads;
    uint64_t expirations;
    char calls[32];
    size_t call_count;
    int (*wait_script)(struct epoll_event *events, int count);
};
static struct probe probe;

static void record_call(char call)
{
    if (probe.call_count + 1 < sizeof(probe.calls)) {
        probe.calls[probe.call_count++] = call;
        probe.calls[probe.call_count] = '\0';
    }
}

static void *probe_calloc(size_t n, size_t size)
{
    record_call('A');
    probe.allocs++;
    if (probe.fail_alloc) {
        probe.fail_alloc = false;
        return NULL;
    }
    return calloc(n, size);
}

static void probe_free(void *p)
{
    if (p != NULL) { probe.frees++; record_call('F'); }
    free(p);
}

static int probe_close(int fd)
{
    record_call('X');
    probe.closes++;
    return close(fd);
}

static int probe_timerfd_create(int clockid, int flags)
{
    record_call('C');
    probe.creates++;
    if (probe.fail_create) {
        probe.fail_create = false;
        errno = EMFILE;
        return -1;
    }
    return timerfd_create(clockid, flags);
}

static int probe_timerfd_settime(int fd, int flags,
                                const struct itimerspec *value,
                                struct itimerspec *old)
{
    record_call('S');
    probe.sets++;
    if (probe.fail_set) {
        probe.fail_set = false;
        errno = EIO;
        return -1;
    }
    return timerfd_settime(fd, flags, value, old);
}

static int probe_epoll_ctl(int epfd, int op, int fd, struct epoll_event *ev)
{
    if (op == EPOLL_CTL_ADD) { probe.adds++; record_call('E'); }
    if (op == EPOLL_CTL_DEL) { probe.dels++; record_call('D'); }
    if (probe.fail_ctl == op) {
        probe.fail_ctl = 0;
        errno = EIO;
        return -1;
    }
    return epoll_ctl(epfd, op, fd, ev);
}

static int probe_epoll_wait(int epfd, struct epoll_event *events,
                            int count, int timeout)
{
    probe.waits++;
    if (probe.wait_script != NULL) {
        return probe.wait_script(events, count);
    }
    return epoll_wait(epfd, events, count, timeout);
}

static ssize_t probe_read(int fd, void *buf, size_t size)
{
    if (probe.fake_timer_reads && size == sizeof(uint64_t)) {
        uint64_t one = 1;
        memcpy(buf, &one, sizeof(one));
        return (ssize_t)sizeof(one);
    }
    ssize_t n = read(fd, buf, size);
    if (n == (ssize_t)sizeof(uint64_t) && size == sizeof(uint64_t)) {
        memcpy(&probe.expirations, buf, sizeof(probe.expirations));
    }
    return n;
}

#define calloc probe_calloc
#define free probe_free
#define close probe_close
#define timerfd_create probe_timerfd_create
#define timerfd_settime probe_timerfd_settime
#define epoll_ctl probe_epoll_ctl
#define epoll_wait probe_epoll_wait
#define read probe_read
#include "../../src/platform/loop.c"
#undef calloc
#undef free
#undef close
#undef timerfd_create
#undef timerfd_settime
#undef epoll_ctl
#undef epoll_wait
#undef read

static void reset_probe(void)
{
    memset(&probe, 0, sizeof(probe));
}

static int fd_count(void)
{
    DIR *dir = opendir("/proc/self/fd");
    if (dir == NULL) { return -1; }
    int count = 0;
    const struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] != '.') { count++; }
    }
    closedir(dir);
    return count;
}

static watch_t *timer_watch(mt_loop_t *loop, int id)
{
    for (watch_t *w = loop->watches; w != NULL; w = w->next) {
        if (w->kind == WATCH_TIMER && w->id == id) { return w; }
    }
    return NULL;
}

static void count_cb(mt_loop_t *loop, void *ud)
{
    (void)loop;
    (*(unsigned *)ud)++;
}

static void stop_cb(mt_loop_t *loop, void *ud)
{
    if (ud != NULL) { (*(unsigned *)ud)++; }
    mt_loop_stop(loop);
}

TEST warm_timer_churn_reuses_fd_watch_and_kernel_registration(void)
{
    reset_probe();
    mt_loop_t *loop = NULL;
    ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    int base = fd_count(); ASSERT(base >= 0);
    reset_probe();
    unsigned calls = 0;
    int previous = 0;
    for (unsigned i = 0; i < 1000; i++) {
        int id = 0;
        ASSERT_EQ(MT_OK, mt_loop_add_reusable_timer(loop, 60000, 0, count_cb, &calls, &id));
        ASSERT(id > previous);
        ASSERT_EQ(MT_ERR_NOENT, mt_loop_del_timer(loop, previous));
        ASSERT_EQ(MT_OK, mt_loop_del_timer(loop, id));
        previous = id;
    }
    ASSERT_EQ(0u, calls);
    ASSERT_EQ(1u, probe.creates);
    ASSERT_EQ(1u, probe.adds);
    ASSERT_EQ(1u, probe.allocs);
    ASSERT_EQ(0u, probe.frees);
    ASSERT_EQ(0u, probe.closes);
    ASSERT_EQ(0u, probe.dels);
    ASSERT_EQ(2000u, probe.sets);
    ASSERT_EQ(base + 1, fd_count());
    mt_loop_stop(loop);
    ASSERT_EQ(MT_OK, mt_loop_run(loop));
    ASSERT_EQ(base, fd_count());
    ASSERT_EQ(0u, loop->cached_timers);
    mt_loop_destroy(loop);
    PASS();
}

TEST ordinary_timers_never_consume_or_refill_a_warm_reuse_pool(void)
{
    reset_probe();
    mt_loop_t *loop = NULL;
    ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    unsigned calls = 0;
    int cached_id;
    ASSERT_EQ(MT_OK, mt_loop_add_reusable_timer(loop, 60000, 0, count_cb, &calls, &cached_id));
    watch_t *reserved = timer_watch(loop, cached_id);
    ASSERT_EQ(MT_OK, mt_loop_del_timer(loop, cached_id));
    ASSERT_EQ(1u, loop->cached_timers);
    int base = fd_count(); ASSERT(base >= 0);
    reset_probe();
    for (unsigned i = 0; i < 64; i++) {
        int id;
        ASSERT_EQ(MT_OK, mt_loop_add_timer(loop, 60000, 0, count_cb, &calls, &id));
        watch_t *w = timer_watch(loop, id);
        ASSERT(w != NULL && w != reserved && !w->reusable);
        ASSERT(w->fd != reserved->fd);
        ASSERT_EQ(MT_OK, mt_loop_del_timer(loop, id));
        if (i == 0) { ASSERT_STR_EQ("CSAEDXF", probe.calls); }
        ASSERT_EQ(1u, loop->cached_timers);
        ASSERT_EQ(reserved, loop->idle_timers);
    }
    ASSERT_EQ(64u, probe.creates); ASSERT_EQ(64u, probe.sets);
    ASSERT_EQ(64u, probe.allocs); ASSERT_EQ(64u, probe.adds);
    ASSERT_EQ(64u, probe.dels); ASSERT_EQ(64u, probe.closes);
    ASSERT_EQ(64u, probe.frees); ASSERT_EQ(base, fd_count());
    reset_probe();
    int reused_id;
    ASSERT_EQ(MT_OK, mt_loop_add_reusable_timer(loop, 60000, 0, count_cb, &calls, &reused_id));
    ASSERT_EQ(reserved, timer_watch(loop, reused_id));
    ASSERT_EQ(0u, probe.creates); ASSERT_EQ(0u, probe.adds); ASSERT_EQ(0u, probe.allocs);
    ASSERT_EQ(MT_OK, mt_loop_del_timer(loop, reused_id));
    mt_loop_destroy(loop);
    PASS();
}

TEST ordinary_allocation_failure_preserves_warm_pool_and_original_call_order(void)
{
    reset_probe();
    mt_loop_t *loop = NULL;
    ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    unsigned calls = 0;
    int id;
    ASSERT_EQ(MT_OK, mt_loop_add_reusable_timer(loop, 60000, 0, count_cb, &calls, &id));
    ASSERT_EQ(MT_OK, mt_loop_del_timer(loop, id));
    watch_t *reserved = loop->idle_timers;
    int base = fd_count(); ASSERT(base >= 0);
    reset_probe(); probe.fail_alloc = true; id = -123;
    ASSERT_EQ(MT_ERR_NOMEM, mt_loop_add_timer(loop, 60000, 0, count_cb, &calls, &id));
    ASSERT_EQ(-123, id); ASSERT_STR_EQ("CSAX", probe.calls);
    ASSERT_EQ(reserved, loop->idle_timers); ASSERT_EQ(1u, loop->cached_timers);
    ASSERT(loop->watches == NULL); ASSERT_EQ(base, fd_count());
    mt_loop_destroy(loop);
    PASS();
}

TEST idle_fd_retention_is_bounded_and_destroy_closes_every_timer(void)
{
    reset_probe();
    int base = fd_count(); ASSERT(base >= 0);
    mt_loop_t *loop = NULL;
    ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    int ids[70];
    unsigned calls = 0;
    reset_probe();
    for (size_t i = 0; i < 70; i++) {
        ASSERT_EQ(MT_OK, mt_loop_add_reusable_timer(loop, 60000, 5, count_cb, &calls, &ids[i]));
    }
    for (size_t i = 0; i < 70; i++) {
        ASSERT_EQ(MT_OK, mt_loop_del_timer(loop, ids[i]));
    }
    ASSERT_EQ(32u, loop->cached_timers);
    ASSERT_EQ(38u, probe.closes);
    ASSERT_EQ(38u, probe.dels);
    ASSERT_EQ(102u, probe.sets);
    for (size_t i = 0; i < 32; i++) {
        ASSERT_EQ(MT_OK, mt_loop_add_reusable_timer(loop, 60000, 0, count_cb, &calls, &ids[i]));
    }
    ASSERT_EQ(70u, probe.creates);
    ASSERT_EQ(70u, probe.allocs);
    ASSERT_EQ(70u, probe.adds);
    ASSERT_EQ(0u, loop->cached_timers);
    /* Destroy a mixture of live and idle timers without ever running. */
    for (size_t i = 0; i < 16; i++) {
        ASSERT_EQ(MT_OK, mt_loop_del_timer(loop, ids[i]));
    }
    mt_loop_destroy(loop);
    ASSERT_EQ(base, fd_count());
    PASS();
}

TEST disarming_clears_unread_expirations_before_reuse(void)
{
    reset_probe();
    mt_loop_t *loop = NULL;
    ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    unsigned calls = 0;
    int old_id = 0, new_id = 0;
    ASSERT_EQ(MT_OK, mt_loop_add_reusable_timer(loop, 0, 1, count_cb, &calls, &old_id));
    watch_t *old = timer_watch(loop, old_id);
    ASSERT(old != NULL);
    int fd = old->fd;
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    ASSERT_EQ(1, poll(&pfd, 1, 1000));
    ASSERT_EQ(MT_OK, mt_loop_del_timer(loop, old_id));
    uint64_t ticks = 0;
    errno = 0;
    ASSERT_EQ(-1, read(fd, &ticks, sizeof(ticks)));
    ASSERT_EQ(EAGAIN, errno);
    struct itimerspec value;
    ASSERT_EQ(0, timerfd_gettime(fd, &value));
    ASSERT_EQ(0, value.it_value.tv_sec);
    ASSERT_EQ(0, value.it_value.tv_nsec);
    ASSERT_EQ(0, value.it_interval.tv_sec);
    ASSERT_EQ(0, value.it_interval.tv_nsec);
    struct epoll_event event;
    ASSERT_EQ(0, epoll_wait(loop->epfd, &event, 1, 0));
    ASSERT_EQ(MT_OK, mt_loop_add_reusable_timer(loop, 60000, 0, count_cb, &calls, &new_id));
    ASSERT_EQ(old, timer_watch(loop, new_id));
    ASSERT(new_id != old_id);
    ASSERT_EQ(-1, read(fd, &ticks, sizeof(ticks)));
    ASSERT_EQ(EAGAIN, errno);
    ASSERT_EQ(0, epoll_wait(loop->epfd, &event, 1, 0));
    ASSERT_EQ(MT_ERR_NOENT, mt_loop_del_timer(loop, old_id));
    ASSERT_EQ(MT_OK, mt_loop_del_timer(loop, new_id));
    mt_loop_destroy(loop);
    PASS();
}

struct ordering_ctx {
    mt_loop_t *loop;
    watch_t *first, *old, *sentinel, *child;
    int old_id, child_id;
    unsigned old_calls, child_calls;
    mt_err_t cancel_error, add_error;
    bool inside_callback, recursive, quarantined, reusable_child;
    char trace[16];
    size_t trace_len;
};
static struct ordering_ctx *ordering;

static void trace(struct ordering_ctx *ctx, char value)
{
    if (ctx->trace_len + 1 < sizeof(ctx->trace)) {
        ctx->trace[ctx->trace_len++] = value;
        ctx->trace[ctx->trace_len] = '\0';
    }
}

static void child_cb(mt_loop_t *loop, void *ud)
{
    struct ordering_ctx *ctx = ud;
    ctx->child_calls++;
    ctx->recursive = ctx->inside_callback;
    trace(ctx, 'N');
    mt_loop_stop(loop);
}

static void add_child(mt_loop_t *loop, struct ordering_ctx *ctx)
{
    ctx->inside_callback = true;
    ctx->cancel_error = mt_loop_del_timer(loop, ctx->old_id);
    ctx->add_error = ctx->reusable_child ?
        mt_loop_add_reusable_timer(loop, 0, 0, child_cb, ctx, &ctx->child_id) :
        mt_loop_add_timer(loop, 0, 0, child_cb, ctx, &ctx->child_id);
    ctx->child = timer_watch(loop, ctx->child_id);
    ctx->quarantined = ctx->old->removed && ctx->child != ctx->old;
    trace(ctx, 'R');
    ctx->inside_callback = false;
}

static void add_from_fd(mt_loop_t *loop, int fd, uint32_t events, void *ud)
{
    (void)events;
    char byte;
    if (read(fd, &byte, 1) != 1) { mt_loop_stop(loop); return; }
    struct ordering_ctx *ctx = ud;
    trace(ctx, 'F');
    add_child(loop, ctx);
}

static void add_from_timer(mt_loop_t *loop, void *ud)
{
    struct ordering_ctx *ctx = ud;
    trace(ctx, 'T');
    add_child(loop, ctx);
}

static void sentinel_cb(mt_loop_t *loop, int fd, uint32_t events, void *ud)
{
    (void)events;
    char byte;
    if (read(fd, &byte, 1) != 1) { mt_loop_stop(loop); return; }
    trace(ud, 'S');
}

static int ordering_wait(struct epoll_event *events, int count)
{
    if (count < 3) { errno = EINVAL; return -1; }
    memset(events, 0, 3 * sizeof(*events));
    if (probe.waits == 1) {
        events[0].data.ptr = ordering->first;
        events[1].data.ptr = ordering->old;
        events[2].data.ptr = ordering->sentinel;
        events[0].events = events[1].events = events[2].events = EPOLLIN;
        return 3;
    }
    if (probe.waits == 2 && ordering->child != NULL) {
        events[0].data.ptr = ordering->child;
        events[0].events = EPOLLIN;
        return 1;
    }
    errno = EIO;
    return -1;
}

static greatest_test_res check_ordering(bool from_timer, bool reusable_old,
                                         bool reusable_child)
{
    reset_probe();
    struct ordering_ctx ctx = {0};
    ctx.reusable_child = reusable_child;
    ASSERT_EQ(MT_OK, mt_loop_create(&ctx.loop));
    int first_pipe[2], last_pipe[2];
    ASSERT_EQ(0, pipe(first_pipe));
    ASSERT_EQ(0, pipe(last_pipe));
    ASSERT_EQ(MT_OK, mt_loop_add_fd(ctx.loop, last_pipe[0], EPOLLIN, sentinel_cb, &ctx));
    ctx.sentinel = find_fd_watch(ctx.loop, last_pipe[0]);
    mt_err_t err = reusable_old ?
        mt_loop_add_reusable_timer(ctx.loop, 0, 0, count_cb, &ctx.old_calls, &ctx.old_id) :
        mt_loop_add_timer(ctx.loop, 0, 0, count_cb, &ctx.old_calls, &ctx.old_id);
    ASSERT_EQ(MT_OK, err);
    ctx.old = timer_watch(ctx.loop, ctx.old_id);
    if (from_timer) {
        int first_id;
        ASSERT_EQ(MT_OK, mt_loop_add_timer(ctx.loop, 0, 0, add_from_timer, &ctx, &first_id));
        ctx.first = timer_watch(ctx.loop, first_id);
    } else {
        ASSERT_EQ(MT_OK, mt_loop_add_fd(ctx.loop, first_pipe[0], EPOLLIN, add_from_fd, &ctx));
        ctx.first = find_fd_watch(ctx.loop, first_pipe[0]);
    }
    ASSERT_EQ(1, write(first_pipe[1], "f", 1));
    ASSERT_EQ(1, write(last_pipe[1], "s", 1));
    ordering = &ctx;
    probe.fake_timer_reads = true;
    probe.wait_script = ordering_wait;
    ASSERT_EQ(MT_OK, mt_loop_run(ctx.loop));
    ASSERT_EQ(2u, probe.waits);
    ASSERT_EQ(MT_OK, ctx.cancel_error);
    ASSERT_EQ(MT_OK, ctx.add_error);
    ASSERT_EQ(0u, ctx.old_calls);
    ASSERT_EQ(1u, ctx.child_calls);
    ASSERT(ctx.quarantined);
    ASSERT(!ctx.recursive);
    ASSERT_STR_EQ(from_timer ? "TRSN" : "FRSN", ctx.trace);
    ASSERT_EQ(MT_ERR_NOENT, mt_loop_del_timer(ctx.loop, ctx.child_id));
    ASSERT_EQ(0u, ctx.loop->cached_timers);
    mt_loop_destroy(ctx.loop);
    close(first_pipe[0]); close(first_pipe[1]);
    close(last_pipe[0]); close(last_pipe[1]);
    ordering = NULL;
    PASS();
}

TEST fd_callback_cannot_reuse_a_cancelled_timer_from_the_same_ready_batch(void)
{
    for (unsigned old = 0; old < 2; old++) {
        for (unsigned child = 0; child < 2; child++) {
            CHECK_CALL(check_ordering(false, old != 0, child != 0));
        }
    }
    PASS();
}

TEST timer_callback_cannot_insert_a_child_into_the_current_ready_batch(void)
{
    for (unsigned old = 0; old < 2; old++) {
        for (unsigned child = 0; child < 2; child++) {
            CHECK_CALL(check_ordering(true, old != 0, child != 0));
        }
    }
    PASS();
}

struct self_free_result { unsigned calls; mt_err_t own_error, peer_error; };
struct self_free_ctx { int own_id, peer_id; struct self_free_result *result; };

static void self_free_cb(mt_loop_t *loop, void *ud)
{
    struct self_free_ctx *ctx = ud;
    ctx->result->calls++;
    ctx->result->peer_error = mt_loop_del_timer(loop, ctx->peer_id);
    ctx->result->own_error = mt_loop_del_timer(loop, ctx->own_id);
    free(ctx);
}

static watch_t *self_free_events[3];
static int self_free_wait(struct epoll_event *events, int count)
{
    if (count < 3 || probe.waits != 1) { errno = EIO; return -1; }
    for (size_t i = 0; i < 3; i++) {
        events[i].events = EPOLLIN;
        events[i].data.ptr = self_free_events[i];
    }
    return 3;
}

TEST callback_can_cancel_itself_and_a_ready_peer_then_free_its_userdata(void)
{
    reset_probe();
    mt_loop_t *loop = NULL;
    ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    struct self_free_ctx *ctx = calloc(1, sizeof(*ctx));
    ASSERT(ctx != NULL);
    struct self_free_result result = {0};
    ctx->result = &result;
    unsigned peer_calls = 0, stop_calls = 0;
    int stop_id;
    ASSERT_EQ(MT_OK, mt_loop_add_reusable_timer(loop, 0, 5, self_free_cb, ctx, &ctx->own_id));
    ASSERT_EQ(MT_OK, mt_loop_add_timer(loop, 0, 0, count_cb, &peer_calls, &ctx->peer_id));
    ASSERT_EQ(MT_OK, mt_loop_add_timer(loop, 0, 0, stop_cb, &stop_calls, &stop_id));
    self_free_events[0] = timer_watch(loop, ctx->own_id);
    self_free_events[1] = timer_watch(loop, ctx->peer_id);
    self_free_events[2] = timer_watch(loop, stop_id);
    probe.wait_script = self_free_wait;
    probe.fake_timer_reads = true;
    ASSERT_EQ(MT_OK, mt_loop_run(loop));
    ASSERT_EQ(1u, result.calls);
    ASSERT_EQ(MT_OK, result.own_error);
    ASSERT_EQ(MT_OK, result.peer_error);
    ASSERT_EQ(0u, peer_calls);
    ASSERT_EQ(1u, stop_calls);
    ASSERT(loop->watches == NULL);
    ASSERT(loop->retired == NULL);
    ASSERT(loop->idle_timers == NULL);
    mt_loop_destroy(loop);
    PASS();
}

static void free_oneshot_cb(mt_loop_t *loop, void *ud)
{
    unsigned **calls = ud;
    (**calls)++;
    free(calls);
    mt_loop_stop(loop);
}

TEST reusable_oneshot_callback_may_free_userdata_without_deleting_itself(void)
{
    reset_probe();
    mt_loop_t *loop = NULL;
    ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    unsigned calls = 0;
    unsigned **ud = malloc(sizeof(*ud)); ASSERT(ud != NULL); *ud = &calls;
    int base = fd_count(); ASSERT(base >= 0);
    int id;
    ASSERT_EQ(MT_OK, mt_loop_add_reusable_timer(loop, 0, 0, free_oneshot_cb, ud, &id));
    ASSERT_EQ(MT_OK, mt_loop_run(loop));
    ASSERT_EQ(1u, calls); ASSERT_EQ(MT_ERR_NOENT, mt_loop_del_timer(loop, id));
    ASSERT(loop->watches == NULL); ASSERT(loop->retired == NULL);
    ASSERT(loop->idle_timers == NULL); ASSERT_EQ(base, fd_count());
    mt_loop_destroy(loop);
    PASS();
}

TEST allocation_and_kernel_registration_failures_leave_no_live_timer(void)
{
    reset_probe();
    mt_loop_t *loop = NULL;
    ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    int base = fd_count(); ASSERT(base >= 0);
    unsigned calls = 0;
    for (unsigned fault = 0; fault < 4; fault++) {
        reset_probe();
        probe.fail_alloc = fault == 0;
        probe.fail_create = fault == 1;
        probe.fail_set = fault == 2;
        probe.fail_ctl = fault == 3 ? EPOLL_CTL_ADD : 0;
        int id = -123;
        mt_err_t expected = fault == 0 ? MT_ERR_NOMEM :
                            fault == 1 ? mt_err_from_errno(EMFILE) : MT_ERR_IO;
        ASSERT_EQ(expected, mt_loop_add_reusable_timer(loop, 60000, 0, count_cb, &calls, &id));
        ASSERT_EQ(-123, id);
        ASSERT_EQ(base, fd_count());
        ASSERT(loop->watches == NULL);
        ASSERT_EQ(0u, loop->cached_timers);
    }
    mt_loop_destroy(loop);
    PASS();
}

TEST failed_rearm_discards_cached_timer_and_preserves_other_active_timer(void)
{
    reset_probe();
    mt_loop_t *loop = NULL;
    ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    unsigned calls = 0;
    int active_id = 0, old_id = 0, new_id = -123;
    ASSERT_EQ(MT_OK, mt_loop_add_timer(loop, 60000, 0, count_cb, &calls, &active_id));
    ASSERT_EQ(MT_OK, mt_loop_add_reusable_timer(loop, 60000, 0, count_cb, &calls, &old_id));
    ASSERT_EQ(MT_OK, mt_loop_del_timer(loop, old_id));
    int base = fd_count(); ASSERT(base >= 0);
    reset_probe();
    probe.fail_set = true;
    ASSERT_EQ(MT_ERR_IO, mt_loop_add_reusable_timer(loop, 0, 0, count_cb, &calls, &new_id));
    ASSERT_EQ(-123, new_id);
    ASSERT_EQ(0u, probe.creates);
    ASSERT_EQ(1u, probe.closes);
    ASSERT_EQ(base - 1, fd_count());
    ASSERT_EQ(0u, loop->cached_timers);
    ASSERT(timer_watch(loop, active_id) != NULL);
    ASSERT_EQ(MT_ERR_NOENT, mt_loop_del_timer(loop, old_id));
    ASSERT_EQ(MT_OK, mt_loop_del_timer(loop, active_id));
    mt_loop_destroy(loop);
    PASS();
}

TEST failed_disarm_closes_timer_even_if_epoll_delete_also_fails(void)
{
    reset_probe();
    mt_loop_t *loop = NULL;
    ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    int base = fd_count(); ASSERT(base >= 0);
    unsigned calls = 0;
    int id;
    ASSERT_EQ(MT_OK, mt_loop_add_reusable_timer(loop, 0, 1, count_cb, &calls, &id));
    reset_probe();
    probe.fail_set = true;
    probe.fail_ctl = EPOLL_CTL_DEL;
    ASSERT_EQ(MT_OK, mt_loop_del_timer(loop, id));
    ASSERT_EQ(MT_ERR_NOENT, mt_loop_del_timer(loop, id));
    ASSERT_EQ(1u, probe.closes);
    ASSERT_EQ(1u, probe.dels);
    ASSERT_EQ(0u, loop->cached_timers);
    ASSERT_EQ(base, fd_count());
    struct epoll_event event;
    ASSERT_EQ(0, epoll_wait(loop->epfd, &event, 1, 0));
    mt_loop_destroy(loop);
    PASS();
}

static int error_wait(struct epoll_event *events, int count)
{
    (void)events; (void)count;
    errno = EIO;
    return -1;
}

TEST epoll_wait_error_releases_idle_timers_but_preserves_live_ids(void)
{
    reset_probe();
    mt_loop_t *loop = NULL;
    ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    int base = fd_count(); ASSERT(base >= 0);
    unsigned calls = 0;
    int active, idle;
    ASSERT_EQ(MT_OK, mt_loop_add_timer(loop, 60000, 0, count_cb, &calls, &active));
    ASSERT_EQ(MT_OK, mt_loop_add_reusable_timer(loop, 60000, 0, count_cb, &calls, &idle));
    ASSERT_EQ(MT_OK, mt_loop_del_timer(loop, idle));
    probe.wait_script = error_wait;
    ASSERT_EQ(MT_ERR_IO, mt_loop_run(loop));
    ASSERT_EQ(base + 1, fd_count());
    ASSERT_EQ(0u, loop->cached_timers);
    ASSERT(timer_watch(loop, active) != NULL);
    ASSERT_EQ(MT_OK, mt_loop_del_timer(loop, active));
    mt_loop_stop(loop);
    ASSERT_EQ(MT_OK, mt_loop_run(loop));
    ASSERT_EQ(base, fd_count());
    mt_loop_destroy(loop);
    PASS();
}

TEST timer_id_wrap_avoids_signed_overflow_and_live_id_collisions(void)
{
    reset_probe();
    mt_loop_t *loop = NULL;
    ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    unsigned calls = 0;
    int first, second, last, wrapped;
    ASSERT_EQ(MT_OK, mt_loop_add_reusable_timer(loop, 60000, 0, count_cb, &calls, &first));
    ASSERT_EQ(MT_OK, mt_loop_add_timer(loop, 60000, 0, count_cb, &calls, &second));
    ASSERT_EQ(1, first);
    ASSERT_EQ(2, second);
    loop->next_timer_id = INT_MAX;
    ASSERT_EQ(MT_OK, mt_loop_add_reusable_timer(loop, 60000, 0, count_cb, &calls, &last));
    ASSERT_EQ(INT_MAX, last);
    ASSERT_EQ(MT_OK, mt_loop_add_reusable_timer(loop, 60000, 0, count_cb, &calls, &wrapped));
    ASSERT_EQ(3, wrapped);
    ASSERT(timer_watch(loop, first) != NULL);
    ASSERT(timer_watch(loop, second) != NULL);
    ASSERT(timer_watch(loop, last) != NULL);
    ASSERT(timer_watch(loop, wrapped) != NULL);
    mt_loop_destroy(loop);
    PASS();
}

static watch_t *periodic_watch;
static int periodic_wait(struct epoll_event *events, int count)
{
    if (probe.waits != 1 || count < 1) { errno = EIO; return -1; }
    events[0].events = EPOLLIN;
    events[0].data.ptr = periodic_watch;
    return 1;
}

TEST delayed_periodic_expirations_coalesce_without_changing_kernel_interval(void)
{
    reset_probe();
    mt_loop_t *loop = NULL;
    ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    unsigned calls = 0;
    int id;
    ASSERT_EQ(MT_OK, mt_loop_add_reusable_timer(loop, 1, 5, stop_cb, &calls, &id));
    periodic_watch = timer_watch(loop, id);
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 40000000L};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) { }
    probe.wait_script = periodic_wait;
    ASSERT_EQ(MT_OK, mt_loop_run(loop));
    ASSERT_EQ(1u, calls);
    ASSERT(probe.expirations >= 2);
    struct itimerspec value;
    ASSERT_EQ(0, timerfd_gettime(periodic_watch->fd, &value));
    ASSERT_EQ(0, value.it_interval.tv_sec);
    ASSERT_EQ(5000000L, value.it_interval.tv_nsec);
    ASSERT_EQ(MT_OK, mt_loop_del_timer(loop, id));
    mt_loop_destroy(loop);
    periodic_watch = NULL;
    PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(warm_timer_churn_reuses_fd_watch_and_kernel_registration);
    RUN_TEST(ordinary_timers_never_consume_or_refill_a_warm_reuse_pool);
    RUN_TEST(ordinary_allocation_failure_preserves_warm_pool_and_original_call_order);
    RUN_TEST(idle_fd_retention_is_bounded_and_destroy_closes_every_timer);
    RUN_TEST(disarming_clears_unread_expirations_before_reuse);
    RUN_TEST(fd_callback_cannot_reuse_a_cancelled_timer_from_the_same_ready_batch);
    RUN_TEST(timer_callback_cannot_insert_a_child_into_the_current_ready_batch);
    RUN_TEST(callback_can_cancel_itself_and_a_ready_peer_then_free_its_userdata);
    RUN_TEST(reusable_oneshot_callback_may_free_userdata_without_deleting_itself);
    RUN_TEST(allocation_and_kernel_registration_failures_leave_no_live_timer);
    RUN_TEST(failed_rearm_discards_cached_timer_and_preserves_other_active_timer);
    RUN_TEST(failed_disarm_closes_timer_even_if_epoll_delete_also_fails);
    RUN_TEST(epoll_wait_error_releases_idle_timers_but_preserves_live_ids);
    RUN_TEST(timer_id_wrap_avoids_signed_overflow_and_live_id_collisions);
    RUN_TEST(delayed_periodic_expirations_coalesce_without_changing_kernel_interval);
    GREATEST_MAIN_END();
}
