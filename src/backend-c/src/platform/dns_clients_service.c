/* Loop-owned label inventory and a single coalesced worker. Labels are optional:
 * router discovery failure must never delay DNS or prevent opening the journal. */
#include "magitrickle/dns_clients.h"
#include "magitrickle/dns_openwrt.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

typedef struct { mt_dns_clients_notify_fn fn; void *ud; } client_listener_t;
struct mt_dns_clients {
    mt_loop_t *loop;
    mt_dns_clients_fetch_fn fetch;
    void *fetch_ud;
    int notify[2], timer;
    bool registered, thread_started, mutex_ready, cond_ready;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t thread;
    atomic_bool stopping;
    /* Loop-owned invalidation debounce. The worker reads complete snapshots. */
    int event_timer;
    bool dirty;
    /* Worker commands/results, guarded by mu. */
    uint64_t epoch, result_epoch;
    bool source_active;
#ifdef MT_PLATFORM_OPENWRT
    bool openwrt;
    int control[2];
#endif
    bool requested, result_ready; /* guarded by mu */
    mt_dns_client_t *result;
    size_t result_count;
    mt_err_t result_error;
    /* Remaining fields are touched on the loop thread only. */
    bool inflight;
    size_t listeners;
    client_listener_t subscriptions[MT_DNS_CLIENT_SUBSCRIBERS];
    mt_dns_client_t *entries;
    size_t count;
    uint64_t revision;
};

