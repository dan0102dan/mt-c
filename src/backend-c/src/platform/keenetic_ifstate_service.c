/* Hook ingestion + bounded snapshot worker. The event and route paths never
 * wait for RCI. One outstanding read; retries only recover failed reads or
 * explicitly reported loss, not a periodic poll of interface readiness. */
#include "magitrickle/keenetic_ifstate_service.h"
#include "magitrickle/log.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct refresh_waiter {
    struct refresh_waiter *next;
    mt_kn_ifrefresh_fn done;
    void *ud;
} refresh_waiter_t;

struct mt_kn_ifstate_service {
    mt_loop_t *loop;
    mt_kn_ifstate_t *cache;
    mt_kn_ifstate_fetch_fn fetch;
    int pipefd[2], retry_timer, watch_fd, watch_retry_timer;
    char *marker, *watch_dir;
    const char *marker_name; /* points into marker */
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    atomic_bool stopping;
    bool thread_started, fd_registered, requested, completed;
    mt_kn_ifinfo_t *result;
    size_t count;
    mt_err_t error;
    /* Loop owner only. */
    bool inflight, again;
    uint64_t token;
    unsigned retry_ms;
    mt_err_t last_error;
    refresh_waiter_t *waiting, *active;
    size_t n_waiters;
};

