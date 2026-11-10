/* Production daemon lifecycle with real TCP/UDP sockets and HTTP requests.
 * Only privileged kernel/discovery transports and the restricted UNIX bind
 * are substituted. Settings preparation, restart, persistence and rollback
 * are the same functions as the shipped daemon. */
#include "greatest.h"
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/time.h>
#include <unistd.h>

#include "fake_iptables.h"
#include "fake_ipset_nl.h"

/* Keep persisted fixtures reloadable in unversioned native builds (D-36). */
#undef MT_VERSION
#define MT_VERSION "0.99.0"

int mt_settings_test_main(int argc, char **argv);
#define main mt_settings_test_main
#include "../../src/main/main.c"
#undef main

static atomic_int fail_rename, fail_rtnl, fail_iptables, fail_snapshot, starts;
static atomic_uint admin_port;
static atomic_bool hold_resolver, resolver_entered, hold_send;
static atomic_uint held_sends;
static atomic_int held_fd;
static mt_fake_ipset_nl_t *current_ipset;

/* Address discovery is a netlink-backed transport too. Keep the loopback
 * fixture deterministic while exercising the real interception compiler. */
int __wrap_getifaddrs(struct ifaddrs **out);
void __wrap_freeifaddrs(struct ifaddrs *list);
int __wrap_getifaddrs(struct ifaddrs **out)
{
    struct addresses {
        struct ifaddrs entries[2];
        struct sockaddr_in ipv4;
        struct sockaddr_in6 ipv6;
    } *addresses = calloc(1, sizeof(*addresses));
    *out = NULL;
    if (!addresses) { errno = ENOMEM; return -1; }
    addresses->ipv4.sin_family = AF_INET;
    addresses->ipv4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addresses->ipv6.sin6_family = AF_INET6;
    addresses->ipv6.sin6_addr = in6addr_loopback;
    addresses->entries[0].ifa_name = addresses->entries[1].ifa_name = "lo";
    addresses->entries[0].ifa_addr = (struct sockaddr *)&addresses->ipv4;
    addresses->entries[1].ifa_addr = (struct sockaddr *)&addresses->ipv6;
    addresses->entries[0].ifa_next = &addresses->entries[1];
    *out = addresses->entries;
    return 0;
}
void __wrap_freeifaddrs(struct ifaddrs *list) { free(list); }

static void pause_ms(long ms)
{
    struct timespec delay = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000};
    nanosleep(&delay, NULL);
}

mt_rtnl_t *__wrap_mt_rtnl_open(void);
void __wrap_mt_rtnl_close(mt_rtnl_t *rtnl);
mt_err_t __wrap_mt_rtnl_alloc_mark_table(mt_rtnl_t *rtnl, uint32_t start, uint32_t *out);
mt_err_t __wrap_mt_rtnl_reserve_table(mt_rtnl_t *rtnl, uint32_t table);
void __wrap_mt_rtnl_release_table(mt_rtnl_t *rtnl, uint32_t table);
mt_rtnl_t *__wrap_mt_rtnl_open(void)
{
    atomic_fetch_add(&starts, 1);
    if (atomic_load(&fail_rtnl) > 0) { atomic_fetch_sub(&fail_rtnl, 1); return NULL; }
    return (mt_rtnl_t *)calloc(1, 1);
}
void __wrap_mt_rtnl_close(mt_rtnl_t *rtnl) { free(rtnl); }
mt_err_t __wrap_mt_rtnl_alloc_mark_table(mt_rtnl_t *rtnl, uint32_t start, uint32_t *out) { (void)rtnl; *out = start; return MT_OK; }
mt_err_t __wrap_mt_rtnl_reserve_table(mt_rtnl_t *rtnl, uint32_t table) { (void)rtnl; (void)table; return MT_OK; }
void __wrap_mt_rtnl_release_table(mt_rtnl_t *rtnl, uint32_t table) { (void)rtnl; (void)table; }
mt_err_t __wrap_mt_rtnl_get_default_route(mt_rtnl_t *, int, uint32_t, uint32_t, mt_rtnl_default_route_t *);
mt_err_t __wrap_mt_rtnl_get_default_route(mt_rtnl_t *rtnl, int family, uint32_t table, uint32_t priority, mt_rtnl_default_route_t *out)
{ (void)rtnl; (void)family; (void)table; (void)priority; memset(out, 0, sizeof(*out)); return MT_OK; }

mt_err_t __wrap_mt_nl_watcher_create_with_routes(mt_loop_t *, mt_nl_link_cb, void *, mt_nl_addr_cb,
    void *, mt_nl_change_cb, void *, mt_nl_change_cb, void *, mt_nl_watcher_t **);
mt_err_t __wrap_mt_nl_watcher_create_with_routes(mt_loop_t *loop, mt_nl_link_cb link, void *link_ud,
    mt_nl_addr_cb addr, void *addr_ud, mt_nl_change_cb route, void *route_ud,
    mt_nl_change_cb resync, void *resync_ud, mt_nl_watcher_t **out)
{
    (void)loop; (void)link; (void)link_ud; (void)addr; (void)addr_ud;
    (void)route; (void)route_ud; (void)resync; (void)resync_ud; *out = NULL; return MT_OK;
}

mt_err_t __wrap_mt_httpd_listen_unix(mt_httpd_t *http, const char *path);
mt_err_t __wrap_mt_httpd_listen_unix(mt_httpd_t *http, const char *path)
{
    (void)path;
    mt_err_t err = mt_httpd_listen_tcp(http, "127.0.0.1", 0);
    if (err == MT_OK) { atomic_store(&admin_port, mt_httpd_tcp_port(http)); }
    return err;
}

mt_ipt_executable_t *__wrap_mt_ipt_executable_real_new(mt_ipt_proto_t proto);
mt_ipt_executable_t *__wrap_mt_ipt_executable_real_new(mt_ipt_proto_t proto)
{
    if (atomic_exchange(&fail_iptables, 0)) { return NULL; }
    mt_fake_ipt_t *fake = mt_fake_ipt_new(proto);
    if (fake) {
        (void)mt_fake_ipt_set_initial_rules(fake, "filter", "FORWARD", NULL, NULL, 0);
        (void)mt_fake_ipt_set_initial_rules(fake, "mangle", "PREROUTING", NULL, NULL, 0);
        (void)mt_fake_ipt_set_initial_rules(fake, "nat", "PREROUTING", NULL, NULL, 0);
        (void)mt_fake_ipt_set_initial_rules(fake, "nat", "POSTROUTING", NULL, NULL, 0);
    }
    return fake ? mt_fake_ipt_as_executable(fake) : NULL;
}

mt_ruleset_snapshot_t *__real_mt_ruleset_snapshot_build(const mt_config_t *);
mt_ruleset_snapshot_t *__wrap_mt_ruleset_snapshot_build(const mt_config_t *);
mt_ruleset_snapshot_t *__wrap_mt_ruleset_snapshot_build(const mt_config_t *cfg)
{
    return atomic_exchange(&fail_snapshot, 0) ? NULL : __real_mt_ruleset_snapshot_build(cfg);
}
mt_ipset_nl_t *__wrap_mt_ipset_nl_real_new(void);
mt_ipset_nl_t *__wrap_mt_ipset_nl_real_new(void)
{
    current_ipset = mt_fake_ipset_nl_new();
    return current_ipset ? mt_fake_ipset_nl_as_transport(current_ipset) : NULL;
}

int __real_rename(const char *from, const char *to);
int __wrap_rename(const char *from, const char *to);
int __wrap_rename(const char *from, const char *to)
{
    if (atomic_exchange(&fail_rename, 0)) { errno = EIO; return -1; }
    return __real_rename(from, to);
}

mt_err_t __real_mt_dnsproxy_resolve_upstream(const char *, uint16_t, mt_dns_upstream_t *);
mt_err_t __wrap_mt_dnsproxy_resolve_upstream(const char *, uint16_t, mt_dns_upstream_t *);
mt_err_t __wrap_mt_dnsproxy_resolve_upstream(const char *address, uint16_t port, mt_dns_upstream_t *out)
{
    atomic_store(&resolver_entered, true);
    while (atomic_load(&hold_resolver)) { pause_ms(1); }
    if (!strcmp(address, "unresolvable.invalid")) { return MT_ERR_INVAL; }
    return __real_mt_dnsproxy_resolve_upstream(address, port, out);
}

ssize_t __real_send(int fd, const void *data, size_t size, int flags);
ssize_t __wrap_send(int fd, const void *data, size_t size, int flags);
ssize_t __wrap_send(int fd, const void *data, size_t size, int flags)
{
    if ((flags & MSG_NOSIGNAL) && atomic_load(&hold_send)) {
        if (atomic_load(&held_fd) < 0 && size >= 32 && !memcmp(data, "HTTP/1.1 ", 9)) {
            atomic_store(&held_fd, fd);
            return __real_send(fd, data, 32, flags); /* a real partial ACK */
        }
        if (fd == atomic_load(&held_fd)) { atomic_fetch_add(&held_sends, 1); errno = EAGAIN; return -1; }
    }
    return __real_send(fd, data, size, flags);
}

static int bound_socket(int type, uint16_t *port)
{
    int fd = socket(AF_INET, type, 0);
    if (fd < 0) { return -1; }
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t length = sizeof(address);
    if (bind(fd, (struct sockaddr *)&address, length) || getsockname(fd, (struct sockaddr *)&address, &length)) { close(fd); return -1; }
    *port = ntohs(address.sin_port); return fd;
}
static uint16_t unused_port(void)
{
    uint16_t port = 0;
    int fd = bound_socket(SOCK_STREAM, &port);
    if (fd >= 0) { close(fd); }
    return port;
}

