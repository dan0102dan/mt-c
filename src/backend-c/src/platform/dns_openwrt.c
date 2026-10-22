/* Use the platform's host inventory and event loop. All libubus/uloop calls
 * belong to this worker; both libraries have process-wide internal state. */
#define _GNU_SOURCE
#include "magitrickle/dns_openwrt.h"
#ifdef MT_PLATFORM_OPENWRT
#include "../interfaces/dns_clients_internal.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libubus.h>
#include <libubox/blobmsg.h>
#include <libubox/uloop.h>

static mt_err_t add_addresses(struct blob_attr *array, int family,
    const char *name, const char *owner, mt_dns_client_table_t *table)
{
    if (!array) { return MT_OK; }
    if (blobmsg_type(array) != BLOBMSG_TYPE_ARRAY ||
        blobmsg_check_array(array, BLOBMSG_TYPE_STRING) < 0) { return MT_ERR_PROTO; }
    struct blob_attr *address;
    size_t remaining;
    blobmsg_for_each_attr(address, array, remaining) {
        unsigned char binary[16];
        const char *text = blobmsg_get_string(address);
        /* Host hints contain addresses, never delegated prefixes. The owner
         * is retained even when no name is known, to detect ambiguous IPs. */
        if (inet_pton(family, text, binary) != 1) { continue; }
        mt_err_t err = mt_dns_client_table_add_owned(table, text, name, owner, NULL);
        if (err != MT_OK) { return err; }
    }
    return remaining ? MT_ERR_PROTO : MT_OK;
}

mt_err_t mt_dns_openwrt_parse(const struct blob_attr *reply, size_t len,
    mt_dns_client_t **clients, size_t *count)
{
    if (!clients || !count) { return MT_ERR_INVAL; }
    *clients = NULL; *count = 0;
    if (len > MT_DNS_CLIENT_BODY_LIMIT) { return MT_ERR_LIMIT; }
    if (!reply || len < sizeof(*reply) || blob_raw_len(reply) != len ||
        blob_is_extended(reply)) { return MT_ERR_PROTO; }
    enum { NAME, IPV4, IPV6, FIELDS };
    const struct blobmsg_policy policy[FIELDS] = {
        [NAME] = {.name = "name", .type = BLOBMSG_TYPE_UNSPEC},
        [IPV4] = {.name = "ipaddrs", .type = BLOBMSG_TYPE_UNSPEC},
        [IPV6] = {.name = "ip6addrs", .type = BLOBMSG_TYPE_UNSPEC},
    };
    mt_dns_client_table_t table = {0};
    mt_err_t err = MT_OK;
    struct blob_attr *row;
    size_t remaining, rows = 0;
    blobmsg_for_each_attr(row, reply, remaining) {
        if (++rows > MT_DNS_CLIENT_LIMIT) { err = MT_ERR_LIMIT; break; }
        if (!blobmsg_check_attr_len(row, true, remaining) ||
            blobmsg_type(row) != BLOBMSG_TYPE_TABLE) { err = MT_ERR_PROTO; break; }
        char owner[18];
        if (!mt_dns_client_mac(blobmsg_name(row), owner)) { continue; }
        struct blob_attr *field;
        size_t fields_remaining;
        blobmsg_for_each_attr(field, row, fields_remaining) {
            if (!blobmsg_check_attr_len(field, true, fields_remaining)) { err = MT_ERR_PROTO; break; }
        }
        if (err != MT_OK || fields_remaining) { err = MT_ERR_PROTO; break; }
        struct blob_attr *fields[FIELDS] = {0};
        if (blobmsg_data_len(row) && blobmsg_parse(policy, FIELDS, fields,
            blobmsg_data(row), (unsigned int)blobmsg_data_len(row))) { err = MT_ERR_PROTO; break; }
        if (fields[NAME] && blobmsg_type(fields[NAME]) != BLOBMSG_TYPE_STRING) { err = MT_ERR_PROTO; break; }
        const char *name = fields[NAME] ? blobmsg_get_string(fields[NAME]) : NULL;
        err = add_addresses(fields[IPV4], AF_INET, name, owner, &table);
        if (err == MT_OK) { err = add_addresses(fields[IPV6], AF_INET6, name, owner, &table); }
        if (err != MT_OK) { break; }
    }
    if (err == MT_OK && remaining) { err = MT_ERR_PROTO; }
    if (err != MT_OK) { free(table.items); return err; }
    mt_dns_client_table_finish(&table, clients, count);
    return MT_OK;
}

