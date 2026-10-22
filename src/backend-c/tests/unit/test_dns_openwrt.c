#define _GNU_SOURCE
#include "greatest.h"
#include "magitrickle/dns_openwrt.h"
#ifdef MT_PLATFORM_OPENWRT
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <sys/socket.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <libubus.h>
#include <libubox/blobmsg.h>
extern char **environ;

static void lease(struct blob_buf *blob, const char *name, const char *address, unsigned expires, bool permanent)
{
    void *row = blobmsg_open_table(blob, NULL);
    blobmsg_add_string(blob, "hostname", name);
    if (permanent) { blobmsg_add_u8(blob, "expires", 0); }
    else { blobmsg_add_u32(blob, "expires", expires); }
    blobmsg_add_string(blob, strchr(address, ':') ? "ip6addr" : "ipaddr", address);
    blobmsg_close_table(blob, row);
}
TEST native_snapshot_parses_both_families_expiry_conflicts_and_prefixes(void)
{
    struct blob_buf blob = {0}; ASSERT_EQ(0, blob_buf_init(&blob, 0));
    void *v4 = blobmsg_open_array(&blob, "dhcp_leases");
    lease(&blob, "Phone", "192.168.1.2", 60, false);
    lease(&blob, "Phone", "192.168.1.2", 60, false);
    lease(&blob, "One", "192.168.1.3", 0, true);
    lease(&blob, "Other", "192.168.1.3", 0, true);
    lease(&blob, "Expired", "192.168.1.4", 0, false);
    lease(&blob, "Negative", "192.168.1.5", UINT32_MAX, false);
    lease(&blob, "*", "192.168.1.6", 60, false);
    blobmsg_close_array(&blob, v4);
    void *v6 = blobmsg_open_array(&blob, "dhcp6_leases");
    lease(&blob, "Телефон", "2001:db8::2", 0, true);
    void *row = blobmsg_open_table(&blob, NULL);
    blobmsg_add_string(&blob, "hostname", "DHCP6"); blobmsg_add_u32(&blob, "expires", 30);
    void *addresses = blobmsg_open_array(&blob, "ip6addrs");
    blobmsg_add_string(&blob, NULL, "fd00::3/128");
    blobmsg_add_string(&blob, NULL, "2001:db8::3/128");
    blobmsg_add_string(&blob, NULL, "2001:db8:4::/64");
    blobmsg_close_array(&blob, addresses); blobmsg_close_table(&blob, row);
    blobmsg_close_array(&blob, v6);
    mt_dns_client_t *clients = NULL; size_t count = 0; uint64_t expiry = 0;
    ASSERT_EQ(MT_OK, mt_dns_openwrt_parse(blob.head, blob_raw_len(blob.head), &clients, &count, &expiry));
    ASSERT_EQ(4u, count); ASSERT_EQ(30000u, expiry);
    ASSERT_STR_EQ("192.168.1.2", clients[0].ip); ASSERT_STR_EQ("Phone", clients[0].name);
    ASSERT_STR_EQ("2001:db8::2", clients[1].ip); ASSERT_STR_EQ("Телефон", clients[1].name);
    ASSERT_STR_EQ("2001:db8::3", clients[2].ip); ASSERT_STR_EQ("fd00::3", clients[3].ip);
    free(clients);
    for (size_t n = 0; n < blob_raw_len(blob.head); n++) {
        ASSERT_EQ(MT_ERR_PROTO, mt_dns_openwrt_parse(blob.head, n, &clients, &count, &expiry));
        ASSERT(clients == NULL); ASSERT_EQ(0u, count);
    }
    ASSERT_EQ(MT_ERR_LIMIT, mt_dns_openwrt_parse(blob.head, MT_DNS_CLIENT_BODY_LIMIT + 1, &clients, &count, &expiry));
    blob_buf_free(&blob);
    PASS();
}
TEST malformed_and_oversized_inventory_never_publishes_partial_names(void)
{
    struct blob_buf blob = {0}; ASSERT_EQ(0, blob_buf_init(&blob, 0));
    void *v4 = blobmsg_open_array(&blob, "dhcp_leases");
    for (size_t i = 0; i <= MT_DNS_CLIENT_LIMIT; i++) { lease(&blob, "Phone", "192.168.1.2", 60, false); }
    blobmsg_close_array(&blob, v4);
    void *v6 = blobmsg_open_array(&blob, "dhcp6_leases"); blobmsg_close_array(&blob, v6);
    mt_dns_client_t *clients = NULL; size_t count = 0; uint64_t expiry;
    ASSERT_EQ(MT_ERR_LIMIT, mt_dns_openwrt_parse(blob.head, blob_raw_len(blob.head), &clients, &count, &expiry));
    ASSERT(clients == NULL); ASSERT_EQ(0u, count); blob_buf_free(&blob);
    ASSERT_EQ(0, blob_buf_init(&blob, 0));
    v4 = blobmsg_open_array(&blob, "dhcp_leases"); blobmsg_close_array(&blob, v4);
    ASSERT_EQ(MT_ERR_PROTO, mt_dns_openwrt_parse(blob.head, blob_raw_len(blob.head), &clients, &count, &expiry));
    blob_buf_free(&blob);
    PASS();
}