typedef struct dns_stub {
    int udp, tcp;
    uint16_t port;
    uint8_t answer;
    atomic_bool stopping;
    pthread_t thread;
} dns_stub_t;

static size_t dns_answer(uint8_t *message, size_t length, uint8_t answer)
{
    if (length < 17 || length + 28 > 512) { return 0; }
    uint16_t type = (uint16_t)((message[length - 4] << 8) | message[length - 3]);
    message[2] = 0x81; message[3] = 0x80; message[6] = 0; message[7] = 1;
    uint8_t rr[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4};
    rr[3] = (uint8_t)type; rr[11] = type == 28 ? 16 : 4;
    memcpy(message + length, rr, sizeof(rr)); length += sizeof(rr);
    if (type == 28) { memset(message + length, 0, 16); message[length] = 0x20; message[length + 1] = 1; message[length + 15] = answer; length += 16; }
    else { message[length++] = 192; message[length++] = 0; message[length++] = 2; message[length++] = answer; }
    return length;
}

static void *stub_loop(void *userdata)
{
    dns_stub_t *s = userdata;
    while (!atomic_load(&s->stopping)) {
        struct pollfd fds[] = {{s->udp, POLLIN, 0}, {s->tcp, POLLIN, 0}};
        if (poll(fds, 2, 20) <= 0) { continue; }
        uint8_t message[514];
        if (fds[0].revents & POLLIN) {
            struct sockaddr_storage peer; socklen_t size = sizeof(peer);
            ssize_t length = recvfrom(s->udp, message, 512, 0, (struct sockaddr *)&peer, &size);
            if (length > 0) {
                size_t response = dns_answer(message, (size_t)length, s->answer);
                (void)sendto(s->udp, message, response, 0, (struct sockaddr *)&peer, size);
            }
        }
        if (fds[1].revents & POLLIN) {
            int fd = accept(s->tcp, NULL, NULL);
            if (fd < 0) { continue; }
            struct timeval timeout = {1, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            if (recv(fd, message, 2, MSG_WAITALL) == 2) {
                size_t length = (size_t)((message[0] << 8) | message[1]);
                if (length <= 512 && recv(fd, message + 2, length, MSG_WAITALL) == (ssize_t)length) {
                    length = dns_answer(message + 2, length, s->answer);
                    message[0] = (uint8_t)(length >> 8); message[1] = (uint8_t)length;
                    (void)send(fd, message, length + 2, 0);
                }
            }
            close(fd);
        }
    }
    return NULL;
}

static void stub_close_sockets(dns_stub_t *s)
{
    int saved_errno = errno;
    if (s->udp >= 0) { close(s->udp); s->udp = -1; }
    if (s->tcp >= 0) { close(s->tcp); s->tcp = -1; }
    errno = saved_errno;
}

static bool stub_start_at(dns_stub_t *s, uint8_t answer, uint16_t first_port)
{
    memset(s, 0, sizeof(*s)); s->udp = s->tcp = -1; s->answer = answer;
    atomic_init(&s->stopping, false);
    /* UDP port 0 can select a number occupied by TCP (including TIME_WAIT).
     * Retry that collision only, releasing both sockets before another try. */
    for (unsigned attempt = 0; attempt < 32; attempt++) {
        s->udp = socket(AF_INET, SOCK_DGRAM, 0);
        if (s->udp < 0) { goto fail; }
        struct sockaddr_in address = {.sin_family = AF_INET,
            .sin_port = htons(attempt == 0 ? first_port : 0),
            .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        if (bind(s->udp, (struct sockaddr *)&address, sizeof(address))) {
            if (errno == EADDRINUSE) { goto retry; }
            goto fail;
        }
        socklen_t length = sizeof(address);
        if (getsockname(s->udp, (struct sockaddr *)&address, &length)) { goto fail; }
        s->port = ntohs(address.sin_port);
        s->tcp = socket(AF_INET, SOCK_STREAM, 0);
        if (s->tcp < 0) { goto fail; }
        if (bind(s->tcp, (struct sockaddr *)&address, sizeof(address))) {
            if (errno == EADDRINUSE) { goto retry; }
            goto fail;
        }
        if (listen(s->tcp, 8)) { goto fail; }
        int thread_error = pthread_create(&s->thread, NULL, stub_loop, s);
        if (thread_error) { errno = thread_error; goto fail; }
        return true;
retry:
        stub_close_sockets(s);
    }
fail:
    stub_close_sockets(s);
    return false;
}
static bool stub_start(dns_stub_t *s, uint8_t answer) { return stub_start_at(s, answer, 0); }
static void stub_stop(dns_stub_t *s) { atomic_store(&s->stopping, true); pthread_join(s->thread, NULL); stub_close_sockets(s); }

static int dns_query(uint16_t port, bool tcp, bool aaaa, uint8_t *last)
{
    uint8_t question[] = {0x12, 0x34, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 7, 'e','x','a','m','p','l','e', 4,'t','e','s','t',0, 0,1,0,1};
    if (aaaa) { question[sizeof(question) - 3] = 28; }
    int fd = socket(AF_INET, tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    struct timeval timeout = {1, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (fd < 0 || connect(fd, (struct sockaddr *)&address, sizeof(address))) { if (fd >= 0) { close(fd); } return -1; }
    if (tcp) { uint8_t length[] = {0, sizeof(question)}; (void)send(fd, length, 2, 0); }
    (void)send(fd, question, sizeof(question), 0);
    uint8_t response[512]; ssize_t got;
    if (tcp) {
        uint8_t length[2];
        if (recv(fd, length, 2, MSG_WAITALL) != 2) { close(fd); return -1; }
        size_t size = (size_t)((length[0] << 8) | length[1]);
        got = size <= sizeof(response) ? recv(fd, response, size, MSG_WAITALL) : -1;
    } else { got = recv(fd, response, sizeof(response), 0); }
    close(fd);
    if (got < 12) { return -1; }
    if (last) { *last = response[got - 1]; }
    return (response[6] << 8) | response[7];
}

typedef struct harness {
    mt_config_t config;
    mt_settings_t *settings;
    mt_cache_t *cache;
    struct daemon daemon;
    char path[80];
    pthread_t thread;
    mt_err_t outcome;
} harness_t;

static void diagnostic(mt_http_req_t *req, mt_http_res_t *res, void *userdata)
{
    (void)req;
    harness_t *h = userdata;
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "cache_domains", (double)mt_cache_domain_count(h->cache));
    cJSON_AddNumberToObject(json, "set_entries", current_ipset ? (double)mt_fake_ipset_nl_entry_count(current_ipset, "mt_aabbccdd_4") : 0);
    cJSON_AddBoolToObject(json, "ipv4", h->daemon.ipt4 != NULL);
    cJSON_AddBoolToObject(json, "ipv6", h->daemon.ipt6 != NULL);
    cJSON_AddBoolToObject(json, "remap", h->daemon.port_remap != NULL);
    mt_ipt_rules_snapshot_t *rules = NULL;
    cJSON *targets = cJSON_AddArrayToObject(json, "remap_targets");
    if (h->daemon.ipt4 && mt_ipt_get_current_rules(h->daemon.ipt4, &rules) == MT_OK) {
        const mt_ipt_table_rules_t *nat = mt_ipt_rules_snapshot_find_table(rules, "nat");
        const mt_ipt_chain_rules_t *chain = nat ? mt_ipt_table_rules_find_chain(nat, "MT_DNSOR") : NULL;
        for (size_t i = 0; chain && i < chain->n_rules; i++) {
            const mt_ipt_rule_t *rule = chain->rules[i];
            for (size_t j = 0; j + 1 < rule->n_parts; j++) {
                if (!strcmp(rule->parts[j], "--to-destination")) {
                    cJSON_AddItemToArray(targets, cJSON_CreateString(rule->parts[j + 1]));
                }
            }
        }
    }
    mt_ipt_rules_snapshot_free(rules);
    mt_http_res_write_json(res, 200, json);
}
static void invalidate_runtime(mt_http_req_t *req, mt_http_res_t *res, void *userdata)
{
    (void)req;
    harness_t *h = userdata;
    mt_settings_runtime_stopped(h->settings);
    mt_http_res_write_json(res, 200, cJSON_CreateObject());
}
static void register_diagnostic(harness_t *h)
{
    if (h->daemon.http_tcp) { mt_httpd_route(h->daemon.http_tcp, "GET", "/test/state", diagnostic, h); }
    mt_httpd_route(h->daemon.http_unix, "GET", "/test/state", diagnostic, h);
    mt_httpd_route(h->daemon.http_unix, "POST", "/test/runtime/invalidate", invalidate_runtime, h);
}
static void *runtime_loop(void *userdata)
{
    harness_t *h = userdata;
    for (;;) {
        h->outcome = mt_loop_run(h->daemon.loop);
        if (h->outcome != MT_OK || !mt_settings_ready(h->settings)) { break; }
        h->outcome = daemon_apply_settings(&h->daemon, &h->config, h->path, h->settings, h->cache);
        if (h->outcome != MT_OK) { break; }
        register_diagnostic(h);
    }
    return NULL;
}

static harness_t *harness_start_ports(uint16_t upstream, bool enabled_group, bool ephemeral)
{
    atomic_store(&fail_rename, 0); atomic_store(&fail_rtnl, 0); atomic_store(&fail_iptables, 0); atomic_store(&fail_snapshot, 0);
    atomic_store(&hold_resolver, false); atomic_store(&hold_send, false); current_ipset = NULL;
    harness_t *h = calloc(1, sizeof(*h));
    if (!h || mt_config_init_defaults(&h->config) != MT_OK) { return NULL; }
    mt_app_config_t *app = &h->config.app;
    mt_strset(&app->http_web.host.address, "127.0.0.1"); app->http_web.host.port = ephemeral ? 0 : unused_port();
    mt_strset(&app->dns_proxy.host.address, "127.0.0.1"); app->dns_proxy.host.port = ephemeral ? 0 : unused_port();
    mt_strset(&app->dns_proxy.upstream.address, "127.0.0.1"); app->dns_proxy.upstream.port = upstream;
    app->dns_proxy.disable_remap53 = true; app->netfilter.disable_ipv4 = app->netfilter.disable_ipv6 = true;
    if (ephemeral) {
        app->dns_proxy.disable_remap53 = false;
        app->netfilter.disable_ipv4 = false;
        mt_strset(&app->link[0], "lo");
    }
    mt_strset(&app->log_level, "error");
    if (enabled_group) {
        const char *group = "configVersion: 0.9\ngroups: [{id: aabbccdd, name: Keep, enable: true, interface: blackhole, rules: [{id: 11223344, type: domain, rule: example.test, enable: true}]}]\n";
        if (mt_config_load_buffer(&h->config, group, strlen(group)) != MT_OK) { return NULL; }
    }
    snprintf(h->path, sizeof(h->path), "/tmp/mt-settings-test-XXXXXX");
    int fd = mkstemp(h->path); if (fd < 0) { return NULL; } close(fd);
    if (mt_config_save_file(&h->config, MT_VERSION, h->path) != MT_OK) { return NULL; }
    h->settings = mt_settings_create(&h->config); h->cache = mt_cache_create(0);
    if (!h->settings || !h->cache) { return NULL; }
    mt_err_t start = daemon_start(&h->daemon, &h->config, h->path, h->settings, h->cache, NULL, NULL);
    if (start != MT_OK) {
        fprintf(stderr, "settings harness startup: %s: %s\n", h->daemon.failure_stage, mt_err_str(start));
        return NULL;
    }
    register_diagnostic(h);
    if (pthread_create(&h->thread, NULL, runtime_loop, h)) { return NULL; }
    return h;
}
static harness_t *harness_start(uint16_t upstream, bool enabled_group) { return harness_start_ports(upstream, enabled_group, false); }
static void harness_stop(harness_t *h)
{
    atomic_store(&hold_resolver, false); atomic_store(&hold_send, false);
    mt_loop_stop(h->daemon.loop); pthread_join(h->thread, NULL);
    (void)daemon_teardown(&h->daemon);
    mt_settings_destroy(h->settings); mt_cache_destroy(h->cache); mt_config_clear(&h->config);
    unlink(h->path); free(h); current_ipset = NULL;
}

static int connection(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (fd < 0 || connect(fd, (struct sockaddr *)&address, sizeof(address))) { if (fd >= 0) { close(fd); } return -1; }
    struct timeval timeout = {2, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    return fd;
}
static int send_request(uint16_t port, const char *method, const char *path, const char *body)
{
    int fd = connection(port); if (fd < 0) { return -1; }
    char head[512];
    int size = snprintf(head, sizeof(head), "%s %s HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", method, path, body ? strlen(body) : 0);
    if (send(fd, head, (size_t)size, 0) != size || (body && send(fd, body, strlen(body), 0) != (ssize_t)strlen(body))) { close(fd); return -1; }
    return fd;
}
static int read_response(int fd, cJSON **json)
{
    *json = NULL; if (fd < 0) { return 0; }
    char response[16384]; size_t used = 0;
    while (used < sizeof(response) - 1) {
        ssize_t got = recv(fd, response + used, sizeof(response) - 1 - used, 0);
        if (got <= 0) { break; } used += (size_t)got;
    }
    close(fd); response[used] = '\0';
    int status = 0; (void)sscanf(response, "HTTP/1.1 %d", &status);
    char *body = strstr(response, "\r\n\r\n");
    if (body) { *json = cJSON_Parse(body + 4); }
    return status;
}
static int request(uint16_t port, const char *method, const char *path, const char *body, cJSON **json)
{
    return read_response(send_request(port, method, path, body), json);
}
static cJSON *item(const cJSON *json, const char *name) { return cJSON_GetObjectItemCaseSensitive(json, name); }
static const char *text_value(const cJSON *json, const char *name) { const cJSON *value = item(json, name); return cJSON_IsString(value) ? value->valuestring : ""; }
static char *payload(const cJSON *snapshot)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "revision", text_value(snapshot, "revision"));
    cJSON_AddItemToObject(body, "settings", cJSON_Duplicate(item(snapshot, "settings"), true));
    char *text = cJSON_PrintUnformatted(body); cJSON_Delete(body); return text;
}
static int submit(uint16_t port, const cJSON *snapshot, cJSON **ack)
{
    char *body = payload(snapshot);
    int status = request(port, "PUT", "/api/v1/system/settings", body, ack); free(body); return status;
}
static char *permission_payload(const cJSON *snapshot, const cJSON *permission)
{
    char *plain = payload(snapshot);
    cJSON *body = cJSON_Parse(plain); free(plain);
    cJSON_AddItemToObject(body, "allow_dns_restart", cJSON_Duplicate(permission, true));
    char *text = cJSON_PrintUnformatted(body); cJSON_Delete(body); return text;
}
static int submit_without_restart(uint16_t port, const cJSON *snapshot, cJSON **ack)
{
    cJSON *permission = cJSON_CreateFalse();
    char *body = permission_payload(snapshot, permission); cJSON_Delete(permission);
    int status = request(port, "PUT", "/api/v1/system/settings", body, ack); free(body); return status;
}
static int preview(uint16_t port, const cJSON *snapshot, cJSON **response)
{
    char *body = payload(snapshot);
    int status = request(port, "POST", "/api/v1/system/settings/preview", body, response); free(body); return status;
}
static void replace_setting(cJSON *snapshot, const char *path, const char *value)
{
    char buffer[96]; snprintf(buffer, sizeof(buffer), "%s", path);
    cJSON *parent = item(snapshot, "settings");
    char *save = NULL, *part = strtok_r(buffer, ".", &save);
    for (;;) {
        char *next = strtok_r(NULL, ".", &save);
        if (!next) { cJSON_ReplaceItemInObjectCaseSensitive(parent, part, cJSON_Parse(value)); return; }
        parent = item(parent, part); part = next;
    }
}
static cJSON *await_operation(uint16_t port, const char *id, const char *state)
{
    for (size_t i = 0; i < 300; i++) {
        cJSON *json = NULL;
        int status = request(port ? port : (uint16_t)atomic_load(&admin_port), "GET", "/api/v1/system/settings", NULL, &json);
        if (status == 200 && !strcmp(text_value(item(json, "operation"), "id"), id) &&
            !strcmp(text_value(item(json, "operation"), "state"), state)) { return json; }
        cJSON_Delete(json); pause_ms(10);
    }
    return NULL;
}
static char *read_file(const char *path)
{
    FILE *file = fopen(path, "rb"); if (!file) { return NULL; }
    if (fseek(file, 0, SEEK_END)) { fclose(file); return NULL; }
    long length = ftell(file); rewind(file);
    char *text = length >= 0 ? malloc((size_t)length + 1) : NULL;
    if (!text || fread(text, 1, (size_t)length, file) != (size_t)length) { free(text); fclose(file); return NULL; }
    text[length] = '\0'; fclose(file); return text;
}

TEST dns_stub_retries_cross_protocol_port_collision_without_leaking_sockets(void)
{
    dns_stub_t guard; ASSERT(stub_start(&guard, 1));
    uint16_t occupied_port = guard.port;
    int occupied_tcp = dup(guard.tcp);
    stub_stop(&guard); /* Keep TCP reserved while releasing its UDP counterpart. */
    ASSERT(occupied_tcp >= 0);
    dns_stub_t stub;
    bool started = stub_start_at(&stub, 12, occupied_port);
    if (!started) { close(occupied_tcp); ASSERT(started); }
    uint16_t chosen_port = stub.port;
    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in address = {.sin_family = AF_INET,
        .sin_port = htons(occupied_port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    int rebound = udp < 0 ? -1 : bind(udp, (struct sockaddr *)&address, sizeof(address));
    if (udp >= 0) { close(udp); }
    uint8_t udp_answer = 0, tcp_answer = 0;
    int udp_result = dns_query(stub.port, false, false, &udp_answer);
    int tcp_result = dns_query(stub.port, true, false, &tcp_answer);
    stub_stop(&stub); close(occupied_tcp);
    ASSERT(chosen_port != occupied_port);
    ASSERT_EQ(0, rebound); /* The failed attempt released its UDP socket. */
    ASSERT_EQ(1, udp_result); ASSERT_EQ(12, udp_answer);
    ASSERT_EQ(1, tcp_result); ASSERT_EQ(12, tcp_answer);
    PASS();
}

TEST projection_is_strict_and_preserves_hidden_settings(void)
{
    mt_config_t config; ASSERT_EQ(MT_OK, mt_config_init_defaults(&config));
    mt_app_config_t *app = &config.app;
    app->dns_proxy.max_concurrent = 0;
    app->dns_proxy.max_idle_conns = UINT64_C(0x100000002);
    app->dns_proxy.timeout = MT_DURATION_MS - 1;
    app->netfilter.ipset.additional_ttl = 3 * MT_DURATION_SEC + 99;
    ASSERT_EQ(MT_OK, mt_strset(&app->log_level, "legacy-unknown"));
    cJSON *json = mt_settings_json(app); ASSERT(json);
    ASSERT_EQ(100, item(item(json, "dnsProxy"), "maxConcurrent")->valueint);
    ASSERT_EQ(2, item(item(json, "dnsProxy"), "maxIdleConns")->valueint);
    ASSERT_EQ(5000, item(item(json, "dnsProxy"), "timeoutMs")->valueint);
    ASSERT_EQ(3, item(item(item(json, "netfilter"), "ipset"), "additionalTTLSeconds")->valueint);
    ASSERT_STR_EQ("info", text_value(json, "logLevel"));
    ASSERT(item(item(json, "httpWeb"), "skin") == NULL);
    ASSERT(item(item(json, "netfilter"), "iptables") == NULL);
    ASSERT(item(json, "keeneticPolicy") == NULL);
    char revision[65], after[65], field[96];
    ASSERT_EQ(MT_OK, mt_settings_revision(app, revision));
    ASSERT_EQ(MT_OK, mt_strset(&app->http_web.skin, "retained-skin"));
    ASSERT_EQ(MT_OK, mt_strset(&app->netfilter.iptables.chain_prefix, "retained_"));
    ASSERT_EQ(MT_OK, mt_strset(&app->netfilter.ipset.table_prefix, "sets_"));
    app->netfilter.start_mark_table_index = 1234;
    ASSERT_EQ(MT_OK, mt_settings_revision(app, after)); ASSERT_STR_EQ(revision, after);
    mt_app_config_t parsed;
    ASSERT_EQ(MT_OK, mt_settings_parse(json, app, &parsed, field));
    ASSERT_STR_EQ("retained-skin", parsed.http_web.skin);
    ASSERT_STR_EQ("retained_", parsed.netfilter.iptables.chain_prefix);
    ASSERT_STR_EQ("sets_", parsed.netfilter.ipset.table_prefix);
    ASSERT_EQ(1234, parsed.netfilter.start_mark_table_index);
    mt_app_config_clear(&parsed);
    cJSON_AddStringToObject(item(json, "httpWeb"), "skin", "forbidden");
    ASSERT_EQ(MT_ERR_INVAL, mt_settings_parse(json, app, &parsed, field));
    ASSERT_STR_EQ("httpWeb.skin", field); cJSON_DeleteItemFromObjectCaseSensitive(item(json, "httpWeb"), "skin");
    cJSON_AddBoolToObject(json, "showAllInterfaces", true);
    ASSERT_EQ(MT_ERR_INVAL, mt_settings_parse(json, app, &parsed, field));
    cJSON_Delete(json);
    const double invalid[] = {0, -1, 0.5, 4294967296.0};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        json = mt_settings_json(app);
        cJSON_SetNumberValue(item(item(json, "dnsProxy"), "timeoutMs"), invalid[i]);
        ASSERT_EQ(MT_ERR_INVAL, mt_settings_parse(json, app, &parsed, field));
        ASSERT_STR_EQ("dnsProxy.timeoutMs", field); cJSON_Delete(json);
    }
    json = mt_settings_json(app);
    cJSON_SetNumberValue(item(item(json, "dnsProxy"), "maxConcurrent"), UINT32_MAX);
    cJSON_SetNumberValue(item(item(json, "dnsProxy"), "maxIdleConns"), 0);
    cJSON_SetNumberValue(item(item(json, "dnsProxy"), "timeoutMs"), UINT32_MAX);
    cJSON_SetNumberValue(item(item(item(json, "netfilter"), "ipset"), "additionalTTLSeconds"), UINT32_MAX);
    ASSERT_EQ(MT_OK, mt_settings_parse(json, app, &parsed, field));
    ASSERT_EQ((mt_duration_t)UINT32_MAX * MT_DURATION_MS, parsed.dns_proxy.timeout);
    ASSERT_EQ((mt_duration_t)UINT32_MAX * MT_DURATION_SEC, parsed.netfilter.ipset.additional_ttl);
    mt_app_config_clear(&parsed); cJSON_Delete(json); mt_config_clear(&config); PASS();
}

TEST preview_classifies_actual_changes_without_mutating_or_resolving(void)
{
    dns_stub_t stub; ASSERT(stub_start(&stub, 12)); harness_t *h = harness_start(stub.port, false); ASSERT(h);
    uint16_t web = h->config.app.http_web.host.port;
    cJSON *snapshot = NULL, *response = NULL;
    ASSERT_EQ(200, request(web, "GET", "/api/v1/system/settings", NULL, &snapshot));
    char *before = read_file(h->path); ASSERT(before);
    int started = atomic_load(&starts);
    atomic_store(&resolver_entered, false); atomic_store(&hold_resolver, true);
    const struct { const char *path, *value; bool restart; } cases[] = {
        {"httpWeb.enabled", "false", true}, {"httpWeb.auth.enabled", "true", false},
        {"httpWeb.host.address", "\"0.0.0.0\"", true}, {"httpWeb.host.port", "1", true},
        {"dnsProxy.host.address", "\"0.0.0.0\"", true}, {"dnsProxy.host.port", "1", true},
        {"dnsProxy.upstream.address", "\"unresolvable.invalid\"", true}, {"dnsProxy.upstream.port", "1", true},
        {"dnsProxy.disableRemap53", "false", true}, {"dnsProxy.disableFakePTR", "true", false},
        {"dnsProxy.disableDropAAAA", "true", false}, {"dnsProxy.maxIdleConns", "20", true},
        {"dnsProxy.maxConcurrent", "50", true}, {"dnsProxy.timeoutMs", "6000", true},
        {"netfilter.disableIPv4", "false", true}, {"netfilter.disableIPv6", "false", true},
        {"netfilter.ipset.additionalTTLSeconds", "42", false}, {"link", "[]", true},
        {"showAllInterfaces", "true", false}, {"logLevel", "\"warn\"", false},
    };
    ASSERT_EQ(200, preview(web, snapshot, &response));
    ASSERT(cJSON_IsFalse(item(response, "dns_restart_required"))); cJSON_Delete(response);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        cJSON *changed = cJSON_Duplicate(snapshot, true);
        replace_setting(changed, cases[i].path, cases[i].value);
        ASSERT_EQ(200, preview(web, changed, &response));
        ASSERT_EQ(cases[i].restart, cJSON_IsTrue(item(response, "dns_restart_required")));
        cJSON_Delete(changed); cJSON_Delete(response);
    }
    cJSON *changed = cJSON_Duplicate(snapshot, true);
    replace_setting(changed, "showAllInterfaces", "true"); replace_setting(changed, "dnsProxy.timeoutMs", "6000");
    ASSERT_EQ(200, preview(web, changed, &response)); ASSERT(cJSON_IsTrue(item(response, "dns_restart_required"))); cJSON_Delete(response);
    ASSERT_EQ(409, submit_without_restart(web, changed, &response));
    ASSERT_STR_EQ("SETTINGS_RESTART_REQUIRED", text_value(response, "code")); cJSON_Delete(response);
    cJSON *permission = cJSON_CreateNumber(0);
    char *invalid = permission_payload(changed, permission); cJSON_Delete(permission);
    ASSERT_EQ(400, request(web, "PUT", "/api/v1/system/settings", invalid, &response)); free(invalid);
    ASSERT_STR_EQ("allow_dns_restart", text_value(response, "field")); cJSON_Delete(response);
    replace_setting(changed, "dnsProxy.timeoutMs", "5000");
    ASSERT_EQ(200, preview(web, changed, &response)); ASSERT(cJSON_IsFalse(item(response, "dns_restart_required"))); cJSON_Delete(response);
    replace_setting(changed, "showAllInterfaces", "false");
    ASSERT_EQ(200, preview(web, changed, &response)); ASSERT(cJSON_IsFalse(item(response, "dns_restart_required"))); cJSON_Delete(response);
    cJSON_Delete(changed);
    atomic_store(&hold_resolver, false);
    ASSERT(!atomic_load(&resolver_entered)); ASSERT_EQ(started, atomic_load(&starts));
    ASSERT_EQ(200, request(web, "GET", "/api/v1/system/settings", NULL, &response));
    ASSERT_STR_EQ(text_value(snapshot, "revision"), text_value(response, "revision")); ASSERT(cJSON_IsNull(item(response, "operation")));
    char *after = read_file(h->path); ASSERT(after); ASSERT_STR_EQ(before, after);
    free(before); free(after); cJSON_Delete(snapshot); cJSON_Delete(response); harness_stop(h); stub_stop(&stub); PASS();
}

TEST live_settings_preserve_dns_runtime_and_revoke_unauthenticated_streams(void)
{
    dns_stub_t stub; ASSERT(stub_start(&stub, 12)); harness_t *h = harness_start(stub.port, true); ASSERT(h);
    uint16_t web = h->daemon.ports.http, dns = h->daemon.ports.dns;
    mt_loop_t *loop = h->daemon.loop; mt_dnsproxy_t *proxy = h->daemon.proxy;
    mt_dns_pipeline_t *pipeline = h->daemon.pipeline; mt_httpd_t *http = h->daemon.http_tcp;
    const char *dns_address = h->config.app.dns_proxy.host.address, *chain = h->config.app.netfilter.iptables.chain_prefix;
    char **links = h->config.app.link;
    int started = atomic_load(&starts);
    ASSERT_EQ(1, dns_query(dns, false, false, NULL)); ASSERT_EQ(0, dns_query(dns, false, true, NULL));
    int stream = send_request(web, "GET", "/api/v1/system/dns/stream", NULL); ASSERT(stream >= 0);
    char stream_data[2048]; ssize_t received = recv(stream, stream_data, sizeof(stream_data) - 1, 0); ASSERT(received > 0);
    stream_data[received] = '\0'; ASSERT(strstr(stream_data, "HTTP/1.1 200"));
    cJSON *snapshot = NULL, *response = NULL, *ack = NULL;
    ASSERT_EQ(200, request(web, "GET", "/api/v1/system/settings", NULL, &snapshot));
    replace_setting(snapshot, "showAllInterfaces", "true"); replace_setting(snapshot, "logLevel", "\"warn\"");
    replace_setting(snapshot, "httpWeb.auth.enabled", "true"); replace_setting(snapshot, "dnsProxy.disableFakePTR", "true");
    replace_setting(snapshot, "dnsProxy.disableDropAAAA", "true"); replace_setting(snapshot, "netfilter.ipset.additionalTTLSeconds", "42");
    ASSERT_EQ(200, preview(web, snapshot, &response)); ASSERT(cJSON_IsFalse(item(response, "dns_restart_required"))); cJSON_Delete(response);
    atomic_store(&resolver_entered, false); atomic_store(&hold_resolver, true);
    ASSERT_EQ(202, submit_without_restart(web, snapshot, &ack));
    response = await_operation(0, text_value(item(ack, "operation"), "id"), "succeeded");
    atomic_store(&hold_resolver, false); ASSERT(response);
    ASSERT(!atomic_load(&resolver_entered)); ASSERT_EQ(started, atomic_load(&starts));
    ASSERT(h->daemon.loop == loop); ASSERT(h->daemon.proxy == proxy); ASSERT(h->daemon.pipeline == pipeline); ASSERT(h->daemon.http_tcp == http);
    ASSERT(h->config.app.dns_proxy.host.address == dns_address); ASSERT(h->config.app.netfilter.iptables.chain_prefix == chain); ASSERT(h->config.app.link == links);
    ASSERT_EQ(MT_LOG_WARN, mt_log_level()); ASSERT_STR_EQ(text_value(ack, "target_revision"), text_value(response, "revision"));
    ASSERT_EQ(1, dns_query(dns, false, false, NULL)); ASSERT_EQ(1, dns_query(dns, true, false, NULL)); ASSERT_EQ(1, dns_query(dns, false, true, NULL));
    cJSON *denied = NULL; ASSERT_EQ(401, request(web, "GET", "/api/v1/system/settings", NULL, &denied)); cJSON_Delete(denied);
    do { received = recv(stream, stream_data, sizeof(stream_data), 0); } while (received > 0);
    close(stream); ASSERT_EQ(0, received);
    mt_config_t persisted; ASSERT_EQ(MT_OK, mt_config_init_defaults(&persisted));
    ASSERT_EQ(MT_OK, mt_config_load_file(&persisted, h->path));
    ASSERT_EQ(web, persisted.app.http_web.host.port); ASSERT_EQ(dns, persisted.app.dns_proxy.host.port);
    ASSERT_EQ((size_t)1, mt_cache_domain_count(h->cache));
    ASSERT(persisted.app.show_all_interfaces); ASSERT(persisted.app.http_web.auth.enabled);
    ASSERT(persisted.app.dns_proxy.disable_fake_ptr); ASSERT(persisted.app.dns_proxy.disable_drop_aaaa);
    ASSERT_EQ(42 * MT_DURATION_SEC, persisted.app.netfilter.ipset.additional_ttl); ASSERT_STR_EQ("warn", persisted.app.log_level);
    mt_config_clear(&persisted);
    ASSERT_EQ(200, submit_without_restart((uint16_t)atomic_load(&admin_port), response, &denied));
    ASSERT_STR_EQ("succeeded", text_value(item(denied, "operation"), "state")); cJSON_Delete(denied);
    /* A second live operation also works on the same, never-stopped loop. */
    replace_setting(response, "httpWeb.auth.enabled", "false");
    cJSON_Delete(ack); ASSERT_EQ(202, submit_without_restart((uint16_t)atomic_load(&admin_port), response, &ack));
    cJSON *confirmed = await_operation(web, text_value(item(ack, "operation"), "id"), "succeeded"); ASSERT(confirmed);
    ASSERT_EQ(started, atomic_load(&starts)); ASSERT(h->daemon.loop == loop); ASSERT_EQ(1, dns_query(dns, true, false, NULL));
    cJSON_Delete(confirmed); cJSON_Delete(snapshot); cJSON_Delete(response); cJSON_Delete(ack); harness_stop(h); stub_stop(&stub); PASS();
}

TEST live_persistence_failure_preserves_memory_file_and_dns(void)
{
    dns_stub_t stub; ASSERT(stub_start(&stub, 12)); harness_t *h = harness_start(stub.port, false); ASSERT(h);
    uint16_t web = h->config.app.http_web.host.port, dns = h->config.app.dns_proxy.host.port;
    cJSON *snapshot = NULL, *ack = NULL;
    ASSERT_EQ(200, request(web, "GET", "/api/v1/system/settings", NULL, &snapshot));
    char *before = read_file(h->path); ASSERT(before); int started = atomic_load(&starts);
    replace_setting(snapshot, "showAllInterfaces", "true"); replace_setting(snapshot, "httpWeb.auth.enabled", "true");
    replace_setting(snapshot, "logLevel", "\"warn\""); replace_setting(snapshot, "dnsProxy.disableDropAAAA", "true");
    atomic_store(&fail_rename, 1);
    ASSERT_EQ(202, submit_without_restart(web, snapshot, &ack));
    cJSON *failed = await_operation(web, text_value(item(ack, "operation"), "id"), "failed"); ASSERT(failed);
    ASSERT_STR_EQ(text_value(snapshot, "revision"), text_value(failed, "revision"));
    ASSERT(!h->config.app.show_all_interfaces); ASSERT(!h->config.app.http_web.auth.enabled); ASSERT_EQ(MT_LOG_ERROR, mt_log_level());
    ASSERT_EQ(started, atomic_load(&starts)); ASSERT_EQ(1, dns_query(dns, false, false, NULL)); ASSERT_EQ(0, dns_query(dns, false, true, NULL));
    char *after = read_file(h->path); ASSERT(after); ASSERT_STR_EQ(before, after);
    free(before); free(after); cJSON_Delete(snapshot); cJSON_Delete(ack); cJSON_Delete(failed); harness_stop(h); stub_stop(&stub); PASS();
}

TEST live_acknowledgement_cannot_escalate_to_dns_restart(void)
{
    dns_stub_t stub; ASSERT(stub_start(&stub, 12)); harness_t *h = harness_start(stub.port, false); ASSERT(h);
    uint16_t web = h->config.app.http_web.host.port, dns = h->config.app.dns_proxy.host.port;
    cJSON *snapshot = NULL, *ack = NULL, *response = NULL;
    ASSERT_EQ(200, request(web, "GET", "/api/v1/system/settings", NULL, &snapshot));
    replace_setting(snapshot, "showAllInterfaces", "true");
    ASSERT_EQ(200, preview(web, snapshot, &response)); ASSERT(cJSON_IsFalse(item(response, "dns_restart_required"))); cJSON_Delete(response);
    char *before = read_file(h->path); ASSERT(before); int started = atomic_load(&starts);
    cJSON *permission = cJSON_CreateFalse(); char *body = permission_payload(snapshot, permission); cJSON_Delete(permission);
    atomic_store(&held_fd, -1); atomic_store(&hold_send, true); atomic_store(&held_sends, 0);
    int fd = send_request(web, "PUT", "/api/v1/system/settings", body); free(body); ASSERT(fd >= 0);
    for (size_t i = 0; !atomic_load(&held_sends) && i < 200; i++) { pause_ms(1); }
    ASSERT(atomic_load(&held_sends) > 0);
    ASSERT_EQ(200, request((uint16_t)atomic_load(&admin_port), "POST", "/test/runtime/invalidate", NULL, &response)); cJSON_Delete(response);
    atomic_store(&hold_send, false);
    ASSERT_EQ(202, read_response(fd, &ack));
    cJSON *failed = await_operation(web, text_value(item(ack, "operation"), "id"), "failed"); ASSERT(failed);
    ASSERT(strstr(text_value(item(failed, "operation"), "error"), "DNS restart"));
    ASSERT_EQ(started, atomic_load(&starts)); ASSERT_EQ(1, dns_query(dns, false, false, NULL)); ASSERT(!h->config.app.show_all_interfaces);
    char *after = read_file(h->path); ASSERT(after); ASSERT_STR_EQ(before, after);
    free(before); free(after); cJSON_Delete(snapshot); cJSON_Delete(ack); cJSON_Delete(failed); harness_stop(h); stub_stop(&stub); PASS();
}

TEST real_dns_upstream_switch_preserves_cache_and_repopulates_sets(void)
{
    dns_stub_t old, next; ASSERT(stub_start(&old, 10)); ASSERT(stub_start(&next, 20));
    harness_t *h = harness_start(old.port, true); ASSERT(h);
    uint16_t web = h->config.app.http_web.host.port, dns = h->config.app.dns_proxy.host.port;
    uint8_t answer = 0;
    ASSERT_EQ(1, dns_query(dns, false, false, &answer)); ASSERT_EQ(10, answer);
    ASSERT_EQ(1, dns_query(dns, true, false, &answer)); ASSERT_EQ(10, answer);
    ASSERT_EQ(0, dns_query(dns, false, true, NULL));
    cJSON *snapshot = NULL, *ack = NULL;
    ASSERT_EQ(200, request(web, "GET", "/api/v1/system/settings", NULL, &snapshot));
    cJSON *settings = item(snapshot, "settings");
    cJSON_SetNumberValue(item(item(item(settings, "dnsProxy"), "upstream"), "port"), next.port);
    cJSON_ReplaceItemInObjectCaseSensitive(item(settings, "dnsProxy"), "disableDropAAAA", cJSON_CreateTrue());
    cJSON_SetNumberValue(item(item(settings, "dnsProxy"), "timeoutMs"), 1234);
    cJSON_SetNumberValue(item(item(item(settings, "netfilter"), "ipset"), "additionalTTLSeconds"), 42);
    ASSERT_EQ(202, submit(web, snapshot, &ack)); ASSERT(ack);
    cJSON *confirmed = await_operation(web, text_value(item(ack, "operation"), "id"), "succeeded"); ASSERT(confirmed);
    ASSERT_STR_EQ(text_value(ack, "target_revision"), text_value(confirmed, "revision"));
    cJSON *diag = NULL; ASSERT_EQ(200, request(web, "GET", "/test/state", NULL, &diag));
    ASSERT_EQ(1, item(diag, "cache_domains")->valueint);
    ASSERT_EQ(1, item(diag, "set_entries")->valueint); cJSON_Delete(diag);
    ASSERT_EQ(1, dns_query(dns, false, false, &answer)); ASSERT_EQ(20, answer);
    ASSERT_EQ(1, dns_query(dns, true, false, &answer)); ASSERT_EQ(20, answer);
    ASSERT_EQ(1, dns_query(dns, false, true, NULL));
    mt_config_t persisted; ASSERT_EQ(MT_OK, mt_config_init_defaults(&persisted));
    ASSERT_EQ(MT_OK, mt_config_load_file(&persisted, h->path));
    ASSERT_EQ(next.port, persisted.app.dns_proxy.upstream.port);
    ASSERT_EQ(1234 * MT_DURATION_MS, persisted.app.dns_proxy.timeout);
    ASSERT_EQ(42 * MT_DURATION_SEC, persisted.app.netfilter.ipset.additional_ttl);
    ASSERT_EQ((size_t)1, persisted.n_groups); ASSERT_STR_EQ("Keep", persisted.groups[0]->name);
    mt_config_clear(&persisted); cJSON_Delete(snapshot); cJSON_Delete(ack); cJSON_Delete(confirmed);
    harness_stop(h); stub_stop(&old); stub_stop(&next); PASS();
}

TEST pending_resolution_keeps_dns_live_and_preserves_later_profile_saves(void)
{
    dns_stub_t stub; ASSERT(stub_start(&stub, 12));
    harness_t *h = harness_start(stub.port, false); ASSERT(h);
    uint16_t web = h->config.app.http_web.host.port, dns = h->config.app.dns_proxy.host.port;
    cJSON *snapshot = NULL, *ack = NULL, *response = NULL;
    ASSERT_EQ(200, request(web, "GET", "/api/v1/system/settings", NULL, &snapshot));
    cJSON_SetNumberValue(item(item(item(snapshot, "settings"), "dnsProxy"), "timeoutMs"), 6000);
    atomic_store(&hold_resolver, true); atomic_store(&resolver_entered, false);
    int before = atomic_load(&starts);
    ASSERT_EQ(202, submit(web, snapshot, &ack));
    ASSERT_EQ(1, dns_query(dns, false, false, NULL));
    ASSERT_EQ(before, atomic_load(&starts));
    ASSERT_EQ(409, submit(web, snapshot, &response));
    ASSERT_STR_EQ("SETTINGS_BUSY", text_value(response, "code")); cJSON_Delete(response);
    const char *profile = "{\"profiles\":[{\"id\":\"11223344\",\"name\":\"Saved during preparation\",\"interfaces\":[\"tun0\"],\"on_unavailable\":\"blackhole\"}]}";
    ASSERT_EQ(200, request(web, "PUT", "/api/v1/profiles", profile, &response)); cJSON_Delete(response);
    ASSERT_EQ(200, request(web, "GET", "/api/v1/system/settings", NULL, &response));
    ASSERT_STR_EQ(text_value(snapshot, "revision"), text_value(response, "revision")); cJSON_Delete(response);
    atomic_store(&hold_resolver, false);
    response = await_operation(web, text_value(item(ack, "operation"), "id"), "succeeded"); ASSERT(response);
    cJSON_Delete(response);
    ASSERT_EQ(200, request(web, "GET", "/api/v1/profiles", NULL, &response));
    ASSERT_STR_EQ("Saved during preparation", text_value(cJSON_GetArrayItem(item(response, "profiles"), 0), "name"));
    cJSON_Delete(response);
    mt_config_t persisted; ASSERT_EQ(MT_OK, mt_config_init_defaults(&persisted));
    ASSERT_EQ(MT_OK, mt_config_load_file(&persisted, h->path)); ASSERT_EQ((size_t)1, persisted.n_profiles);
    ASSERT_STR_EQ("Saved during preparation", persisted.profiles[0]->name); mt_config_clear(&persisted);
    cJSON_Delete(snapshot); cJSON_Delete(ack); harness_stop(h); stub_stop(&stub); PASS();
}

TEST stale_request_and_invalid_fields_do_not_change_file_or_runtime(void)
{
    dns_stub_t stub; ASSERT(stub_start(&stub, 12)); harness_t *h = harness_start(stub.port, false); ASSERT(h);
    uint16_t web = h->config.app.http_web.host.port;
    cJSON *snapshot = NULL, *response = NULL;
    ASSERT_EQ(200, request(web, "GET", "/api/v1/system/settings", NULL, &snapshot));
    char *before = read_file(h->path); ASSERT(before); int started = atomic_load(&starts);
    cJSON_SetNumberValue(item(item(item(item(snapshot, "settings"), "httpWeb"), "host"), "port"), 0);
    ASSERT_EQ(400, submit(web, snapshot, &response)); ASSERT_STR_EQ("httpWeb.host.port", text_value(response, "field")); cJSON_Delete(response);
    ASSERT_EQ(400, preview(web, snapshot, &response)); ASSERT_STR_EQ("httpWeb.host.port", text_value(response, "field")); cJSON_Delete(response);
    cJSON_SetNumberValue(item(item(item(item(snapshot, "settings"), "httpWeb"), "host"), "port"), web);
    cJSON_ReplaceItemInObjectCaseSensitive(snapshot, "revision", cJSON_CreateString("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"));
    ASSERT_EQ(409, submit(web, snapshot, &response)); ASSERT_STR_EQ("SETTINGS_CONFLICT", text_value(response, "code")); cJSON_Delete(response);
    ASSERT_EQ(409, preview(web, snapshot, &response)); ASSERT_STR_EQ("SETTINGS_CONFLICT", text_value(response, "code")); cJSON_Delete(response);
    char *after = read_file(h->path); ASSERT(after); ASSERT_STR_EQ(before, after); ASSERT_EQ(started, atomic_load(&starts));
    free(before); free(after); cJSON_Delete(snapshot); harness_stop(h); stub_stop(&stub); PASS();
}

TEST legacy_apply_invalidates_pending_settings_and_identical_retry_recreates_runtime(void)
{
    dns_stub_t stub; ASSERT(stub_start(&stub, 12)); harness_t *h = harness_start(stub.port, false); ASSERT(h);
    uint16_t web = h->config.app.http_web.host.port;
    cJSON *snapshot = NULL, *ack = NULL, *response = NULL;
    ASSERT_EQ(200, request(web, "GET", "/api/v1/system/settings", NULL, &snapshot));
    cJSON_SetNumberValue(item(item(item(snapshot, "settings"), "dnsProxy"), "timeoutMs"), 6000);
    atomic_store(&hold_resolver, true);
    int started = atomic_load(&starts);
    ASSERT_EQ(202, submit(web, snapshot, &ack));
    ASSERT_EQ(200, request(web, "GET", "/api/v1/system/config/snapshot", NULL, &response));
    cJSON_ReplaceItemInObjectCaseSensitive(item(item(response, "config"), "app"), "logLevel", cJSON_CreateString("warn"));
    char *body = cJSON_PrintUnformatted(response); cJSON_Delete(response);
    ASSERT_EQ(200, request(web, "POST", "/api/v1/system/config/apply", body, &response)); free(body); cJSON_Delete(response);
    atomic_store(&hold_resolver, false);
    response = await_operation(web, text_value(item(ack, "operation"), "id"), "failed"); ASSERT(response);
    ASSERT_EQ(started, atomic_load(&starts)); ASSERT_EQ(MT_LOG_ERROR, mt_log_level());
    ASSERT_STR_EQ("warn", text_value(item(response, "settings"), "logLevel"));
    cJSON_Delete(ack);
    ASSERT_EQ(202, submit(web, response, &ack)); cJSON_Delete(response);
    response = await_operation(web, text_value(item(ack, "operation"), "id"), "succeeded"); ASSERT(response);
    ASSERT_EQ(MT_LOG_WARN, mt_log_level()); ASSERT_EQ(started + 1, atomic_load(&starts));
    cJSON_Delete(snapshot); cJSON_Delete(ack); cJSON_Delete(response); harness_stop(h); stub_stop(&stub); PASS();
}

TEST startup_and_persistence_failures_restore_old_listeners_file_and_runtime(void)
{
    dns_stub_t stub; ASSERT(stub_start(&stub, 12)); harness_t *h = harness_start(stub.port, false); ASSERT(h);
    uint16_t web = h->config.app.http_web.host.port, dns = h->config.app.dns_proxy.host.port;
    for (int kind = 0; kind < 5; kind++) {
        cJSON *snapshot = NULL, *ack = NULL;
        ASSERT_EQ(200, request(web, "GET", "/api/v1/system/settings", NULL, &snapshot));
        char *before = read_file(h->path); ASSERT(before);
        cJSON *settings = item(snapshot, "settings");
        cJSON_SetNumberValue(item(item(settings, "dnsProxy"), "timeoutMs"), 6000);
        uint16_t busy_port = 0; int busy_fd = -1;
        if (kind == 0) { atomic_store(&fail_rename, 1); }
        if (kind == 1) { busy_fd = bound_socket(SOCK_STREAM, &busy_port); ASSERT(busy_fd >= 0); ASSERT_EQ(0, listen(busy_fd, 4)); cJSON_SetNumberValue(item(item(item(settings, "httpWeb"), "host"), "port"), busy_port); }
        if (kind == 2) { atomic_store(&fail_iptables, 1); cJSON_ReplaceItemInObjectCaseSensitive(item(settings, "netfilter"), "disableIPv4", cJSON_CreateFalse()); }
        if (kind == 3) { atomic_store(&fail_snapshot, 1); }
        if (kind == 4) { busy_fd = bound_socket(SOCK_DGRAM, &busy_port); ASSERT(busy_fd >= 0); cJSON_SetNumberValue(item(item(item(settings, "dnsProxy"), "host"), "port"), busy_port); }
        ASSERT_EQ(202, submit(web, snapshot, &ack));
        cJSON *confirmed = await_operation(web, text_value(item(ack, "operation"), "id"), "failed"); ASSERT(confirmed);
        ASSERT_STR_EQ(text_value(snapshot, "revision"), text_value(confirmed, "revision"));
        ASSERT(*text_value(item(confirmed, "operation"), "error"));
        char *after = read_file(h->path); ASSERT(after); ASSERT_STR_EQ(before, after);
        ASSERT_EQ(1, dns_query(dns, false, false, NULL)); ASSERT_EQ(1, dns_query(dns, true, false, NULL));
        if (busy_fd >= 0) { close(busy_fd); }
        free(before); free(after); cJSON_Delete(snapshot); cJSON_Delete(ack); cJSON_Delete(confirmed);
    }
    harness_stop(h); stub_stop(&stub); PASS();
}

TEST http_dns_ports_auth_and_web_disable_take_effect(void)
{
    dns_stub_t stub; ASSERT(stub_start(&stub, 12)); harness_t *h = harness_start(stub.port, false); ASSERT(h);
    uint16_t old_web = h->config.app.http_web.host.port, web = unused_port(), dns = unused_port();
    cJSON *snapshot = NULL, *ack = NULL, *response = NULL;
    ASSERT_EQ(200, request(old_web, "GET", "/api/v1/system/settings", NULL, &snapshot));
    cJSON *settings = item(snapshot, "settings");
    cJSON_SetNumberValue(item(item(item(settings, "httpWeb"), "host"), "port"), web);
    cJSON_SetNumberValue(item(item(item(settings, "dnsProxy"), "host"), "port"), dns);
    cJSON_ReplaceItemInObjectCaseSensitive(item(item(settings, "httpWeb"), "auth"), "enabled", cJSON_CreateTrue());
    ASSERT_EQ(202, submit(old_web, snapshot, &ack));
    ASSERT_EQ(web, item(item(ack, "next_web"), "port")->valueint);
    response = await_operation(0, text_value(item(ack, "operation"), "id"), "succeeded"); ASSERT(response);
    ASSERT_EQ(1, dns_query(dns, false, false, NULL)); ASSERT_EQ(1, dns_query(dns, true, false, NULL));
    cJSON *auth = NULL; ASSERT_EQ(401, request(web, "GET", "/api/v1/system/settings", NULL, &auth)); cJSON_Delete(auth);
    ASSERT_EQ(200, request(web, "GET", "/api/v1/auth", NULL, &auth)); ASSERT(cJSON_IsTrue(item(auth, "enabled"))); cJSON_Delete(auth);
    ASSERT(connection(old_web) < 0);
    cJSON_Delete(snapshot); cJSON_Delete(ack); snapshot = response;
    cJSON_ReplaceItemInObjectCaseSensitive(item(item(snapshot, "settings"), "httpWeb"), "enabled", cJSON_CreateFalse());
    ASSERT_EQ(202, submit((uint16_t)atomic_load(&admin_port), snapshot, &ack));
    response = await_operation(0, text_value(item(ack, "operation"), "id"), "succeeded"); ASSERT(response);
    ASSERT(connection(web) < 0); ASSERT_EQ(1, dns_query(dns, false, false, NULL));
    cJSON_Delete(snapshot); cJSON_Delete(ack); cJSON_Delete(response); harness_stop(h); stub_stop(&stub); PASS();
}

TEST acknowledgement_flush_gates_apply_and_disconnect_cancels_it(void)
{
    dns_stub_t stub; ASSERT(stub_start(&stub, 12)); harness_t *h = harness_start(stub.port, false); ASSERT(h);
    uint16_t web = h->config.app.http_web.host.port, dns = h->config.app.dns_proxy.host.port;
    for (int restart = 0; restart < 2; restart++) for (int disconnect = 1; disconnect >= 0; disconnect--) {
        cJSON *snapshot = NULL, *ack = NULL;
        ASSERT_EQ(200, request(web, "GET", "/api/v1/system/settings", NULL, &snapshot));
        if (restart) { replace_setting(snapshot, "dnsProxy.timeoutMs", "6000"); }
        else { replace_setting(snapshot, "showAllInterfaces", "true"); }
        char *body = payload(snapshot), *before = read_file(h->path); ASSERT(before);
        atomic_store(&held_fd, -1); atomic_store(&hold_send, true); atomic_store(&held_sends, 0);
        int started = atomic_load(&starts), fd = send_request(web, "PUT", "/api/v1/system/settings", body); free(body); ASSERT(fd >= 0);
        for (size_t i = 0; !atomic_load(&held_sends) && i < 200; i++) { pause_ms(1); }
        ASSERT(atomic_load(&held_sends) > 0);
        ASSERT_EQ(started, atomic_load(&starts)); ASSERT_EQ(1, dns_query(dns, false, false, NULL));
        char *during = read_file(h->path); ASSERT_STR_EQ(before, during); free(during);
        if (disconnect) {
            struct linger reset = {1, 0}; setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)); close(fd);
            atomic_store(&hold_send, false);
            cJSON *observed = NULL;
            for (size_t i = 0; i < 200; i++) {
                cJSON_Delete(observed); observed = NULL;
                (void)request(web, "GET", "/api/v1/system/settings", NULL, &observed);
                if (!strcmp(text_value(item(observed, "operation"), "state"), "failed")) { break; }
                pause_ms(1);
            }
            ASSERT_STR_EQ("failed", text_value(item(observed, "operation"), "state"));
            ASSERT_EQ(started, atomic_load(&starts)); cJSON_Delete(observed);
        } else {
            atomic_store(&hold_send, false);
            ASSERT_EQ(202, read_response(fd, &ack));
            cJSON *confirmed = await_operation(web, text_value(item(ack, "operation"), "id"), "succeeded"); ASSERT(confirmed); cJSON_Delete(confirmed);
            ASSERT_EQ(started + restart, atomic_load(&starts));
        }
        free(before); cJSON_Delete(snapshot); cJSON_Delete(ack);
    }
    harness_stop(h); stub_stop(&stub); PASS();
}