size_t mt_dns_clients_count(const mt_dns_clients_t *s) { return s ? s->count : 0; }
const mt_dns_client_t *mt_dns_clients_at(const mt_dns_clients_t *s, size_t index)
{
    return s && index < s->count ? &s->entries[index] : NULL;
}
uint64_t mt_dns_clients_revision(const mt_dns_clients_t *s)
{
    return s ? s->revision : 0;
}
const char *mt_dns_clients_lookup(const mt_dns_clients_t *s, const char *address)
{
    char ip[MT_DNS_CLIENT_IP_LEN];
    if (!s || !mt_dns_client_address(address, ip, sizeof(ip))) { return NULL; }
    size_t lo = 0, hi = s->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(ip, s->entries[mid].ip);
        if (!cmp) { return s->entries[mid].name; }
        if (cmp < 0) { hi = mid; } else { lo = mid + 1; }
    }
    return NULL;
}
/* Wake either the native ubus worker or the ordinary fetch worker. */
static void wake_worker(mt_dns_clients_t *s)
{
#ifdef MT_PLATFORM_OPENWRT
    if (s->openwrt) {
        ssize_t written;
        do { written = write(s->control[1], "x", 1); } while (written < 0 && errno == EINTR);
        return; /* EAGAIN means a wakeup is already queued. */
    }
#endif
    pthread_cond_signal(&s->cv);
}
#ifdef MT_PLATFORM_OPENWRT
static mt_dns_source_command_t native_command(void *ud)
{
    mt_dns_clients_t *s = ud;
    pthread_mutex_lock(&s->mu);
    mt_dns_source_command_t command = {.active = s->source_active,
        .refresh = s->requested, .epoch = s->epoch};
    s->requested = false;
    pthread_mutex_unlock(&s->mu);
    return command;
}
static void native_publish(void *ud, uint64_t epoch, mt_dns_client_t *entries,
                            size_t count, mt_err_t error)
{
    mt_dns_clients_t *s = ud;
    pthread_mutex_lock(&s->mu);
    /* Metadata is a replaceable snapshot; bound queued results to one map. */
    free(s->result);
    s->result = entries; s->result_count = count; s->result_error = error;
    s->result_epoch = epoch; s->result_ready = true;
    pthread_mutex_unlock(&s->mu);
    ssize_t written;
    do { written = write(s->notify[1], "x", 1); } while (written < 0 && errno == EINTR);
}
#endif
void mt_dns_clients_refresh(mt_dns_clients_t *s)
{
    if (!s || !s->listeners || atomic_load(&s->stopping)) { return; }
#ifdef MT_PLATFORM_OPENWRT
    if (s->openwrt) {
        /* The native event loop owns request coalescing and cancellation. */
        pthread_mutex_lock(&s->mu);
        s->requested = true; wake_worker(s);
        pthread_mutex_unlock(&s->mu);
        return;
    }
#endif
    if (s->inflight) { return; }
    s->dirty = false;
    if (s->event_timer) { (void)mt_loop_del_timer(s->loop, s->event_timer); s->event_timer = 0; }
    s->inflight = true;
    pthread_mutex_lock(&s->mu);
    s->requested = true;
    wake_worker(s);
    pthread_mutex_unlock(&s->mu);
}
static void *client_worker(void *ud)
{
    mt_dns_clients_t *s = ud;
#ifdef MT_PLATFORM_OPENWRT
    if (s->openwrt) {
        const mt_dns_openwrt_worker_t worker = {.control_fd = s->control[0], .stop = &s->stopping,
            .ud = s, .command = native_command, .publish = native_publish};
        mt_dns_openwrt_run(&worker);
        return NULL;
    }
#endif
    for (;;) {
        pthread_mutex_lock(&s->mu);
        while (!s->requested && !atomic_load(&s->stopping)) { pthread_cond_wait(&s->cv, &s->mu); }
        if (atomic_load(&s->stopping)) { pthread_mutex_unlock(&s->mu); break; }
        s->requested = false;
        uint64_t epoch = s->epoch;
        pthread_mutex_unlock(&s->mu);
        mt_dns_client_t *entries = NULL;
        size_t count = 0;
        mt_err_t err = s->fetch(&entries, &count, &s->stopping, s->fetch_ud);
        if (count > MT_DNS_CLIENT_LIMIT || (count && !entries)) { err = MT_ERR_LIMIT; }
        pthread_mutex_lock(&s->mu);
        s->result_epoch = epoch;
        s->result = entries; s->result_count = count; s->result_error = err; s->result_ready = true;
        pthread_mutex_unlock(&s->mu);
        ssize_t n;
        do { n = write(s->notify[1], "x", 1); } while (n < 0 && errno == EINTR);
    }
    return NULL;
}
static bool same_clients(const mt_dns_clients_t *s, const mt_dns_client_t *entries, size_t count)
{
    if (s->count != count) { return false; }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(s->entries[i].ip, entries[i].ip) || strcmp(s->entries[i].name, entries[i].name)) { return false; }
    }
    return true;
}
static void source_schedule(mt_dns_clients_t *s);
static void clients_completed(mt_loop_t *loop, int fd, uint32_t events, void *ud)
{
    (void)loop; (void)events;
    mt_dns_clients_t *s = ud;
    char buf[64];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0 || (n < 0 && errno == EINTR)) { continue; }
        break;
    }
    pthread_mutex_lock(&s->mu);
    if (!s->result_ready) { pthread_mutex_unlock(&s->mu); return; }
    mt_dns_client_t *entries = s->result;
    size_t count = s->result_count;
    mt_err_t err = s->result_error;
    bool stale = s->result_epoch != s->epoch;
    s->result = NULL; s->result_count = 0; s->result_ready = false;
    pthread_mutex_unlock(&s->mu);
    s->inflight = false;
    source_schedule(s);
    if (stale) {
        free(entries);
        mt_dns_clients_refresh(s);
        return;
    }
    /* Do not display a stale name for a reassigned address after a failure,
     * and do not keep device inventories alive without an open viewer. */
    if (err != MT_OK || !s->listeners || count > MT_DNS_CLIENT_LIMIT || (count && !entries)) {
        free(entries); entries = NULL; count = 0;
    }
    if (same_clients(s, entries, count)) { free(entries); return; }
    free(s->entries); s->entries = entries; s->count = count; s->revision++;
    for (size_t i = 0; i < MT_DNS_CLIENT_SUBSCRIBERS; i++) {
        client_listener_t listener = s->subscriptions[i];
        if (listener.fn) { listener.fn(listener.ud); }
    }
}
static void clients_tick(mt_loop_t *loop, void *ud)
{
    (void)loop;
    mt_dns_clients_t *s = ud;
    if (s->inflight) { mt_dns_clients_invalidate(s); }
    else { mt_dns_clients_refresh(s); }
}
static void source_tick(mt_loop_t *loop, void *ud)
{
    (void)loop;
    mt_dns_clients_t *s = ud;
    s->event_timer = 0;
    if (!s->dirty || s->inflight || !s->listeners) { return; }
    s->dirty = false;
    mt_dns_clients_refresh(s);
}
static void source_schedule(mt_dns_clients_t *s)
{
    if (!s->listeners || !s->dirty || s->event_timer || atomic_load(&s->stopping)) { return; }
    /* One-shot debounce: no timer wakeups while the source is idle. */
    (void)mt_loop_add_timer(s->loop, 1000, 0, source_tick, s, &s->event_timer);
}
void mt_dns_clients_invalidate(mt_dns_clients_t *s)
{
    if (!s || !s->listeners || atomic_load(&s->stopping)) { return; }
    s->dirty = true;
    source_schedule(s);
}
static void source_stop(mt_dns_clients_t *s)
{
    if (s->event_timer) { (void)mt_loop_del_timer(s->loop, s->event_timer); s->event_timer = 0; }
    if (s->timer) { (void)mt_loop_del_timer(s->loop, s->timer); s->timer = 0; }
    s->dirty = false;
}
int mt_dns_clients_subscribe(mt_dns_clients_t *s, mt_dns_clients_notify_fn fn, void *ud)
{
    if (!s || !fn) { return 0; }
    for (size_t i = 0; i < MT_DNS_CLIENT_SUBSCRIBERS; i++) {
        if (s->subscriptions[i].fn) { continue; }
        if (!s->listeners) {
            bool needs_timer = true;
#ifdef MT_PLATFORM_OPENWRT
            needs_timer = !s->openwrt;
#endif
            /* Without reconciliation a one-time snapshot can remain stale
             * indefinitely. Fail optional discovery before activating it;
             * the journal remains available with plain IP addresses. */
            if (needs_timer && !s->timer &&
                mt_loop_add_timer(s->loop, MT_DNS_CLIENT_REFRESH_MS, MT_DNS_CLIENT_REFRESH_MS,
                                  clients_tick, s, &s->timer) != MT_OK) { return 0; }
        }
        s->subscriptions[i] = (client_listener_t){fn, ud};
        s->listeners++;
        if (s->listeners == 1) {
            pthread_mutex_lock(&s->mu);
            s->source_active = true; s->epoch++;
            pthread_mutex_unlock(&s->mu);
            mt_dns_clients_refresh(s);
        }
        return (int)i + 1;
    }
    return 0;
}
void mt_dns_clients_unsubscribe(mt_dns_clients_t *s, int id)
{
    if (!s || id <= 0 || (unsigned)id > MT_DNS_CLIENT_SUBSCRIBERS || !s->subscriptions[id - 1].fn) { return; }
    s->subscriptions[id - 1] = (client_listener_t){0};
    if (--s->listeners == 0) {
        source_stop(s);
        pthread_mutex_lock(&s->mu);
        s->source_active = false; s->epoch++;
        wake_worker(s);
        pthread_mutex_unlock(&s->mu);
        free(s->entries); s->entries = NULL; s->count = 0; s->revision++;
    }
}

