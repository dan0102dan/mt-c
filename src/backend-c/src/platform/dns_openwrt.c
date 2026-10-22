/* Native ubus client. All library calls run on the metadata worker: libubus
 * uses shared internal buffers and must not be driven from two threads. */
#define _GNU_SOURCE
#include "magitrickle/dns_openwrt.h"
#ifdef MT_PLATFORM_OPENWRT
#include "../interfaces/dns_clients_internal.h"
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <libubus.h>
#include <libubox/blobmsg.h>

static uint64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) { return 0; }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
static mt_err_t add_address(mt_dns_client_table_t *table, const char *text, const char *name, int family)
{
    char address[MT_DNS_CLIENT_IP_LEN];
    size_t len = strnlen(text, sizeof(address));
    if (len >= sizeof(address)) { return MT_OK; }
    memcpy(address, text, len + 1);
    char *prefix = strchr(address, '/');
    if (prefix) {
        if (family != AF_INET6 || strcmp(prefix, "/128")) { return MT_OK; }
        *prefix = 0;
    }
    unsigned char binary[16];
    if (inet_pton(family, address, binary) != 1) { return MT_OK; }
    return mt_dns_client_table_add(table, address, name);
}
static mt_err_t parse_family(struct blob_attr *array, int family, mt_dns_client_table_t *table,
                              uint64_t *expires_in_ms)
{
    enum { NAME, EXPIRES, ADDRESS, ADDRESSES, FIELDS };
    const struct blobmsg_policy policy[FIELDS] = {
        [NAME] = {.name = "hostname", .type = BLOBMSG_TYPE_STRING},
        [EXPIRES] = {.name = "expires", .type = BLOBMSG_TYPE_UNSPEC},
        [ADDRESS] = {.name = family == AF_INET ? "ipaddr" : "ip6addr", .type = BLOBMSG_TYPE_STRING},
        [ADDRESSES] = {.name = "ip6addrs", .type = BLOBMSG_TYPE_ARRAY},
    };
    if (blobmsg_check_array(array, BLOBMSG_TYPE_TABLE) < 0) { return MT_ERR_PROTO; }
    struct blob_attr *row; size_t remaining;
    size_t rows = 0;
    blobmsg_for_each_attr(row, array, remaining) {
        if (++rows > MT_DNS_CLIENT_LIMIT) { return MT_ERR_LIMIT; }
        struct blob_attr *fields[FIELDS];
        if (blobmsg_parse(policy, FIELDS, fields, blobmsg_data(row), (unsigned int)blobmsg_data_len(row))) { return MT_ERR_PROTO; }
        if (!fields[NAME] || !fields[EXPIRES]) { continue; }
        char name[MT_DNS_CLIENT_NAME_LEN];
        if (!mt_dns_client_label(blobmsg_get_string(fields[NAME]), name)) { continue; }
        int type = blobmsg_type(fields[EXPIRES]);
        uint32_t lifetime;
        if (type == BLOBMSG_TYPE_INT8 && blobmsg_get_u8(fields[EXPIRES]) == 0) { lifetime = 0; }
        else if (type == BLOBMSG_TYPE_INT32) {
            lifetime = blobmsg_get_u32(fields[EXPIRES]);
            if (!lifetime || lifetime > INT32_MAX) { continue; }
        } else { continue; }
        if (lifetime && (uint64_t)lifetime * 1000u < *expires_in_ms) { *expires_in_ms = (uint64_t)lifetime * 1000u; }
        mt_err_t err = MT_OK;
        if (family == AF_INET6 && fields[ADDRESSES]) {
            if (blobmsg_check_array(fields[ADDRESSES], BLOBMSG_TYPE_STRING) < 0) { return MT_ERR_PROTO; }
            struct blob_attr *address; size_t left;
            blobmsg_for_each_attr(address, fields[ADDRESSES], left) {
                err = add_address(table, blobmsg_get_string(address), name, family);
                if (err != MT_OK) { return err; }
            }
        } else if (fields[ADDRESS]) {
            err = add_address(table, blobmsg_get_string(fields[ADDRESS]), name, family);
        }
        if (err != MT_OK) { return err; }
    }
    return MT_OK;
}
mt_err_t mt_dns_openwrt_parse(const struct blob_attr *reply, size_t len,
    mt_dns_client_t **clients, size_t *count, uint64_t *expires_in_ms)
{
    if (!clients || !count || !expires_in_ms) { return MT_ERR_INVAL; }
    *clients = NULL; *count = 0; *expires_in_ms = MT_DNS_CLIENT_REFRESH_MS;
    if (len > MT_DNS_CLIENT_BODY_LIMIT) { return MT_ERR_LIMIT; }
    if (!reply || len < sizeof(*reply) || blob_raw_len(reply) != len ||
        blob_is_extended(reply)) { return MT_ERR_PROTO; }
    const struct blobmsg_policy policy[] = {
        {.name = "dhcp_leases", .type = BLOBMSG_TYPE_ARRAY},
        {.name = "dhcp6_leases", .type = BLOBMSG_TYPE_ARRAY},
    };
    struct blob_attr *arrays[2];
    if (blobmsg_parse(policy, 2, arrays, blob_data(reply), (unsigned int)blob_len(reply)) || !arrays[0] || !arrays[1]) { return MT_ERR_PROTO; }
    mt_dns_client_table_t table = {0};
    mt_err_t err = parse_family(arrays[0], AF_INET, &table, expires_in_ms);
    if (err == MT_OK) { err = parse_family(arrays[1], AF_INET6, &table, expires_in_ms); }
    if (err != MT_OK) { free(table.items); return err; }
    mt_dns_client_table_finish(&table, clients, count);
    return MT_OK;
}