TEST rollback_failure_keeps_admin_status_and_same_settings_retry_recovers(void)
{
    dns_stub_t stub; ASSERT(stub_start(&stub, 12)); harness_t *h = harness_start(stub.port, false); ASSERT(h);
    uint16_t web = h->config.app.http_web.host.port;
    cJSON *snapshot = NULL, *ack = NULL;
    ASSERT_EQ(200, request(web, "GET", "/api/v1/system/settings", NULL, &snapshot));
    cJSON_SetNumberValue(item(item(item(snapshot, "settings"), "dnsProxy"), "timeoutMs"), 6000);
    atomic_store(&fail_rtnl, 2);
    ASSERT_EQ(202, submit(web, snapshot, &ack));
    cJSON *failed = await_operation(0, text_value(item(ack, "operation"), "id"), "rollback_failed"); ASSERT(failed);
    ASSERT_STR_EQ(text_value(snapshot, "revision"), text_value(failed, "revision"));
    cJSON_Delete(ack);
    /* A failed preparation must not turn admin-only recovery into a valid
     * active-runtime baseline for a later identical request. */
    cJSON *unresolvable = cJSON_Duplicate(failed, true);
    cJSON_ReplaceItemInObjectCaseSensitive(item(item(item(unresolvable, "settings"), "dnsProxy"), "upstream"), "address", cJSON_CreateString("unresolvable.invalid"));
    ASSERT_EQ(202, submit((uint16_t)atomic_load(&admin_port), unresolvable, &ack)); cJSON_Delete(unresolvable);
    cJSON *preparation_failed = await_operation(0, text_value(item(ack, "operation"), "id"), "failed"); ASSERT(preparation_failed);
    cJSON *live = cJSON_Duplicate(preparation_failed, true), *impact = NULL;
    replace_setting(live, "showAllInterfaces", "true");
    ASSERT_EQ(200, preview((uint16_t)atomic_load(&admin_port), live, &impact));
    ASSERT(cJSON_IsTrue(item(impact, "dns_restart_required"))); cJSON_Delete(impact);
    ASSERT_EQ(409, submit_without_restart((uint16_t)atomic_load(&admin_port), live, &impact));
    ASSERT_STR_EQ("SETTINGS_RESTART_REQUIRED", text_value(impact, "code")); cJSON_Delete(impact);
    ASSERT_EQ(200, request((uint16_t)atomic_load(&admin_port), "GET", "/api/v1/system/settings", NULL, &impact));
    ASSERT_STR_EQ(text_value(item(preparation_failed, "operation"), "id"), text_value(item(impact, "operation"), "id"));
    cJSON_Delete(impact); cJSON_Delete(live); cJSON_Delete(preparation_failed); cJSON_Delete(ack);
    ASSERT_EQ(202, submit((uint16_t)atomic_load(&admin_port), failed, &ack));
    cJSON *confirmed = await_operation(web, text_value(item(ack, "operation"), "id"), "succeeded"); ASSERT(confirmed);
    ASSERT_EQ(1, dns_query(h->config.app.dns_proxy.host.port, false, false, NULL));
    cJSON_Delete(snapshot); cJSON_Delete(ack); cJSON_Delete(failed); cJSON_Delete(confirmed);
    harness_stop(h); stub_stop(&stub); PASS();
}