/* The fake provider is a separate process: it uses the real libubus wire and
 * subscription APIs without sharing libubus's internal buffers with the worker. */
static unsigned server_revision;
static bool server_expiry, server_hold;
static struct ubus_request_data deferred;
static int server_report;
static void report(char c) { if (write(server_report, &c, 1) != 1) { _exit(20); } }
static int snapshot(struct ubus_context *ctx, struct ubus_object *obj,
                    struct ubus_request_data *req, const char *method, struct blob_attr *message)
{
    (void)obj; (void)method; (void)message;
    if (server_hold) { ubus_defer_request(ctx, req, &deferred); report('h'); return 0; }
    struct blob_buf blob = {0};
    if (blob_buf_init(&blob, 0)) { return UBUS_STATUS_NO_MEMORY; }
    void *v4 = blobmsg_open_array(&blob, "dhcp_leases");
    char name[32]; (void)snprintf(name, sizeof(name), "Device %u", server_revision);
    lease(&blob, name, "192.168.1.2", server_expiry ? 1 : 600, false);
    blobmsg_close_array(&blob, v4);
    void *v6 = blobmsg_open_array(&blob, "dhcp6_leases");
    lease(&blob, name, "fd00::2", server_expiry ? 1 : 600, false);
    blobmsg_close_array(&blob, v6);
    int status = ubus_send_reply(ctx, req, blob.head);
    blob_buf_free(&blob); report('q');
    return status;
}
static int fake_server(const char *socket_path)
{
    server_report = 4;
    struct ubus_context *ctx = ubus_connect(socket_path);
    if (!ctx) { return 10; }
    /* libubus initializes uloop, but this test producer drives poll itself. */
    signal(SIGTERM, SIG_DFL);
    const struct ubus_method methods[] = {UBUS_METHOD_NOARG("getDHCPLeases", snapshot)};
    struct ubus_object_type inventory_type = UBUS_OBJECT_TYPE("test-inventory", methods);
    struct ubus_object inventory = {.name = "luci-rpc", .type = &inventory_type,
                                    .methods = methods, .n_methods = 1};
    struct ubus_object_type dns_type = UBUS_OBJECT_TYPE("test-dnsmasq", methods);
    struct ubus_object dns = {.name = "dnsmasq.lan", .type = &dns_type, .methods = methods, .n_methods = 1};
    int added = ubus_add_object(ctx, &inventory);
    if (!added) { added = ubus_add_object(ctx, &dns); }
    if (added) { fprintf(stderr, "fake provider registration: %s\n", ubus_strerror(added)); ubus_free(ctx); return 11; }
    report('r');
    bool done = false;
    while (!done) {
        struct pollfd fds[] = {{.fd = 3, .events = POLLIN}, {.fd = ctx->sock.fd, .events = POLLIN}};
        int ready = poll(fds, 2, 1000);
        if (ready < 0 && errno == EINTR) { continue; }
        if (ready < 0 || (fds[1].revents & (POLLHUP | POLLERR))) { break; }
        if (fds[1].revents & POLLIN) { ubus_handle_event(ctx); }
        if (fds[0].revents & POLLIN) {
            char command;
            if (read(3, &command, 1) != 1) { break; }
            struct blob_buf event = {0}; blob_buf_init(&event, 0);
            if (command == 'e' || command == 'b') {
                server_revision++;
                for (unsigned i = 0; i < (command == 'b' ? 10u : 1u); i++) { ubus_notify(ctx, &dns, "dhcp.ack", event.head, -1); }
            } else if (command == 'c') {
                server_revision++; blobmsg_add_string(&event, "package", "dhcp");
                ubus_send_event(ctx, "config.change", event.head);
            } else if (command == 'x') {
                server_revision++; server_expiry = true;
                ubus_notify(ctx, &dns, "dhcp.ack", event.head, -1);
            } else if (command == 'h' || command == 'f') {
                server_hold = command == 'h'; server_revision++; server_expiry = false;
                ubus_notify(ctx, &dns, "dhcp.ack", event.head, -1);
            } else if (command == 'o') {
                ubus_remove_object(ctx, &dns); server_revision++;
                dns.id = 0; dns_type.id = 0;
                ubus_add_object(ctx, &dns);
            } else if (command == 's') { done = true; }
            blob_buf_free(&event);
        }
    }
    ubus_free(ctx);
    return 0;
}

