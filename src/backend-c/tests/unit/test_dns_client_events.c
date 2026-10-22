#include "greatest.h"
#include "magitrickle/dns_neighbors.h"
#ifdef MT_ENTWARE_KN
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int event_pipe[2];
static atomic_uint fetches;
static unsigned notifications;
static mt_loop_t *event_loop;
static bool timed_out;
int __wrap_mt_dns_neighbors_watch_open(void);
mt_err_t __wrap_mt_dns_neighbors_watch_drain(int fd, bool *changed);
mt_err_t __wrap_mt_dns_clients_fetch_keenetic_cached(mt_dns_client_t **out, size_t *count,
    const atomic_bool *stop, mt_dns_hotspot_cache_t *cache);
int __wrap_mt_dns_neighbors_watch_open(void) { return event_pipe[0]; }
mt_err_t __wrap_mt_dns_neighbors_watch_drain(int fd, bool *changed)
{
    char bytes[128];
    *changed = read(fd, bytes, sizeof(bytes)) > 0;
    return MT_OK;
}
mt_err_t __wrap_mt_dns_clients_fetch_keenetic_cached(mt_dns_client_t **out, size_t *count,
    const atomic_bool *stop, mt_dns_hotspot_cache_t *cache)
{
    (void)cache;
    unsigned n = atomic_fetch_add(&fetches, 1) + 1;
    /* A notification racing the dump must cause a subsequent refresh. */
    if (n == 2) { if (write(event_pipe[1], "event", 5) != 5) { abort(); } }
    struct timespec delay = {.tv_nsec = 50000000};
    nanosleep(&delay, NULL);
    if (atomic_load(stop)) { return MT_ERR_CANCELED; }
    *out = calloc(1, sizeof(**out));
    if (!*out) { return MT_ERR_NOMEM; }
    *count = 1;
    strcpy((*out)[0].ip, "fe80::1");
    (void)snprintf((*out)[0].name, sizeof((*out)[0].name), "Device %u", n);
    return MT_OK;
}
static void event_changed(void *ud)
{
    (void)ud;
    notifications++;
    if (notifications == 1) { if (write(event_pipe[1], "burst of notifications", 22) != 22) { abort(); } }
    if (notifications == 3) { mt_loop_stop(event_loop); }
}
static void event_deadline(mt_loop_t *loop, void *ud)
{
    (void)ud; timed_out = true; mt_loop_stop(loop);
}
TEST neighbor_events_coalesce_preserve_races_and_release_watch(void)
{
    atomic_init(&fetches, 0); notifications = 0; timed_out = false;
    ASSERT_EQ(0, pipe(event_pipe));
    ASSERT_EQ(0, fcntl(event_pipe[0], F_SETFL, O_NONBLOCK));
    ASSERT_EQ(MT_OK, mt_loop_create(&event_loop));
    mt_dns_clients_t *clients = NULL;
    ASSERT_EQ(MT_OK, mt_dns_clients_create(event_loop, NULL, NULL, &clients));
    int sub = mt_dns_clients_subscribe(clients, event_changed, NULL);
    ASSERT(sub > 0);
    int watchdog = 0;
    ASSERT_EQ(MT_OK, mt_loop_add_timer(event_loop, 5000, 0, event_deadline, NULL, &watchdog));
    ASSERT_EQ(MT_OK, mt_loop_run(event_loop));
    ASSERT(!timed_out); ASSERT_EQ(3u, notifications); ASSERT_EQ(3u, atomic_load(&fetches));
    ASSERT_STR_EQ("Device 3", mt_dns_clients_lookup(clients, "fe80::1"));
    mt_dns_clients_unsubscribe(clients, sub);
    ASSERT_EQ(-1, fcntl(event_pipe[0], F_GETFD)); ASSERT_EQ(EBADF, errno);
    ASSERT(mt_dns_clients_lookup(clients, "fe80::1") == NULL);
    mt_dns_clients_destroy(clients);
    mt_loop_destroy(event_loop); close(event_pipe[1]);
    PASS();
}

#endif
GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
#ifdef MT_ENTWARE_KN
    RUN_TEST(neighbor_events_coalesce_preserve_races_and_release_watch);
#endif
    GREATEST_MAIN_END();
}