TEST resolution_failure_and_true_noop_leave_runtime_running(void)
{
    dns_stub_t stub; ASSERT(stub_start(&stub, 12)); harness_t *h = harness_start(stub.port, false); ASSERT(h);
    uint16_t web = h->config.app.http_web.host.port;
    cJSON *snapshot = NULL, *ack = NULL;
    ASSERT_EQ(200, request(web, "GET", "/api/v1/system/settings", NULL, &snapshot));
    int started = atomic_load(&starts);
    ASSERT_EQ(200, submit(web, snapshot, &ack)); ASSERT_STR_EQ("succeeded", text_value(item(ack, "operation"), "state")); cJSON_Delete(ack);
    cJSON_ReplaceItemInObjectCaseSensitive(item(item(item(snapshot, "settings"), "dnsProxy"), "upstream"), "address", cJSON_CreateString("unresolvable.invalid"));
    ASSERT_EQ(202, submit(web, snapshot, &ack));
    cJSON *failed = await_operation(web, text_value(item(ack, "operation"), "id"), "failed"); ASSERT(failed);
    ASSERT_EQ(started, atomic_load(&starts)); ASSERT_EQ(1, dns_query(h->config.app.dns_proxy.host.port, false, false, NULL));
    cJSON_Delete(snapshot); cJSON_Delete(ack); cJSON_Delete(failed); harness_stop(h); stub_stop(&stub); PASS();
}

