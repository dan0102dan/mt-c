#include "greatest.h"
#include "magitrickle/dns_clients.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    mt_loop_t *loop;
    mt_dns_clients_t *clients;
    atomic_uint fetches;
    atomic_bool release;
    unsigned hold_fetch, notifications;
    int subscription;
    bool reopen, event_sent, timed_out, failed;
} fixture_t;

static mt_err_t fetch(mt_dns_client_t **out, size_t *count, const atomic_bool *stop, void *ud)
{
    fixture_t *f = ud;
    *out = NULL; *count = 0;
    unsigned n = atomic_fetch_add(&f->fetches, 1) + 1;
    while (n == f->hold_fetch && !atomic_load(&f->release) && !atomic_load(stop)) {
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
    if (atomic_load(stop)) { return MT_ERR_CANCELED; }
    *out = calloc(1, sizeof(**out));
    if (!*out) { return MT_ERR_NOMEM; }
    *count = 1;
    strcpy((*out)[0].ip, "fe80::1");
    (void)snprintf((*out)[0].name, sizeof((*out)[0].name), "Device %u", n);
    return MT_OK;
}
static void changed(void *ud)
{
    fixture_t *f = ud;
    f->notifications++;
    if (f->reopen) {
        const char *name = mt_dns_clients_lookup(f->clients, "fe80::1");
        if (!name || strcmp(name, "Device 2")) { f->failed = true; }
        mt_loop_stop(f->loop);
    } else {
        if (f->notifications == 1) {
            for (unsigned i = 0; i < 100; i++) { mt_dns_clients_invalidate(f->clients); }
        }
        if (f->notifications == 3) { mt_loop_stop(f->loop); }
    }
}
static void release_fetch(mt_loop_t *loop, void *ud)
{
    (void)loop;
    atomic_store(&((fixture_t *)ud)->release, true);
}
static void during_fetch(mt_loop_t *loop, void *ud)
{
    fixture_t *f = ud;
    if (f->event_sent || atomic_load(&f->fetches) != f->hold_fetch) { return; }
    f->event_sent = true;
    if (f->reopen) {
        mt_dns_clients_unsubscribe(f->clients, f->subscription);
        f->subscription = mt_dns_clients_subscribe(f->clients, changed, f);
        if (!f->subscription) { f->failed = true; }
        atomic_store(&f->release, true);
    } else {
        for (unsigned i = 0; i < 100; i++) { mt_dns_clients_invalidate(f->clients); }
        /* The debounce expires while the snapshot is still in progress. */
        int timer = 0;
        if (mt_loop_add_timer(loop, 1200, 0, release_fetch, f, &timer) != MT_OK) {
            f->failed = true; mt_loop_stop(loop);
        }
    }
}
static void deadline(mt_loop_t *loop, void *ud)
{
    ((fixture_t *)ud)->timed_out = true; mt_loop_stop(loop);
}
static bool setup(fixture_t *f)
{
    atomic_init(&f->fetches, 0); atomic_init(&f->release, false);
    if (mt_loop_create(&f->loop) != MT_OK ||
        mt_dns_clients_create(f->loop, fetch, f, &f->clients) != MT_OK) { return false; }
    /* No viewer means no request, even if the router emits a burst. */
    for (unsigned i = 0; i < 100; i++) { mt_dns_clients_invalidate(f->clients); }
    f->subscription = mt_dns_clients_subscribe(f->clients, changed, f);
    int poll = 0, watchdog = 0;
    return f->subscription &&
        mt_loop_add_timer(f->loop, 5, 5, during_fetch, f, &poll) == MT_OK &&
        mt_loop_add_timer(f->loop, 6000, 0, deadline, f, &watchdog) == MT_OK;
}
static void cleanup(fixture_t *f)
{
    mt_dns_clients_unsubscribe(f->clients, f->subscription);
    mt_dns_clients_invalidate(f->clients);
    mt_dns_clients_destroy(f->clients);
    mt_loop_destroy(f->loop);
}
TEST source_events_coalesce_and_survive_an_inflight_snapshot(void)
{
    fixture_t f = {.hold_fetch = 2};
    ASSERT(setup(&f));
    ASSERT_EQ(MT_OK, mt_loop_run(f.loop));
    bool correct = !f.timed_out && !f.failed && f.event_sent && f.notifications == 3 &&
        atomic_load(&f.fetches) == 3 && mt_dns_clients_lookup(f.clients, "fe80::1") &&
        !strcmp(mt_dns_clients_lookup(f.clients, "fe80::1"), "Device 3");
    cleanup(&f);
    ASSERT(correct);
    ASSERT_EQ(3u, atomic_load(&f.fetches));
    PASS();
}
TEST a_reopened_viewer_never_receives_the_previous_snapshot(void)
{
    fixture_t f = {.hold_fetch = 1, .reopen = true};
    ASSERT(setup(&f));
    ASSERT_EQ(MT_OK, mt_loop_run(f.loop));
    cleanup(&f);
    ASSERT(!f.timed_out); ASSERT(!f.failed); ASSERT(f.event_sent);
    ASSERT_EQ(1u, f.notifications); ASSERT_EQ(2u, atomic_load(&f.fetches));
    PASS();
}
GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(source_events_coalesce_and_survive_an_inflight_snapshot);
    RUN_TEST(a_reopened_viewer_never_receives_the_previous_snapshot);
    GREATEST_MAIN_END();
}