typedef struct {
    const mt_dns_openwrt_worker_t *worker;
    struct ubus_auto_conn connection;
    struct ubus_subscriber subscriber;
    struct ubus_request request;
    struct uloop_fd control;
    struct uloop_timeout refresh, deadline;
    void (*native_connection_lost)(struct ubus_context *ctx);
    uint64_t epoch, request_epoch;
    bool active, initialized, subscribed, pending, dirty, received;
    mt_dns_client_t *clients;
    size_t count;
    mt_err_t error;
} source_t;

static void schedule_refresh(source_t *s, int delay)
{
    if (!s->active || atomic_load(s->worker->stop)) { return; }
    s->dirty = true;
    if (!s->pending && (!s->refresh.pending || uloop_timeout_remaining64(&s->refresh) > delay)) {
        uloop_timeout_set(&s->refresh, delay);
    }
}

static void publish(source_t *s, uint64_t epoch, mt_err_t error)
{
    if (error != MT_OK || !s->active || atomic_load(s->worker->stop)) {
        free(s->clients); s->clients = NULL; s->count = 0;
    }
    if (s->active && !atomic_load(s->worker->stop)) {
        s->worker->publish(s->worker->ud, epoch, s->clients, s->count, error);
    }
    s->clients = NULL; s->count = 0;
}

static void cancel_request(source_t *s)
{
    uloop_timeout_cancel(&s->deadline);
    if (s->pending) { ubus_abort_request(&s->connection.ctx, &s->request); }
    s->pending = false;
    free(s->clients); s->clients = NULL; s->count = 0;
}

static void inventory_received(struct ubus_request *request, int type, struct blob_attr *message)
{
    (void)type;
    source_t *s = request->priv;
    if (s->received || !message) { s->error = MT_ERR_PROTO; return; }
    s->received = true;
    s->error = mt_dns_openwrt_parse(message, blob_raw_len(message), &s->clients, &s->count);
}

static void inventory_done(struct ubus_request *request, int status)
{
    source_t *s = request->priv;
    uloop_timeout_cancel(&s->deadline);
    s->pending = false;
    mt_err_t error = status || !s->received ? MT_ERR_UPSTREAM : s->error;
    publish(s, s->request_epoch, error);
    schedule_refresh(s, s->dirty ? 1000 : error == MT_OK ? (int)MT_DNS_CLIENT_REFRESH_MS : 5000);
}

static void inventory_timeout(struct uloop_timeout *timer)
{
    source_t *s = container_of(timer, source_t, deadline);
    cancel_request(s);
    publish(s, s->request_epoch, MT_ERR_TIMEOUT);
    schedule_refresh(s, s->dirty ? 1000 : 5000);
}

static bool new_object(struct ubus_context *ctx, struct ubus_subscriber *subscriber, const char *path)
{
    (void)ctx;
    source_t *s = container_of(subscriber, source_t, subscriber);
    if (strcmp(path, "service") && strcmp(path, "dhcp") && strcmp(path, "luci-rpc") &&
        strcmp(path, "dnsmasq") && strncmp(path, "dnsmasq.", 8)) { return false; }
    schedule_refresh(s, 1000);
    /* Subscribing to luci-rpc also reports its removal. libubus restores these
     * subscriptions when providers or the broker return with new object IDs. */
    return true;
}

static void removed(struct ubus_context *ctx, struct ubus_subscriber *subscriber, uint32_t id)
{
    (void)ctx; (void)id;
    schedule_refresh(container_of(subscriber, source_t, subscriber), 1000);
}

static int notification(struct ubus_context *ctx, struct ubus_object *obj,
    struct ubus_request_data *request, const char *method, struct blob_attr *message)
{
    (void)ctx; (void)request;
    struct ubus_subscriber *subscriber = container_of(obj, struct ubus_subscriber, obj);
    source_t *s = container_of(subscriber, source_t, subscriber);
    if (!strncmp(method, "dhcp.", 5)) { schedule_refresh(s, 1000); }
    if (!strcmp(method, "config.change") && message) {
        const struct blobmsg_policy policy = {.name = "config", .type = BLOBMSG_TYPE_STRING};
        struct blob_attr *config = NULL;
        if (!blobmsg_parse(&policy, 1, &config, blob_data(message), (unsigned int)blob_len(message)) &&
            config && !strcmp(blobmsg_get_string(config), "dhcp")) { schedule_refresh(s, 1000); }
    }
    return 0;
}

static void connection_lost(struct ubus_context *ctx)
{
    struct ubus_auto_conn *connection = container_of(ctx, struct ubus_auto_conn, ctx);
    source_t *s = container_of(connection, source_t, connection);
    cancel_request(s);
    publish(s, s->epoch, MT_ERR_UPSTREAM);
    s->native_connection_lost(ctx);
    schedule_refresh(s, 5000);
}