typedef struct source source_t;
typedef struct { struct ubus_event_handler handler; source_t *source; } event_t;
struct source {
    const mt_dns_openwrt_worker_t *worker;
    struct ubus_context bus;
    struct ubus_subscriber subscriber;
    event_t objects, config;
    bool connected, lost, dirty;
    uint64_t due, serial;
};
static void invalidate(source_t *s)
{
    s->serial++;
    if (!s->dirty) { s->due = now_ms() + 1000u; }
    s->dirty = true;
}
static bool relevant_object(const char *path)
{
    return !strcmp(path, "dhcp") || !strcmp(path, "luci-rpc") ||
           !strcmp(path, "dnsmasq") || !strncmp(path, "dnsmasq.", 8);
}
static bool new_object(struct ubus_context *ctx, struct ubus_subscriber *subscriber, const char *path)
{
    (void)ctx;
    source_t *s = (source_t *)((char *)subscriber - offsetof(source_t, subscriber));
    if (!relevant_object(path)) { return false; }
    invalidate(s);
    /* luci-rpc is inventory only; dnsmasq/odhcpd emit object notifications. */
    return strcmp(path, "luci-rpc") != 0;
}
static int notification(struct ubus_context *ctx, struct ubus_object *obj,
    struct ubus_request_data *request, const char *method, struct blob_attr *message)
{
    (void)ctx; (void)request; (void)message;
    struct ubus_subscriber *subscriber = (struct ubus_subscriber *)((char *)obj - offsetof(struct ubus_subscriber, obj));
    source_t *s = (source_t *)((char *)subscriber - offsetof(source_t, subscriber));
    if (!strncmp(method, "dhcp.", 5)) { invalidate(s); }
    return 0;
}
static void removed(struct ubus_context *ctx, struct ubus_subscriber *subscriber, uint32_t id)
{
    (void)ctx; (void)id;
    source_t *s = (source_t *)((char *)subscriber - offsetof(source_t, subscriber));
    invalidate(s);
}
static void event_received(struct ubus_context *ctx, struct ubus_event_handler *handler,
                            const char *type, struct blob_attr *message)
{
    (void)ctx;
    event_t *event = (event_t *)((char *)handler - offsetof(event_t, handler));
    const struct blobmsg_policy policy[] = {{.name = "path", .type = BLOBMSG_TYPE_STRING},
                                           {.name = "package", .type = BLOBMSG_TYPE_STRING}};
    struct blob_attr *fields[2];
    if (!message || blobmsg_parse(policy, 2, fields, blob_data(message), (unsigned int)blob_len(message))) { return; }
    if ((!strncmp(type, "ubus.object.", 12) && fields[0] && relevant_object(blobmsg_get_string(fields[0]))) ||
        (!strcmp(type, "config.change") && fields[1] && !strcmp(blobmsg_get_string(fields[1]), "dhcp"))) {
        invalidate(event->source);
    }
}
static void connection_lost(struct ubus_context *ctx)
{
    source_t *s = (source_t *)((char *)ctx - offsetof(source_t, bus));
    s->lost = true;
}
static void disconnect_source(source_t *s)
{
    if (s->connected) { ubus_shutdown(&s->bus); uloop_done(); }
    s->connected = false; s->lost = false;
    memset(&s->subscriber, 0, sizeof(s->subscriber));
    memset(&s->objects, 0, sizeof(s->objects));
    memset(&s->config, 0, sizeof(s->config));
}
static mt_err_t connect_source(source_t *s)
{
    if (ubus_connect_ctx(&s->bus, s->worker->socket_path)) { uloop_done(); return MT_ERR_UPSTREAM; }
    s->connected = true; s->bus.connection_lost = connection_lost;
    s->subscriber.cb = notification; s->subscriber.remove_cb = removed;
    s->subscriber.new_obj_cb = new_object;
    s->objects.source = s; s->objects.handler.cb = event_received;
    s->config.source = s; s->config.handler.cb = event_received;
    if (ubus_register_subscriber(&s->bus, &s->subscriber) ||
        ubus_register_event_handler(&s->bus, &s->objects.handler, "ubus.object.*") ||
        ubus_register_event_handler(&s->bus, &s->config.handler, "config.change")) {
        disconnect_source(s); return MT_ERR_UPSTREAM;
    }
    return MT_OK;
}
typedef struct {
    mt_dns_client_t *clients;
    size_t count;
    uint64_t expires_in_ms;
    mt_err_t error;
    bool received, done;
    int status;
} reply_t;
static void inventory_received(struct ubus_request *request, int type, struct blob_attr *message)
{
    (void)type;
    reply_t *reply = request->priv;
    if (reply->received || !message) { reply->error = MT_ERR_PROTO; return; }
    reply->received = true;
    reply->error = mt_dns_openwrt_parse(message, blob_raw_len(message),
        &reply->clients, &reply->count, &reply->expires_in_ms);
}
static void inventory_done(struct ubus_request *request, int status)
{
    reply_t *reply = request->priv;
    reply->done = true; reply->status = status;
}
static mt_err_t fetch_inventory(source_t *s, reply_t *reply)
{
    uint32_t id;
    if (ubus_lookup_id(&s->bus, "luci-rpc", &id)) { return MT_ERR_UPSTREAM; }
    struct ubus_request request;
    struct blob_buf args = {0};
    if (blob_buf_init(&args, 0)) { return MT_ERR_NOMEM; }
    int error = ubus_invoke_async(&s->bus, id, "getDHCPLeases", args.head, &request);
    blob_buf_free(&args);
    if (error) { return MT_ERR_UPSTREAM; }
    request.priv = reply; request.data_cb = inventory_received; request.complete_cb = inventory_done;
    ubus_complete_request_async(&s->bus, &request);
    uint64_t deadline = now_ms() + 2000u;
    while (!reply->done && !s->lost && !atomic_load(s->worker->stop)) {
        uint64_t now = now_ms();
        if (now >= deadline) { break; }
        struct pollfd fd = {.fd = s->bus.sock.fd, .events = POLLIN};
        int ready = poll(&fd, 1, (int)(deadline - now > 50 ? 50 : deadline - now));
        if (ready < 0 && errno == EINTR) { continue; }
        if (ready < 0 || (fd.revents & (POLLHUP | POLLERR | POLLNVAL))) { s->lost = true; break; }
        if (ready) { ubus_handle_event(&s->bus); }
    }
    if (!reply->done) { ubus_abort_request(&s->bus, &request); }
    if (atomic_load(s->worker->stop)) { return MT_ERR_CANCELED; }
    if (s->lost) { return MT_ERR_UPSTREAM; }
    if (!reply->done) { return MT_ERR_TIMEOUT; }
    if (reply->status || !reply->received) { return MT_ERR_UPSTREAM; }
    return reply->error;
}
void mt_dns_openwrt_run(const mt_dns_openwrt_worker_t *worker)
{
    source_t source = {.worker = worker};
    uint64_t epoch = 0, reconcile_at = 0, reconnect_at = 0;
    unsigned backoff_ms = 1000;
    bool active = false;
    while (!atomic_load(worker->stop)) {
        mt_dns_source_command_t command = worker->command(worker->ud);
        uint64_t now = now_ms();
        if (command.epoch != epoch || command.active != active) {
            disconnect_source(&source);
            epoch = command.epoch; active = command.active;
            source.dirty = active; source.due = now;
            reconnect_at = now; reconcile_at = now; backoff_ms = 1000;
        }
        if (command.refresh && active) { source.dirty = true; source.due = now; }
        if (active && source.lost) {
            disconnect_source(&source);
            worker->publish(worker->ud, epoch, NULL, 0, MT_ERR_UPSTREAM);
            reconnect_at = now + backoff_ms;
        }
        if (active && !source.connected && now >= reconnect_at) {
            mt_err_t error = connect_source(&source);
            if (error != MT_OK) {
                worker->publish(worker->ud, epoch, NULL, 0, error);
                reconnect_at = now_ms() + backoff_ms;
                if (backoff_ms < 30000) { backoff_ms *= 2; if (backoff_ms > 30000) { backoff_ms = 30000; } }
            } else {
                backoff_ms = 1000; source.dirty = true; source.due = now_ms();
            }
        }
        now = now_ms();
        if (active && source.connected && (now >= reconcile_at || (source.dirty && now >= source.due))) {
            uint64_t serial = source.serial;
            source.dirty = false;
            reply_t reply = {.expires_in_ms = MT_DNS_CLIENT_REFRESH_MS};
            mt_err_t error = fetch_inventory(&source, &reply);
            if (error != MT_OK) { free(reply.clients); reply.clients = NULL; reply.count = 0; }
            worker->publish(worker->ud, epoch, reply.clients, reply.count, error);
            uint64_t delay = error == MT_OK ? reply.expires_in_ms : 5000u;
            if (!delay) { delay = 1000; }
            reconcile_at = now_ms() + delay;
            if (source.serial != serial) { source.dirty = true; source.due = now_ms() + 1000u; }
        }
        struct pollfd fds[2] = {{.fd = worker->control_fd, .events = POLLIN},
                                {.fd = active && source.connected ? source.bus.sock.fd : -1, .events = POLLIN}};
        int timeout = -1;
        if (active) {
            uint64_t deadline = source.connected ? reconcile_at : reconnect_at;
            if (source.connected && source.dirty && source.due < deadline) { deadline = source.due; }
            now = now_ms();
            timeout = deadline <= now ? 0 : deadline - now > INT_MAX ? INT_MAX : (int)(deadline - now);
        }
        int ready = poll(fds, 2, timeout);
        if (ready < 0 && errno == EINTR) { continue; }
        if (ready < 0) { break; }
        if (fds[0].revents) { char bytes[128]; while (read(worker->control_fd, bytes, sizeof(bytes)) > 0) {} }
        if (fds[1].revents & POLLIN) { ubus_handle_event(&source.bus); }
        if (fds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) { source.lost = true; }
    }
    disconnect_source(&source);
}
#endif