TEST family_and_dns_interception_components_are_recreated(void)
{
    dns_stub_t stub; ASSERT(stub_start(&stub, 12)); harness_t *h = harness_start(stub.port, false); ASSERT(h);
    uint16_t web = h->config.app.http_web.host.port;
    cJSON *snapshot = NULL, *ack = NULL;
    ASSERT_EQ(200, request(web, "GET", "/api/v1/system/settings", NULL, &snapshot));
    cJSON *settings = item(snapshot, "settings");
    cJSON_ReplaceItemInObjectCaseSensitive(item(settings, "netfilter"), "disableIPv4", cJSON_CreateFalse());
    cJSON_ReplaceItemInObjectCaseSensitive(item(settings, "netfilter"), "disableIPv6", cJSON_CreateFalse());
    cJSON_ReplaceItemInObjectCaseSensitive(item(settings, "dnsProxy"), "disableRemap53", cJSON_CreateFalse());
    cJSON_ReplaceItemInObjectCaseSensitive(settings, "link", cJSON_CreateArray());
    ASSERT_EQ(202, submit(web, snapshot, &ack));
    cJSON *confirmed = await_operation(web, text_value(item(ack, "operation"), "id"), "succeeded"); ASSERT(confirmed);
    cJSON *diag = NULL; ASSERT_EQ(200, request(web, "GET", "/test/state", NULL, &diag));
    ASSERT(cJSON_IsTrue(item(diag, "ipv4"))); ASSERT(cJSON_IsTrue(item(diag, "ipv6"))); ASSERT(cJSON_IsTrue(item(diag, "remap")));
    ASSERT_EQ(1, dns_query(h->config.app.dns_proxy.host.port, false, false, NULL));
    cJSON_Delete(diag); cJSON_Delete(snapshot); cJSON_Delete(ack); cJSON_Delete(confirmed);
    harness_stop(h); stub_stop(&stub); PASS();
}