static void connected(struct ubus_context *ctx)
{
    struct ubus_auto_conn *connection = container_of(ctx, struct ubus_auto_conn, ctx);
    source_t *s = container_of(connection, source_t, connection);
    s->initialized = true;
    s->native_connection_lost = ctx->connection_lost;
    ctx->connection_lost = connection_lost;
    schedule_refresh(s, 0);
}

static void fetch_inventory(struct uloop_timeout *timer)
{
    source_t *s = container_of(timer, source_t, refresh);
    if (!s->active || atomic_load(s->worker->stop)) { return; }
    struct ubus_context *ctx = &s->connection.ctx;
    mt_err_t error = MT_ERR_UPSTREAM;
    if (!s->initialized || !ctx->sock.registered || ctx->sock.eof) { goto fail; }
    if (!s->subscribed) {
        if (ubus_register_subscriber(ctx, &s->subscriber)) { goto fail; }
        s->subscribed = true;
    }
    uint32_t id;
    if (ubus_lookup_id(ctx, "luci-rpc", &id)) { goto fail; }
    struct blob_buf args = {0};
    if (blob_buf_init(&args, 0)) { error = MT_ERR_NOMEM; goto fail; }
    int status = ubus_invoke_async(ctx, id, "getHostHints", args.head, &s->request);
    blob_buf_free(&args);
    if (status) { goto fail; }
    /* These timers bound an outstanding RPC, not libubus' synchronous
     * connection/lookup operations before it. DNS forwarding stays separate. */
    uloop_timeout_cancel(&s->refresh);
    s->dirty = false; s->received = false; s->error = MT_OK;
    s->pending = true; s->request_epoch = s->epoch;
    s->request.priv = s; s->request.data_cb = inventory_received;
    s->request.complete_cb = inventory_done;
    ubus_complete_request_async(ctx, &s->request);
    uloop_timeout_set(&s->deadline, 2000);
    return;
fail:
    publish(s, s->epoch, error);
    schedule_refresh(s, 5000);
}

static void disconnect_source(source_t *s)
{
    uloop_timeout_cancel(&s->refresh);
    uloop_timeout_cancel(&s->connection.timer);
    cancel_request(s);
    if (s->initialized) { ubus_shutdown(&s->connection.ctx); }
    memset(&s->connection, 0, sizeof(s->connection));
    memset(&s->subscriber, 0, sizeof(s->subscriber));
    s->initialized = false; s->subscribed = false; s->dirty = false;
}

static void process_commands(source_t *s)
{
    if (atomic_load(s->worker->stop)) { uloop_end(); return; }
    mt_dns_source_command_t command = s->worker->command(s->worker->ud);
    if (command.epoch != s->epoch || command.active != s->active) {
        disconnect_source(s);
        s->active = command.active; s->epoch = command.epoch;
        if (s->active) {
            s->subscriber.cb = notification; s->subscriber.remove_cb = removed;
            s->subscriber.new_obj_cb = new_object;
            s->connection.path = s->worker->socket_path;
            s->connection.cb = connected;
            ubus_auto_connect(&s->connection);
            if (!s->initialized) { publish(s, s->epoch, MT_ERR_UPSTREAM); }
        }
    }
    if (command.refresh) { schedule_refresh(s, 0); }
}

static void control_received(struct uloop_fd *fd, unsigned int events)
{
    (void)events;
    source_t *s = container_of(fd, source_t, control);
    char bytes[128];
    ssize_t size;
    do { size = read(fd->fd, bytes, sizeof(bytes)); } while (size > 0 || (size < 0 && errno == EINTR));
    if (size == 0 || (size < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { uloop_end(); return; }
    process_commands(s);
}

void mt_dns_openwrt_run(const mt_dns_openwrt_worker_t *worker)
{
    source_t source = {.worker = worker, .control = {.fd = worker->control_fd, .cb = control_received},
        .refresh = {.cb = fetch_inventory}, .deadline = {.cb = inventory_timeout}};
    /* The main application owns child processes such as iptables. uloop's
     * default SIGCHLD handler would otherwise reap them with waitpid(-1). */
    bool handle_sigchld = uloop_handle_sigchld;
    uloop_handle_sigchld = false;
    if (uloop_init()) { uloop_handle_sigchld = handle_sigchld; return; }
    if (!uloop_fd_add(&source.control, ULOOP_READ)) {
        process_commands(&source);
        if (!atomic_load(worker->stop)) { uloop_run(); }
    }
    disconnect_source(&source);
    uloop_fd_delete(&source.control);
    uloop_done();
    uloop_handle_sigchld = handle_sigchld;
}
#endif
