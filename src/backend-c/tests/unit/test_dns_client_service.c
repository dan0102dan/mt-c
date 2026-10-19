#include "greatest.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "magitrickle/dns_clients.h"

typedef struct {
    mt_loop_t *loop;
    mt_dns_clients_t *clients;
    pthread_t owner;
    atomic_uint phase, fetches;
    atomic_bool wrong_thread;
    unsigned notifications, ticks;
    uint64_t revision;
    bool failed, timed_out;
} fixture_t;
static mt_err_t fake_fetch(mt_dns_client_t **out, size_t *count, const atomic_bool *stop, void *ud)
{
    fixture_t *f = ud;
    *out = NULL; *count = 0;
    if (pthread_equal(pthread_self(), f->owner)) { atomic_store(&f->wrong_thread, true); }
    atomic_fetch_add(&f->fetches, 1);
    /* The event loop must remain responsive while an optional upstream is slow. */
    struct timespec delay = {.tv_nsec = 50000000};
    nanosleep(&delay, NULL);
    if (atomic_load(stop)) { return MT_ERR_CANCELED; }
    unsigned phase = atomic_load(&f->phase);
    if (phase == 2) { return MT_ERR_UPSTREAM; }
    mt_dns_client_t *entries = calloc(1, sizeof(*entries));
    if (!entries) { return MT_ERR_NOMEM; }
    strcpy(entries[0].ip, "192.168.1.42");
    strcpy(entries[0].name, phase == 0 ? "Phone" : phase == 1 ? "Renamed phone" : "Back online");
    *out = entries; *count = 1;
    return MT_OK;
}
static void changed(void *ud)
{
    fixture_t *f = ud;
    unsigned phase = atomic_load(&f->phase);
    const char *name = mt_dns_clients_lookup(f->clients, "::ffff:192.168.1.42");
    const char *expected = phase == 0 ? "Phone" : phase == 1 ? "Renamed phone" : "Back online";
    if (phase == 2 ? name != NULL : !name || strcmp(name, expected)) { f->failed = true; }
    if (mt_dns_clients_revision(f->clients) <= f->revision) { f->failed = true; }
    f->revision = mt_dns_clients_revision(f->clients);
    f->notifications++;
    if (phase == 3) { mt_loop_stop(f->loop); return; }
    atomic_store(&f->phase, phase + 1);
    /* Ten simultaneous requests must coalesce into just one worker fetch. */
    for (unsigned i = 0; i < 10; i++) { mt_dns_clients_refresh(f->clients); }
}
static void tick(mt_loop_t *loop, void *ud) { (void)loop; ((fixture_t *)ud)->ticks++; }
static void deadline(mt_loop_t *loop, void *ud)
{
    ((fixture_t *)ud)->timed_out = true;
    mt_loop_stop(loop);
}
TEST optional_discovery_is_async_coalesced_and_updates_existing_labels(void)
{
    fixture_t f = {0};
    f.owner = pthread_self();
    atomic_init(&f.phase, 0); atomic_init(&f.fetches, 0); atomic_init(&f.wrong_thread, false);
    ASSERT_EQ(MT_OK, mt_loop_create(&f.loop));
    ASSERT_EQ(MT_OK, mt_dns_clients_create(f.loop, fake_fetch, &f, &f.clients));
    int subscription = mt_dns_clients_subscribe(f.clients, changed, &f);
    ASSERT(subscription > 0);
    int ticker = 0, watchdog = 0;
    ASSERT_EQ(MT_OK, mt_loop_add_timer(f.loop, 2, 2, tick, &f, &ticker));
    ASSERT_EQ(MT_OK, mt_loop_add_timer(f.loop, 5000, 0, deadline, &f, &watchdog));
    ASSERT_EQ(MT_OK, mt_loop_run(f.loop));
    ASSERT(!f.failed); ASSERT(!f.timed_out); ASSERT(!atomic_load(&f.wrong_thread));
    ASSERT_EQ(4u, atomic_load(&f.fetches)); ASSERT_EQ(4u, f.notifications);
    ASSERT(f.ticks > 2);
    ASSERT_STR_EQ("Back online", mt_dns_clients_lookup(f.clients, "192.168.1.42"));
    ASSERT(mt_dns_clients_lookup(f.clients, "2001:db8::42") == NULL);
    mt_dns_clients_unsubscribe(f.clients, subscription);
    ASSERT(mt_dns_clients_lookup(f.clients, "192.168.1.42") == NULL);
    mt_dns_clients_refresh(f.clients);
    mt_dns_clients_destroy(f.clients);
    mt_loop_destroy(f.loop);
    ASSERT_EQ(4u, atomic_load(&f.fetches));
    PASS();
}
TEST generic_build_has_no_implicit_router_discovery(void)
{
#if !defined(MT_ENTWARE_KN) && !defined(MT_PLATFORM_OPENWRT)
    mt_loop_t *loop = NULL; mt_dns_clients_t *clients = NULL;
    ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    ASSERT_EQ(MT_OK, mt_dns_clients_create(loop, NULL, NULL, &clients));
    ASSERT(clients == NULL);
    mt_loop_destroy(loop);
#endif
    ASSERT(mt_dns_clients_lookup(NULL, "192.168.1.1") == NULL);
    ASSERT_EQ(0u, mt_dns_clients_revision(NULL));
    mt_dns_clients_destroy(NULL);
    PASS();
}
GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(optional_discovery_is_async_coalesced_and_updates_existing_labels);
    RUN_TEST(generic_build_has_no_implicit_router_discovery);
    GREATEST_MAIN_END();
}