TEST legacy_ephemeral_ports_are_usable_and_rollback_restores_both_transports(void)
{
    dns_stub_t stub; ASSERT(stub_start(&stub, 12)); harness_t *h = harness_start_ports(stub.port, false, true); ASSERT(h);
    uint16_t web = h->daemon.ports.http, dns = h->daemon.ports.dns;
    ASSERT(web > 0); ASSERT(dns > 0);
    cJSON *diag = NULL;
    ASSERT_EQ(200, request(web, "GET", "/test/state", NULL, &diag));
    cJSON *targets = item(diag, "remap_targets");
    ASSERT_EQ(2, cJSON_GetArraySize(targets));
    char destination[16]; snprintf(destination, sizeof(destination), ":%u", dns);
    ASSERT_STR_EQ(destination, cJSON_GetArrayItem(targets, 0)->valuestring);
    ASSERT_STR_EQ(destination, cJSON_GetArrayItem(targets, 1)->valuestring);
    cJSON_Delete(diag);
    ASSERT_EQ(1, dns_query(dns, false, false, NULL)); ASSERT_EQ(1, dns_query(dns, true, false, NULL));
    cJSON *snapshot = NULL, *ack = NULL;
    ASSERT_EQ(200, request(web, "GET", "/api/v1/system/settings", NULL, &snapshot));
    ASSERT_EQ(web, item(item(item(item(snapshot, "settings"), "httpWeb"), "host"), "port")->valueint);
    ASSERT_EQ(dns, item(item(item(item(snapshot, "settings"), "dnsProxy"), "host"), "port")->valueint);
    ASSERT_EQ(200, submit(web, snapshot, &ack)); cJSON_Delete(ack);
    mt_loop_t *loop = h->daemon.loop;
    replace_setting(snapshot, "showAllInterfaces", "true");
    ASSERT_EQ(202, submit_without_restart(web, snapshot, &ack));
    cJSON *live = await_operation(web, text_value(item(ack, "operation"), "id"), "succeeded"); ASSERT(live);
    ASSERT(h->daemon.loop == loop); ASSERT_EQ(0, h->config.app.http_web.host.port); ASSERT_EQ(0, h->config.app.dns_proxy.host.port);
    mt_config_t persisted; ASSERT_EQ(MT_OK, mt_config_init_defaults(&persisted));
    ASSERT_EQ(MT_OK, mt_config_load_file(&persisted, h->path));
    ASSERT_EQ(0, persisted.app.http_web.host.port); ASSERT_EQ(0, persisted.app.dns_proxy.host.port); mt_config_clear(&persisted);
    ASSERT_EQ(1, dns_query(dns, false, false, NULL)); ASSERT_EQ(1, dns_query(dns, true, false, NULL));
    cJSON_Delete(snapshot); snapshot = live; cJSON_Delete(ack);
    char *before = read_file(h->path); ASSERT(before);
    cJSON_SetNumberValue(item(item(item(snapshot, "settings"), "dnsProxy"), "timeoutMs"), 6000);
    atomic_store(&fail_rename, 1);
    ASSERT_EQ(202, submit(web, snapshot, &ack));
    cJSON *failed = await_operation(web, text_value(item(ack, "operation"), "id"), "failed"); ASSERT(failed);
    ASSERT_STR_EQ(text_value(snapshot, "revision"), text_value(failed, "revision"));
    ASSERT_EQ(1, dns_query(dns, false, false, NULL)); ASSERT_EQ(1, dns_query(dns, true, false, NULL));
    char *after = read_file(h->path); ASSERT(after); ASSERT_STR_EQ(before, after);
    free(before); free(after); cJSON_Delete(snapshot); cJSON_Delete(ack); cJSON_Delete(failed);
    harness_stop(h); stub_stop(&stub); PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    mt_sub_fetch_global_init();
    RUN_TEST(dns_stub_retries_cross_protocol_port_collision_without_leaking_sockets);
    RUN_TEST(projection_is_strict_and_preserves_hidden_settings);
    RUN_TEST(preview_classifies_actual_changes_without_mutating_or_resolving);
    RUN_TEST(live_settings_preserve_dns_runtime_and_revoke_unauthenticated_streams);
    RUN_TEST(live_persistence_failure_preserves_memory_file_and_dns);
    RUN_TEST(live_acknowledgement_cannot_escalate_to_dns_restart);
    RUN_TEST(real_dns_upstream_switch_preserves_cache_and_repopulates_sets);
    RUN_TEST(pending_resolution_keeps_dns_live_and_preserves_later_profile_saves);
    RUN_TEST(stale_request_and_invalid_fields_do_not_change_file_or_runtime);
    RUN_TEST(legacy_apply_invalidates_pending_settings_and_identical_retry_recreates_runtime);
    RUN_TEST(startup_and_persistence_failures_restore_old_listeners_file_and_runtime);
    RUN_TEST(http_dns_ports_auth_and_web_disable_take_effect);
    RUN_TEST(acknowledgement_flush_gates_apply_and_disconnect_cancels_it);
    RUN_TEST(rollback_failure_keeps_admin_status_and_same_settings_retry_recovers);
    RUN_TEST(resolution_failure_and_true_noop_leave_runtime_running);
    RUN_TEST(family_and_dns_interception_components_are_recreated);
    RUN_TEST(legacy_ephemeral_ports_are_usable_and_rollback_restores_both_transports);
    mt_sub_fetch_global_cleanup();
    GREATEST_MAIN_END();
}