static void start_read(mt_kn_ifstate_service_t *s);
static void watch_start(mt_kn_ifstate_service_t *s);
static void retry_read(mt_loop_t *loop, void *ud)
{
    (void)loop;
    mt_kn_ifstate_service_t *s = ud;
    s->retry_timer = 0;
    start_read(s);
}
static void schedule_read(mt_kn_ifstate_service_t *s, unsigned delay)
{
    if (s->retry_timer || atomic_load(&s->stopping)) { return; }
    if (mt_loop_add_timer(s->loop, delay, 0, retry_read, s, &s->retry_timer) != MT_OK) {
        MT_WARN("NDM interface snapshot retry could not be scheduled");
    }
}
static void start_read(mt_kn_ifstate_service_t *s)
{
    if (atomic_load(&s->stopping)) { return; }
    if (s->inflight) { s->again = true; return; }
    s->inflight = true;
    s->active = s->waiting; s->waiting = NULL;
    s->again = false;
    s->token = mt_kn_ifstate_revision(s->cache);
    /* Consume the loss marker at READ START. A later delivery failure then
     * creates a new marker/event instead of being erased by a stale result. */
    if (s->marker) { (void)rmdir(s->marker); }
    pthread_mutex_lock(&s->mutex);
    s->requested = true;
    pthread_cond_signal(&s->ready);
    pthread_mutex_unlock(&s->mutex);
}
void mt_kn_ifstate_service_resync(mt_kn_ifstate_service_t *s)
{
    if (!s || atomic_load(&s->stopping)) { return; }
    if (s->inflight) { s->again = true; }
    else { schedule_read(s, 20); }
}
mt_err_t mt_kn_ifstate_service_refresh(mt_kn_ifstate_service_t *s,
                                       mt_kn_ifrefresh_fn done, void *ud)
{
    if (!s || !done || atomic_load(&s->stopping)) { return MT_ERR_STATE; }
    if (s->n_waiters >= 64) { return MT_ERR_LIMIT; }
    refresh_waiter_t *w = malloc(sizeof(*w));
    if (!w) { return MT_ERR_NOMEM; }
    *w = (refresh_waiter_t){.next = s->waiting, .done = done, .ud = ud};
    s->waiting = w; s->n_waiters++;
    if (s->inflight) { s->again = true; }
    else {
        if (s->retry_timer) { mt_loop_del_timer(s->loop, s->retry_timer); s->retry_timer = 0; }
        start_read(s);
    }
    return MT_OK;
}
static void finish_waiters(mt_kn_ifstate_service_t *s, refresh_waiter_t *w, mt_err_t error)
{
    while (w) {
        refresh_waiter_t *next = w->next;
        s->n_waiters--;
        w->done(w->ud, error);
        free(w); w = next;
    }
}
static void *snapshot_worker(void *ud)
{
    mt_kn_ifstate_service_t *s = ud;
    for (;;) {
        pthread_mutex_lock(&s->mutex);
        while (!s->requested && !atomic_load(&s->stopping)) { pthread_cond_wait(&s->ready, &s->mutex); }
        if (atomic_load(&s->stopping)) { pthread_mutex_unlock(&s->mutex); break; }
        s->requested = false;
        pthread_mutex_unlock(&s->mutex);
        mt_kn_ifinfo_t *items = NULL;
        size_t count = 0;
        mt_err_t error = s->fetch(&items, &count, &s->stopping);
        pthread_mutex_lock(&s->mutex);
        s->result = items; s->count = count; s->error = error; s->completed = true;
        pthread_mutex_unlock(&s->mutex);
        ssize_t n;
        do { n = write(s->pipefd[1], "x", 1); } while (n < 0 && errno == EINTR);
    }
    return NULL;
}
static void completed(mt_loop_t *loop, int fd, uint32_t events, void *ud)
{
    (void)loop; (void)events;
    mt_kn_ifstate_service_t *s = ud;
    char buffer[64];
    for (;;) {
        ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n > 0 || (n < 0 && errno == EINTR)) { continue; }
        break;
    }
    pthread_mutex_lock(&s->mutex);
    if (!s->completed) { pthread_mutex_unlock(&s->mutex); return; }
    mt_kn_ifinfo_t *items = s->result;
    size_t count = s->count;
    mt_err_t error = s->error;
    s->completed = false; s->result = NULL;
    pthread_mutex_unlock(&s->mutex);
    s->inflight = false;
    refresh_waiter_t *finished = s->active; s->active = NULL;
    if (error == MT_OK) { error = mt_kn_ifstate_snapshot(s->cache, items, count, s->token); }
    free(items);
    if (error != MT_OK) {
        if (error != s->last_error) {
            MT_WARN("NDM interface snapshot unavailable: %s; retaining known state, unknown interfaces use kernel checks", mt_err_str(error));
        }
        s->retry_ms = !s->retry_ms ? 1000u : s->retry_ms >= 15000u ? 30000u : s->retry_ms * 2u;
        schedule_read(s, s->retry_ms);
    } else {
        if (s->last_error != MT_OK) { MT_INFO("NDM interface snapshot recovered"); }
        s->retry_ms = 0;
        if (s->again) { schedule_read(s, 20); }
    }
    s->last_error = error;
    /* API callers queued during the old read require a fresh read even if it
     * failed. Do not let a retry-timer allocation failure strand a response. */
    if (s->waiting) {
        if (s->retry_timer) { mt_loop_del_timer(s->loop, s->retry_timer); s->retry_timer = 0; }
        start_read(s);
    }
    finish_waiters(s, finished, error);
}
static void handle_hook(mt_http_req_t *req, mt_http_res_t *res, void *ud)
{
    mt_kn_ifstate_service_t *s = ud;
    size_t len;
    const uint8_t *body = mt_http_req_body(req, &len);
    mt_kn_ifevent_t event;
    mt_err_t err = mt_kn_ifevent_parse((const char *)body, len, &event);
    if (err != MT_OK) {
        mt_http_res_write_error(res, err == MT_ERR_LIMIT ? 413 : 400, "Invalid NDM interface notification");
        return;
    }
    bool resync = false;
    err = mt_kn_ifstate_event(s->cache, &event, &resync);
    if (err != MT_OK) {
        mt_kn_ifstate_service_resync(s);
        mt_http_res_write_error(res, 503, "NDM interface notification could not be retained");
        return;
    }
    if (resync) { mt_kn_ifstate_service_resync(s); }
    /* Receipt acknowledged, not route application. The loop's coalesced
     * reconciler handles kernel writes independently of the NDM hook queue. */
    mt_http_res_write(res, 202, NULL, NULL, 0);
}
mt_err_t mt_kn_ifstate_register_hook(mt_httpd_t *http, mt_kn_ifstate_service_t *s)
{
    return mt_httpd_route(http, "POST", MT_KN_IFSTATE_HOOK_PATH, handle_hook, s);
}
static void watch_retry(mt_loop_t *loop, void *ud)
{
    (void)loop;
    mt_kn_ifstate_service_t *s = ud;
    s->watch_retry_timer = 0;
    watch_start(s);
}
static void watch_close(mt_kn_ifstate_service_t *s)
{
    if (s->watch_fd < 0) { return; }
    mt_loop_del_fd(s->loop, s->watch_fd);
    close(s->watch_fd); s->watch_fd = -1;
}
static void watch_read(mt_loop_t *loop, int fd, uint32_t events, void *ud)
{
    (void)loop;
    mt_kn_ifstate_service_t *s = ud;
    /* Align the event buffer without relying on a char array's alignment. */
    union { struct inotify_event align; char bytes[4096]; } buffer;
    bool restart = (events & (EPOLLERR | EPOLLHUP)) != 0;
    for (unsigned batch = 0; !restart && batch < 8; batch++) {
        ssize_t n = read(fd, buffer.bytes, sizeof(buffer.bytes));
        if (n < 0 && errno == EINTR) { continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { break; }
        if (n <= 0) { restart = true; break; }
        size_t pos = 0;
        while (sizeof(struct inotify_event) <= (size_t)n - pos) {
            const struct inotify_event *event = (const void *)(buffer.bytes + pos);
            if (event->len > (size_t)n - pos - sizeof(*event)) { restart = true; break; }
            if (event->mask & (IN_Q_OVERFLOW | IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)) {
                mt_kn_ifstate_service_resync(s);
                if (!(event->mask & IN_Q_OVERFLOW)) { restart = true; }
            }
            if (event->len && !strncmp(event->name, s->marker_name, event->len) &&
                (event->mask & (IN_CREATE | IN_MOVED_TO))) { mt_kn_ifstate_service_resync(s); }
            pos += sizeof(*event) + event->len;
        }
    }
    if (restart) {
        watch_close(s);
        mt_kn_ifstate_service_resync(s);
        if (!s->watch_retry_timer) {
            (void)mt_loop_add_timer(s->loop, 1000, 0, watch_retry, s, &s->watch_retry_timer);
        }
    }
}
static void watch_start(mt_kn_ifstate_service_t *s)
{
    if (!s->marker || atomic_load(&s->stopping) || s->watch_fd >= 0) { return; }
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd >= 0) {
        int wd = inotify_add_watch(fd, s->watch_dir, IN_CREATE | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF);
        if (wd >= 0 && mt_loop_add_fd(s->loop, fd, EPOLLIN, watch_read, s) == MT_OK) { s->watch_fd = fd; }
        else { close(fd); }
    }
    if (s->watch_fd < 0 && !s->watch_retry_timer) {
        MT_WARN("NDM delivery-loss watcher unavailable; recovery still occurs on restart/next reported loss");
        (void)mt_loop_add_timer(s->loop, 30000, 0, watch_retry, s, &s->watch_retry_timer);
    }
    struct stat st;
    if (lstat(s->marker, &st) == 0) { mt_kn_ifstate_service_resync(s); }
}
mt_err_t mt_kn_ifstate_service_create(mt_loop_t *loop, mt_kn_ifstate_t *cache,
                                     const char *marker, mt_kn_ifstate_fetch_fn fetch,
                                     mt_kn_ifstate_service_t **out)
{
    if (!loop || !cache || !out) { return MT_ERR_INVAL; }
    *out = NULL;
    mt_kn_ifstate_service_t *s = calloc(1, sizeof(*s));
    if (!s) { return MT_ERR_NOMEM; }
    s->loop = loop; s->cache = cache; s->fetch = fetch ? fetch : mt_kn_ifstate_fetch;
    s->pipefd[0] = s->pipefd[1] = s->watch_fd = -1;
    atomic_init(&s->stopping, false);
    if (pthread_mutex_init(&s->mutex, NULL)) { free(s); return MT_ERR_SYS; }
    if (pthread_cond_init(&s->ready, NULL)) { pthread_mutex_destroy(&s->mutex); free(s); return MT_ERR_SYS; }
    mt_err_t error = MT_ERR_SYS;
    if (marker) {
        s->marker = strdup(marker); s->watch_dir = strdup(marker);
        if (!s->marker || !s->watch_dir) { error = MT_ERR_NOMEM; goto fail; }
        char *slash = strrchr(s->watch_dir, '/');
        if (!slash || slash == s->watch_dir || !slash[1]) { error = MT_ERR_INVAL; goto fail; }
        *slash = '\0'; s->marker_name = strrchr(s->marker, '/') + 1;
    }
    if (pipe(s->pipefd)) { goto fail; }
    for (size_t i = 0; i < 2; i++) {
        int flags = fcntl(s->pipefd[i], F_GETFL);
        if (flags < 0 || fcntl(s->pipefd[i], F_SETFL, flags | O_NONBLOCK) < 0 ||
            fcntl(s->pipefd[i], F_SETFD, FD_CLOEXEC) < 0) { goto fail; }
    }
    error = mt_loop_add_fd(loop, s->pipefd[0], EPOLLIN, completed, s);
    if (error != MT_OK) { goto fail; }
    s->fd_registered = true;
    if (pthread_create(&s->thread, NULL, snapshot_worker, s)) { error = MT_ERR_SYS; goto fail; }
    s->thread_started = true;
    start_read(s);
    watch_start(s);
    *out = s;
    return MT_OK;
fail:
    mt_kn_ifstate_service_destroy(s);
    return error;
}
void mt_kn_ifstate_service_destroy(mt_kn_ifstate_service_t *s)
{
    if (!s) { return; }
    atomic_store(&s->stopping, true);
    pthread_mutex_lock(&s->mutex);
    pthread_cond_signal(&s->ready);
    pthread_mutex_unlock(&s->mutex);
    if (s->thread_started) { pthread_join(s->thread, NULL); }
    if (s->retry_timer) { mt_loop_del_timer(s->loop, s->retry_timer); }
    if (s->watch_retry_timer) { mt_loop_del_timer(s->loop, s->watch_retry_timer); }
    watch_close(s);
    if (s->fd_registered) { mt_loop_del_fd(s->loop, s->pipefd[0]); }
    for (size_t i = 0; i < 2; i++) { if (s->pipefd[i] >= 0) { close(s->pipefd[i]); } }
    finish_waiters(s, s->active, MT_ERR_CANCELED);
    finish_waiters(s, s->waiting, MT_ERR_CANCELED);
    free(s->result); free(s->marker); free(s->watch_dir);
    pthread_cond_destroy(&s->ready); pthread_mutex_destroy(&s->mutex);
    free(s);
}