mt_err_t mt_dns_clients_create(mt_loop_t *loop, mt_dns_clients_fetch_fn fetch,
                               void *fetch_ud, mt_dns_clients_t **out)
{
    if (!out || !loop) { return MT_ERR_INVAL; }
    *out = NULL;
    bool native_provider = fetch == NULL;
    if (native_provider) {
#ifdef MT_ENTWARE_KN
        fetch = mt_dns_clients_fetch_keenetic;
#elif defined(MT_PLATFORM_OPENWRT)
        /* Native worker owns inventory, subscriptions and reconnects. */
#else
        return MT_OK;
#endif
    }
    mt_dns_clients_t *s = calloc(1, sizeof(*s));
    if (!s) { return MT_ERR_NOMEM; }
    s->loop = loop; s->fetch = fetch; s->fetch_ud = fetch_ud;
#ifdef MT_PLATFORM_OPENWRT
    s->openwrt = native_provider;
    s->control[0] = -1; s->control[1] = -1;
#endif
    s->notify[0] = -1; s->notify[1] = -1;
    atomic_init(&s->stopping, false);
    mt_err_t err = MT_ERR_SYS;
    if (pthread_mutex_init(&s->mu, NULL) != 0) { goto fail; }
    s->mutex_ready = true;
    if (pthread_cond_init(&s->cv, NULL) != 0) { goto fail; }
    s->cond_ready = true;
#ifdef MT_PLATFORM_OPENWRT
    if (s->openwrt) {
        if (pipe(s->control)) { goto fail; }
        for (int i = 0; i < 2; i++) {
            int flags = fcntl(s->control[i], F_GETFL);
            if (flags < 0 || fcntl(s->control[i], F_SETFL, flags | O_NONBLOCK) < 0 ||
                fcntl(s->control[i], F_SETFD, FD_CLOEXEC) < 0) { goto fail; }
        }
    }
#endif
    if (pipe(s->notify) != 0) { goto fail; }
    for (int i = 0; i < 2; i++) {
        int flags = fcntl(s->notify[i], F_GETFL);
        if (flags < 0 || fcntl(s->notify[i], F_SETFL, flags | O_NONBLOCK) < 0 ||
            fcntl(s->notify[i], F_SETFD, FD_CLOEXEC) < 0) { goto fail; }
    }
    err = mt_loop_add_fd(loop, s->notify[0], EPOLLIN, clients_completed, s);
    if (err != MT_OK) { goto fail; }
    s->registered = true;
    if (pthread_create(&s->thread, NULL, client_worker, s) != 0) { err = MT_ERR_SYS; goto fail; }
    s->thread_started = true;
    *out = s;
    return MT_OK;
fail:
    mt_dns_clients_destroy(s);
    return err;
}
void mt_dns_clients_destroy(mt_dns_clients_t *s)
{
    if (!s) { return; }
    atomic_store(&s->stopping, true);
    if (s->thread_started) {
        pthread_mutex_lock(&s->mu); wake_worker(s); pthread_mutex_unlock(&s->mu);
        pthread_join(s->thread, NULL);
    }
    source_stop(s);
#ifdef MT_PLATFORM_OPENWRT
    if (s->control[0] >= 0) { close(s->control[0]); }
    if (s->control[1] >= 0) { close(s->control[1]); }
#endif
    if (s->registered) { (void)mt_loop_del_fd(s->loop, s->notify[0]); }
    if (s->notify[0] >= 0) { close(s->notify[0]); }
    if (s->notify[1] >= 0) { close(s->notify[1]); }
    if (s->cond_ready) { pthread_cond_destroy(&s->cv); }
    if (s->mutex_ready) { pthread_mutex_destroy(&s->mu); }
    free(s->result); free(s->entries); free(s);
}