typedef struct {
    pthread_mutex_t mutex;
    atomic_bool stop;
    bool active, requested;
    uint64_t epoch;
    unsigned publications, failures;
    mt_dns_client_t *clients;
    size_t count;
    int control[2];
    const char *path;
} fixture_t;
static mt_dns_source_command_t worker_command(void *ud)
{
    fixture_t *f = ud;
    pthread_mutex_lock(&f->mutex);
    mt_dns_source_command_t result = {.active = f->active, .refresh = f->requested, .epoch = f->epoch};
    f->requested = false;
    pthread_mutex_unlock(&f->mutex);
    return result;
}
static void worker_publish(void *ud, uint64_t epoch, mt_dns_client_t *clients, size_t count, mt_err_t error)
{
    fixture_t *f = ud;
    pthread_mutex_lock(&f->mutex);
    if (epoch == f->epoch) {
        free(f->clients); f->clients = clients; f->count = count;
        f->publications++; if (error != MT_OK) { f->failures++; }
    } else { free(clients); }
    pthread_mutex_unlock(&f->mutex);
}
static void *run_worker(void *ud)
{
    fixture_t *f = ud;
    mt_dns_openwrt_worker_t worker = {.control_fd = f->control[0], .stop = &f->stop,
        .ud = f, .command = worker_command, .publish = worker_publish, .socket_path = f->path};
    mt_dns_openwrt_run(&worker);
    return NULL;
}
static void pause_ms(unsigned ms)
{
    struct timespec delay = {.tv_sec = (time_t)(ms / 1000u), .tv_nsec = (long)(ms % 1000u) * 1000000L};
    while (nanosleep(&delay, &delay) && errno == EINTR) {}
}
static bool wait_name(fixture_t *f, const char *name, unsigned timeout)
{
    for (unsigned elapsed = 0; elapsed < timeout; elapsed += 20) {
        pthread_mutex_lock(&f->mutex);
        bool found = f->count == 2 && !strcmp(f->clients[0].name, name) && !strcmp(f->clients[1].name, name);
        pthread_mutex_unlock(&f->mutex);
        if (found) { return true; } pause_ms(20);
    }
    return false;
}
static bool send_command(int fd, char c) { return write(fd, &c, 1) == 1; }
static bool wait_report(int fd, char expected)
{
    for (unsigned i = 0; i < 20; i++) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        if (poll(&p, 1, 100) <= 0) { continue; }
        char c; if (read(fd, &c, 1) == 1 && c == expected) { return true; }
    }
    return false;
}
static bool start_broker(const char *binary, const char *path, pid_t *pid)
{
    char directory[256];
    if (strlen(path) >= sizeof(directory)) { return false; }
    strcpy(directory, path);
    char *slash = strrchr(directory, '/'); if (!slash) { return false; } *slash = 0;
    char acl_path[300]; (void)snprintf(acl_path, sizeof(acl_path), "%s/test.json", directory);
    int pair[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) { return false; }
    struct ucred credentials; socklen_t size = sizeof(credentials);
    int got = getsockopt(pair[0], SOL_SOCKET, SO_PEERCRED, &credentials, &size);
    close(pair[0]); close(pair[1]); if (got) { return false; }
    struct passwd *user = getpwuid(credentials.uid); if (!user) { return false; }
    FILE *acl = fopen(acl_path, "w"); if (!acl) { return false; }
    int written = fprintf(acl, "{\"user\":\"%s\",\"publish\":[\"*\"],\"subscribe\":[\"*\"],"
        "\"access\":{\"*\":{\"methods\":[\"*\"]}},\"listen\":[\"*\"],\"send\":[\"*\"]}\n", user->pw_name);
    bool secured = fchmod(fileno(acl), 0600) == 0;
    if (fclose(acl) || written < 0 || !secured) { return false; }
    /* ubusd requires root-owned ACL files. Run under root or fakeroot; the
     * ACL applies solely to this test's private broker and temporary directory. */
    char *args[] = {(char *)binary, "-A", directory, "-s", (char *)path, NULL};
    if (posix_spawn(pid, binary, NULL, NULL, args, environ)) { return false; }
    struct stat st;
    for (unsigned i = 0; i < 100; i++) { if (!stat(path, &st)) { return true; } pause_ms(10); }
    return false;
}
static bool start_provider(const char *path, pid_t *pid, int *command_fd, int *report_fd)
{
    int command[2], response[2];
    if (pipe2(command, O_CLOEXEC) || pipe2(response, O_CLOEXEC)) { return false; }
    char self[4096]; ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) { return false; } self[n] = 0;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, command[0], 3);
    posix_spawn_file_actions_adddup2(&actions, response[1], 4);
    char *args[] = {self, "--fake-openwrt", (char *)path, NULL};
    int error = posix_spawn(pid, self, &actions, NULL, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(command[0]); close(response[1]);
    *command_fd = command[1]; *report_fd = response[0];
    return !error && wait_report(*report_fd, 'r');
}
TEST real_ubusd_notifications_restarts_config_changes_and_expiry(void)
{
    const char *broker = getenv("MT_TEST_UBUSD");
    if (!broker) { SKIPm("set MT_TEST_UBUSD to the native ubusd binary"); }
    char directory[] = "/tmp/mt-ubus-XXXXXX";
    bool ok = true, thread_started = false, directory_created = false;
    pid_t broker_pid = -1, server_pid = -1; int command_fd = -1, report_fd = -1;
    char path[256] = {0};
    fixture_t f = {.mutex = PTHREAD_MUTEX_INITIALIZER, .active = true, .epoch = 1, .path = path, .control = {-1, -1}};
    atomic_init(&f.stop, false);
    pthread_t thread;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "ubus integration step failed at %d: %s\n", __LINE__, #expr); ok = false; goto cleanup; } } while (0)
#define CHECK_ZERO(expr) CHECK((expr) == 0)
    CHECK(mkdtemp(directory)); directory_created = true;
    (void)snprintf(path, sizeof(path), "%s/bus.sock", directory);
    CHECK(start_broker(broker, path, &broker_pid));
    CHECK(start_provider(path, &server_pid, &command_fd, &report_fd));
    CHECK_ZERO( pipe2(f.control, O_CLOEXEC | O_NONBLOCK));
    CHECK_ZERO( pthread_create(&thread, NULL, run_worker, &f)); thread_started = true;
    CHECK(wait_name(&f, "Device 0", 4000)); CHECK(wait_report(report_fd, 'q'));
    CHECK(send_command(command_fd, 'b')); CHECK(wait_name(&f, "Device 1", 4000));
    CHECK(wait_report(report_fd, 'q'));
    struct pollfd quiet = {.fd = report_fd, .events = POLLIN};
    CHECK_ZERO( poll(&quiet, 1, 1300)); /* Ten notifications produce one snapshot. */
    CHECK(send_command(command_fd, 'c')); CHECK(wait_name(&f, "Device 2", 4000));
    CHECK(send_command(command_fd, 'o')); CHECK(wait_name(&f, "Device 3", 4000));
    CHECK(send_command(command_fd, 'e')); CHECK(wait_name(&f, "Device 4", 4000));
    /* A full broker restart loses every object ID. Recover and resubscribe. */
    CHECK_ZERO( kill(server_pid, SIGTERM)); CHECK(waitpid(server_pid, NULL, 0) == server_pid); server_pid = -1;
    close(command_fd); close(report_fd); command_fd = -1; report_fd = -1;
    CHECK_ZERO( kill(broker_pid, SIGTERM)); CHECK(waitpid(broker_pid, NULL, 0) == broker_pid); broker_pid = -1;
    (void)unlink(path);
    CHECK(start_broker(broker, path, &broker_pid));
    CHECK(start_provider(path, &server_pid, &command_fd, &report_fd));
    CHECK(wait_name(&f, "Device 0", 6000));
    CHECK(send_command(command_fd, 'e')); CHECK(wait_name(&f, "Device 1", 4000));
    CHECK(send_command(command_fd, 'x')); CHECK(wait_name(&f, "Device 2", 4000));
    CHECK(wait_report(report_fd, 'q'));
    /* Drain previous reports, then await an expiry-triggered query. */
    while (poll(&(struct pollfd){.fd = report_fd, .events = POLLIN}, 1, 0) > 0) {
        char c; if (read(report_fd, &c, 1) != 1) { break; }
    }
    CHECK(wait_report(report_fd, 'q'));
    pthread_mutex_lock(&f.mutex); f.active = false; f.epoch++; pthread_mutex_unlock(&f.mutex);
    CHECK(send_command(f.control[1], 'w')); pause_ms(200);
    CHECK(send_command(command_fd, 'e')); pause_ms(200);
    pthread_mutex_lock(&f.mutex); unsigned before = f.publications; pthread_mutex_unlock(&f.mutex);
    pause_ms(1200);
    pthread_mutex_lock(&f.mutex); unsigned after = f.publications; f.active = true; f.epoch++; pthread_mutex_unlock(&f.mutex);
    CHECK(before == after); CHECK(send_command(f.control[1], 'w'));
    CHECK(wait_name(&f, "Device 3", 4000));
    pthread_mutex_lock(&f.mutex); unsigned failures_before = f.failures; pthread_mutex_unlock(&f.mutex);
    CHECK(send_command(command_fd, 'h')); CHECK(wait_report(report_fd, 'h'));
    bool timeout_observed = false;
    for (unsigned elapsed = 0; elapsed < 3000; elapsed += 20) {
        pthread_mutex_lock(&f.mutex);
        timeout_observed = f.failures > failures_before && f.count == 0;
        pthread_mutex_unlock(&f.mutex);
        if (timeout_observed) { break; } pause_ms(20);
    }
    CHECK(timeout_observed);
    CHECK(send_command(command_fd, 'f')); CHECK(wait_name(&f, "Device 5", 4000));
    CHECK(send_command(command_fd, 'h')); CHECK(wait_report(report_fd, 'h'));
    struct timespec before_stop, after_stop;
    CHECK_ZERO(clock_gettime(CLOCK_MONOTONIC, &before_stop));
    atomic_store(&f.stop, true); CHECK(send_command(f.control[1], 'w'));
    CHECK_ZERO(pthread_join(thread, NULL)); thread_started = false;
    CHECK_ZERO(clock_gettime(CLOCK_MONOTONIC, &after_stop));
    int64_t stopped_ms = (int64_t)(after_stop.tv_sec - before_stop.tv_sec) * 1000 +
                         (after_stop.tv_nsec - before_stop.tv_nsec) / 1000000;
    CHECK(stopped_ms < 1500); /* Abort a pending RPC without its two-second deadline. */
cleanup:
    if (!ok && broker_pid > 0) { kill(broker_pid, SIGTERM); waitpid(broker_pid, NULL, 0); broker_pid = -1; }
    atomic_store(&f.stop, true);
    if (f.control[1] >= 0) { bool sent = send_command(f.control[1], 'w'); (void)sent; }
    if (thread_started) { pthread_join(thread, NULL); }
    free(f.clients); pthread_mutex_destroy(&f.mutex);
    if (f.control[0] >= 0) { close(f.control[0]); }
    if (f.control[1] >= 0) { close(f.control[1]); }
    if (server_pid > 0) { kill(server_pid, SIGTERM); waitpid(server_pid, NULL, 0); }
    if (broker_pid > 0) { kill(broker_pid, SIGTERM); waitpid(broker_pid, NULL, 0); }
    if (command_fd >= 0) { close(command_fd); }
    if (report_fd >= 0) { close(report_fd); }
    if (directory_created) {
        char acl_path[300]; (void)snprintf(acl_path, sizeof(acl_path), "%s/test.json", directory);
        unlink(acl_path); unlink(path); rmdir(directory);
    }
#undef CHECK_ZERO
#undef CHECK
    if (!ok) { FAIL(); }
    PASS();
}

#endif
GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
#ifdef MT_PLATFORM_OPENWRT
    if (argc == 3 && !strcmp(argv[1], "--fake-openwrt")) { return fake_server(argv[2]); }
#endif
    GREATEST_MAIN_BEGIN();
#ifdef MT_PLATFORM_OPENWRT
    RUN_TEST(native_snapshot_parses_both_families_expiry_conflicts_and_prefixes);
    RUN_TEST(malformed_and_oversized_inventory_never_publishes_partial_names);
    RUN_TEST(real_ubusd_notifications_restarts_config_changes_and_expiry);
#endif
    GREATEST_MAIN_END();
}
