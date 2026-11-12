/* magitrickled-c — Phase 5 daemon.
 *
 * Loads the YAML config (defaults + overlay), wires up the full netfilter
 * layer (iptables chain patches + cleanup, per-group ipset/ipset-to-link via
 * mt_ruleset_t, port 53 DNAT remap, rtnetlink link/addr watcher), then starts
 * the epoll DNS MITM proxy and processes every upstream response through the
 * records cache + rule matching pipeline (dns.go's handleMessage, ported —
 * see dnspipeline.h), dispatching matches into the matched group's ipset
 * (rule_set.go's AddIPv4Subnet/AddIPv6Subnet, ported — see ruleset.h).
 *
 * Also wires up the HTTP/Unix-socket API (Phase 6): groups/rules CRUD,
 * system endpoints (interfaces/config-save/netfilter.d hook), auth
 * (crypt+JWT), and skin static file serving, all sharing one mt_app_t
 * that now owns the per-group mt_ruleset_t registry this file used to
 * manage inline. Subscription HTTP endpoints are not yet wired (task
 * #36). The Go backend remains authoritative until the migration
 * completes (docs/c-rewrite/).
 */
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "magitrickle/app.h"
#ifdef MT_ENTWARE_KN
#include "magitrickle/dns_keenetic_hook.h"
#include "magitrickle/keenetic_policy_api.h"
#include "magitrickle/keenetic_ifstate_service.h"
#endif
#include "magitrickle/auth.h"
#include "magitrickle/dns_cache.h"
#include "magitrickle/dns_query_log.h"
#include "magitrickle/dnspipeline.h"
#include "magitrickle/dnsproxy.h"
#include "magitrickle/groups.h"
#include "magitrickle/httpd.h"
#include "magitrickle/iptables.h"
#include "magitrickle/log.h"
#include "magitrickle/loop.h"
#include "magitrickle/netfilter_cleaner.h"
#include "magitrickle/netlink_watcher.h"
#include "magitrickle/paths.h"
#include "magitrickle/port_remap.h"
#include "magitrickle/rtnl.h"
#include "magitrickle/rulesnap.h"
#include "magitrickle/ruleset.h"
#include "magitrickle/staticfiles.h"
#include "magitrickle/sub_fetch.h"
#include "magitrickle/subscriptions_api.h"
#include "magitrickle/system.h"
#include "magitrickle/version.h"
#include "magitrickle/update.h"
#include "magitrickle/yamlio.h"

/* Matches Go's cfgFileLocation = AppStateDir + "/config.yaml", so the
 * config file follows the per-platform state dir (paths.h). */
#ifndef MT_CONFIG_PATH
#define MT_CONFIG_PATH MT_APP_STATE_DIR "/config.yaml"
#endif

#define MT_CACHE_CLEANUP_INTERVAL_MS 30000
/* Matches subscriptions.go's subscriptionAutoUpdateTick = time.Minute. */
#define MT_SUBSCRIPTION_AUTO_UPDATE_INTERVAL_MS 60000

struct bound_ports { uint16_t http, dns; };

struct daemon {
#ifdef MT_ENTWARE_KN
    mt_kn_policy_service_t *policy_service;
    mt_kn_ifstate_t *interface_states;
    mt_kn_ifstate_service_t *interface_service;
    /* Event bursts are bounded; overflow degrades to all profiles, never
     * direct-interface routes. Kernel events still reconcile all routes. */
    char ndm_names[64][16];
    size_t n_ndm_names;
    bool ndm_all, kernel_reconcile;
#endif
    mt_loop_t *loop;
    mt_dnsproxy_t *proxy;
    mt_dns_query_log_t *query_log;
    mt_dns_clients_t *dns_clients;
    mt_cache_t *cache;
    mt_dns_pipeline_t *pipeline;
    mt_ipt_t *ipt4;
    mt_ipt_t *ipt6;
    mt_rtnl_t *rtnl;
    mt_nl_watcher_t *watcher;
    int route_reconcile_timer;
    unsigned route_retry_ms;
    mt_port_remap_t *port_remap;
    mt_app_t *app;
    mt_sub_fetcher_t *fetcher;
    mt_httpd_t *http_tcp;
    mt_httpd_t *http_unix;
    mt_settings_t *settings;
    mt_dns_upstream_t upstream;
    struct bound_ports ports;
    mt_groups_ctx_t groups_ctx;
    mt_system_ctx_t system_ctx;
    mt_subs_ctx_t subs_ctx;
    mt_auth_ctx_t auth_ctx;
    mt_static_ctx_t static_ctx;
    bool shutdown_requested;
    const char *failure_stage;

    /* Not owned by daemon_teardown (cfg is main()'s own local, config_path
     * points at argv or MT_CONFIG_PATH) -- only here so on_signal's SIGHUP
     * handler can reach them for a config reload. */
    mt_config_t *cfg;
    const char *config_path;
};

static int64_t now_unix(void)
{
    return (int64_t)time(NULL);
}

/* Reverse-order teardown of everything in `struct daemon`. Every
 * destroy/free/disable below is NULL-safe and idempotent, so this can be
 * called from any partially-constructed state (d is always zero-initialized
 * at the top of main). The HTTP servers must be destroyed while the loop is
 * still alive (mt_httpd_destroy removes their fds from it). app/port_remap
 * borrow ipt4/ipt6/rtnl/cache, so they must be torn down before those; the
 * loop must outlive the watcher/proxy fds registered on it too. */
static mt_err_t daemon_teardown(struct daemon *d)
{
    mt_err_t first = MT_OK;
    mt_settings_runtime_stopped(d->settings);
    mt_settings_attach_loop(d->settings, NULL);
    mt_update_shutdown();
#ifdef MT_ENTWARE_KN
    mt_kn_ifstate_service_destroy(d->interface_service);
    d->interface_service = NULL;
    mt_kn_policy_service_destroy(d->policy_service);
    d->policy_service = NULL;
#endif
    mt_sub_fetcher_destroy(d->fetcher);
    d->fetcher = NULL;
    /* After joining I/O workers: the committer thread reads the ruleset registry and
     * drives the iptables engines, so nothing it touches may be torn down
     * while it still runs. Stopping it also aborts a write in flight, so
     * this does not wait out a full rebuild. */
    mt_app_stop_netfilter_committer(d->app);

    mt_httpd_destroy(d->http_tcp);
    d->http_tcp = NULL;
    mt_httpd_destroy(d->http_unix);
    d->http_unix = NULL;
    /* Streams unsubscribe before their optional label cache is destroyed. */
    mt_dns_clients_destroy(d->dns_clients);
    d->dns_clients = NULL;

    if (d->route_reconcile_timer) {
        (void)mt_loop_del_timer(d->loop, d->route_reconcile_timer);
        d->route_reconcile_timer = 0;
    }
    first = mt_app_suspend(d->app);
    mt_app_destroy(d->app);
    d->app = NULL;
#ifdef MT_ENTWARE_KN
    mt_kn_ifstate_free(d->interface_states);
    d->interface_states = NULL;
#endif

    if (d->port_remap) {
        mt_err_t err = mt_port_remap_disable(d->port_remap);
        if (first == MT_OK) { first = err; }
        mt_port_remap_free(d->port_remap);
        d->port_remap = NULL;
    }

    mt_nl_watcher_destroy(d->watcher);
    d->watcher = NULL;
    mt_dnsproxy_destroy(d->proxy);
    d->proxy = NULL;
    mt_dns_query_log_destroy(d->query_log);
    d->query_log = NULL;
    mt_dns_pipeline_destroy(d->pipeline);
    d->pipeline = NULL;
    /* DNS-derived routing entries survive a General settings cycle. */
    d->cache = NULL;
    mt_ipt_free(d->ipt4);
    d->ipt4 = NULL;
    mt_ipt_free(d->ipt6);
    d->ipt6 = NULL;
    mt_rtnl_close(d->rtnl);
    d->rtnl = NULL;
    mt_loop_destroy(d->loop);
    d->loop = NULL;
    return first;
}

static mt_ruleset_t *find_ruleset(struct daemon *d, mt_id_t id)
{
    mt_ruleset_t *rs = mt_app_find_group_by_id(d->app, id);
    if (rs != NULL) {
        return rs;
    }
    return mt_app_find_subscription_ruleset_by_id(d->app, id);
}

/* Reload stored state and live API settings together. DNS flags live in
 * the transport too; refresh them even on a partial group-apply failure,
 * matching Go's app-overlay-before-groups order. */
static void reload_config(struct daemon *d)
{
    mt_err_t err = mt_app_reload_config(d->app, d->config_path);
    mt_httpd_revalidate_streams(d->http_tcp);
    if (d->proxy) {
        mt_dnsproxy_set_disable_flags(d->proxy,
            d->cfg->app.dns_proxy.disable_fake_ptr,
            d->cfg->app.dns_proxy.disable_drop_aaaa);
    }
    if (err == MT_ERR_NOENT) {
        MT_INFO("reload: config file %s not found, keeping current config", d->config_path);
    } else if (err != MT_OK) {
        MT_ERROR("failed to reload config %s: %s", d->config_path, mt_err_str(err));
    } else {
        MT_INFO("config reloaded from %s", d->config_path);
    }
}

static void on_signal(mt_loop_t *loop, int signo, void *ud)
{
    struct daemon *d = ud;
    if (signo == SIGHUP) {
        MT_INFO("received signal: hangup (reloading config)");
        reload_config(d);
        return;
    }
    MT_INFO("received signal: %s", strsignal(signo));
    d->shutdown_requested = true;
    mt_loop_stop(loop);
}

static void on_cache_cleanup_timer(mt_loop_t *loop, void *ud)
{
    (void)loop;
    struct daemon *d = ud;
    mt_cache_cleanup(d->cache, now_unix());
}

static void auto_update_done(void *ud, mt_id_t id, mt_err_t err, bool changed)
{
    (void)id;
    struct daemon *d = ud;
    if (err == MT_ERR_CANCELED || err == MT_ERR_STATE || err == MT_ERR_NOENT) { return; }
    if (err != MT_OK) { MT_ERROR("failed to sync subscription: %s", mt_err_str(err)); }
    if (changed) {
        mt_err_t serr = mt_app_save_config(d->app, d->config_path, MT_VERSION);
        if (serr != MT_OK) { MT_ERROR("failed to save config file: %s", mt_err_str(serr)); }
    }
}

/* Only enqueue network work here. Completion applies rules on this loop,
 * and stale results after an edit/reload are discarded by app.c. */
static void on_subscription_auto_update_timer(mt_loop_t *loop, void *ud)
{
    (void)loop;
    struct daemon *d = ud;
    mt_err_t err = mt_app_sync_due_subscriptions_async(d->app, d->fetcher,
                                                      now_unix(), auto_update_done, d);
    if (err != MT_OK) { MT_ERROR("failed to schedule subscription sync: %s", mt_err_str(err)); }
}

/* Match sink: dispatches into the matched group's ipset -- port of dns.go's
 * processARecord/processAAAARecord/processCNameRecord AddIPv4Subnet/
 * AddIPv6Subnet calls. */
static void on_match(const mt_match_action_t *action, void *ud)
{
    struct daemon *d = ud;
    mt_ruleset_t *rs = find_ruleset(d, action->group_id);
    if (rs == NULL) {
        return;
    }

    const mt_group_t *group = mt_ruleset_group(rs);
    char address[INET6_ADDRSTRLEN] = "unavailable";
    (void)inet_ntop(action->addr_len == 4 ? AF_INET : AF_INET6,
                    action->addr, address, sizeof(address));
    const char *iface = group && group->iface ? group->iface : "";
    const char *profile = group && group->profile ? group->profile : "";
    uint32_t ttl = action->ttl_seconds;
    mt_err_t err = MT_OK;
    unsigned attempts = action->attempts == 0 ? 1u : action->attempts;
    unsigned performed = 0;
    for (unsigned i = 0; i < attempts; i++) {
        performed++;
        if (action->addr_len == 4) {
            mt_ipv4_subnet_t subnet = {.cidr = 0};
            memcpy(subnet.addr, action->addr, 4);
            err = mt_ruleset_add_ipv4(rs, subnet, &ttl);
        } else {
            mt_ipv6_subnet_t subnet = {.cidr = 0};
            memcpy(subnet.addr, action->addr, 16);
            err = mt_ruleset_add_ipv6(rs, subnet, &ttl);
        }
        /* Coalescing does not turn several chances to write into just one
         * on a transient kernel error. Retry only the final desired TTL. */
        if (err == MT_OK) { break; }
    }

    if (err != MT_OK) {
        MT_ERROR(
            "routing address update failed: kind=%s address=%s domain=%s matched=%s "
            "group=%s interface=%s profile=%s ttl=%u attempts=%u error=%s",
            action->record_kind, address, action->record_domain, action->matched_name,
            action->group_name, iface, profile, ttl, performed, mt_err_str(err));
        return;
    }
    MT_INFO(
        "routing address applied: kind=%s address=%s domain=%s matched=%s "
        "group=%s interface=%s profile=%s ttl=%u attempts=%u",
        action->record_kind, address, action->record_domain, action->matched_name,
        action->group_name, iface, profile, ttl, performed);
}

static void on_response(const mt_dns_msg_t *msg, const char *client_addr,
                        const char *network, void *ud)
{
    /* Response diagnostics live in the DNS transport, where the client,
     * upstream and elapsed time are known. No duplicated summary here. */
    (void)client_addr;
    (void)network;
    struct daemon *d = ud;
    mt_dns_pipeline_handle_message(d->pipeline, msg, now_unix());
}

/* Netlink events are coalesced. Only a real event, receive loss or failed
 * reconciliation schedules work; there is no periodic profile poll. */
static void on_route_reconcile_timer(mt_loop_t *loop, void *ud)
{
    struct daemon *d = ud;
    d->route_reconcile_timer = 0;
    mt_err_t err = MT_OK;
#ifdef MT_ENTWARE_KN
    if (d->kernel_reconcile) { err = mt_app_reconcile_routes(d->app, NULL); }
    else if (d->ndm_all) { err = mt_app_reconcile_profiles(d->app, NULL); }
    else {
        for (size_t i = 0; i < d->n_ndm_names; i++) {
            mt_err_t item = mt_app_reconcile_profiles(d->app, d->ndm_names[i]);
            if (err == MT_OK) { err = item; }
        }
    }
#else
    err = mt_app_reconcile_routes(d->app, NULL);
#endif
    if (err == MT_OK) {
#ifdef MT_ENTWARE_KN
        d->kernel_reconcile = d->ndm_all = false;
        d->n_ndm_names = 0;
#endif
        d->route_retry_ms = 0;
        return;
    }
    unsigned delay = d->route_retry_ms ? d->route_retry_ms : 100u;
    if (delay > 30000u) { delay = 30000u; }
    /* A busy netfilter rebuild is an expected contention event, not a
     * failed route. Keep retrying but promote it to WARN only if it has
     * lasted long enough to warrant attention. Other failures stay WARN. */
    if (err == MT_ERR_AGAIN && delay < 3200u) {
        MT_DEBUG("netlink reconciliation delayed: %s; retry in %u ms",
                 mt_err_str(err), delay);
    } else {
        MT_WARN("netlink reconciliation pending: %s; retry in %u ms",
                mt_err_str(err), delay);
    }
    d->route_retry_ms = delay >= 15000u ? 30000u : delay * 2u;
    if (mt_loop_add_timer(loop, delay, 0, on_route_reconcile_timer, d,
                          &d->route_reconcile_timer) != MT_OK) {
        MT_ERROR("cannot schedule netlink reconciliation; stopping daemon");
        mt_loop_stop(loop);
    }
}

static void arm_route_reconcile(struct daemon *d)
{
    if (d->route_reconcile_timer) { return; }
    if (mt_loop_add_timer(d->loop, 20, 0, on_route_reconcile_timer, d,
                          &d->route_reconcile_timer) != MT_OK) {
        MT_ERROR("cannot schedule netlink reconciliation; stopping daemon");
        mt_loop_stop(d->loop);
    }
}

static void schedule_route_reconcile(struct daemon *d)
{
#ifdef MT_ENTWARE_KN
    d->kernel_reconcile = true;
#endif
    arm_route_reconcile(d);
}

#ifdef MT_ENTWARE_KN
static void on_ndm_state_changed(void *ud, const char *name)
{
    struct daemon *d = ud;
    if (!d->ndm_all) {
        for (size_t i = 0; i < d->n_ndm_names; i++) {
            if (!strcmp(d->ndm_names[i], name)) { return; }
        }
        if (d->n_ndm_names == sizeof(d->ndm_names) / sizeof(*d->ndm_names)) { d->ndm_all = true; }
        else { snprintf(d->ndm_names[d->n_ndm_names++], sizeof(*d->ndm_names), "%s", name); }
    }
    arm_route_reconcile(d);
}
/* Policy gates contain exact local-address exceptions, not xt_addrtype.
 * Refresh them on topology events through the existing coalescing worker.
 * Do NOT trigger this on route changes: MT writes those during a rebuild. */
static void refresh_policy_addresses(struct daemon *d)
{
    if (!d->app || !d->cfg || !d->cfg->app.keenetic_policy.enabled) { return; }
    mt_err_t err = mt_app_force_commit_iptables(d->app);
    if (err != MT_OK) { MT_ERROR("failed to refresh policy local addresses: %s", mt_err_str(err)); }
}
#endif

static void on_link_up(const char *iface_name, bool up, void *ud)
{
    struct daemon *d = ud;
    /* The watcher logs notification attributes at TRACE. up is not a
     * transition; do not misreport repeat notifications as changes. */
    (void)iface_name;
    (void)up;
#ifdef MT_ENTWARE_KN
    refresh_policy_addresses(d);
#endif
    schedule_route_reconcile(d);
}

static void on_addr_change(const char *iface_name, void *ud)
{
    struct daemon *d = ud;
    /* Detail is logged by the watcher before the unchanged reconciliation. */
    (void)iface_name;
#ifdef MT_ENTWARE_KN
    refresh_policy_addresses(d);
#endif
    schedule_route_reconcile(d);
}

static void on_route_change(void *ud)
{
    schedule_route_reconcile(ud);
}

static void on_netlink_resync(void *ud)
{
    struct daemon *d = ud;
    MT_WARN("netlink notification lost or watcher reconnected; resynchronizing routes");
#ifdef MT_ENTWARE_KN
    mt_kn_ifstate_service_resync(d->interface_service);
    refresh_policy_addresses(d);
#endif
    schedule_route_reconcile(d);
}

/* Collects the local addresses of the configured `link` interfaces for the
 * port-53 DNAT remap -- port of start.go's netlink.LinkByName/AddrList loop.
 * A configured interface that doesn't exist is a startup error, matching
 * Go's "failed to find link %s". */
static mt_err_t collect_link_addrs(char *const *link_names, size_t n_link,
                                   mt_remap_addr_t **out, size_t *out_n)
{
    *out = NULL;
    *out_n = 0;
    if (n_link == 0) {
        return MT_OK;
    }

    for (size_t i = 0; i < n_link; i++) {
        if (if_nametoindex(link_names[i]) == 0) {
            MT_ERROR("failed to find link %s: %s", link_names[i],
                     strerror(errno));
            return mt_err_from_errno(errno);
        }
    }

    struct ifaddrs *ifap;
    if (getifaddrs(&ifap) != 0) {
        return mt_err_from_errno(errno);
    }

    size_t cap = 0;
    size_t n = 0;
    mt_remap_addr_t *arr = NULL;
    for (struct ifaddrs *p = ifap; p != NULL; p = p->ifa_next) {
        if (p->ifa_addr == NULL) {
            continue;
        }
        bool matched = false;
        for (size_t i = 0; i < n_link; i++) {
            if (strcmp(p->ifa_name, link_names[i]) == 0) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            continue;
        }

        mt_remap_addr_t a = {0};
        if (p->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in sin;
            memcpy(&sin, p->ifa_addr, sizeof(sin));
            a.family = AF_INET;
            a.iplen = 4;
            memcpy(a.ip, &sin.sin_addr, 4);
        } else if (p->ifa_addr->sa_family == AF_INET6) {
            struct sockaddr_in6 sin6;
            memcpy(&sin6, p->ifa_addr, sizeof(sin6));
            a.family = AF_INET6;
            a.iplen = 16;
            memcpy(a.ip, &sin6.sin6_addr, 16);
        } else {
            continue;
        }

        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 8;
            mt_remap_addr_t *np = realloc(arr, ncap * sizeof(*np));
            if (!np) {
                free(arr);
                freeifaddrs(ifap);
                return MT_ERR_NOMEM;
            }
            arr = np;
            cap = ncap;
        }
        arr[n++] = a;
    }
    freeifaddrs(ifap);
    *out = arr;
    *out_n = n;
    return MT_OK;
}

/* HTTP glue callbacks -- ud is always the daemon's mt_config_t (see main()),
 * matching what auth.h/staticfiles.h expect. */
static bool auth_enabled_fn(void *ud)
{
    const mt_config_t *cfg = ud;
    return cfg->app.http_web.auth.enabled;
}

static const char *auth_state_dir_fn(void *ud)
{
    (void)ud;
    return MT_APP_STATE_DIR;
}

static const char *static_skin_fn(void *ud)
{
    const mt_config_t *cfg = ud;
    return cfg->app.http_web.skin;
}

/* Create a complete WebUI listener without disturbing DNS or any of its
 * routing machinery. The returned server is owned by the caller. */
static mt_err_t daemon_open_web(struct daemon *d, const mt_app_config_t *app,
                                uint16_t fallback_port, mt_httpd_t **out)
{
    mt_httpd_t *http = NULL;
    mt_err_t err = mt_httpd_create(d->loop, &http);
    if (err != MT_OK) { return err; }
    mt_groups_register_routes(http, &d->groups_ctx);
    mt_system_register_routes(http, &d->system_ctx);
#ifdef MT_ENTWARE_KN
    err = mt_kn_policy_register_routes(http, d->policy_service);
    if (err != MT_OK) { mt_httpd_destroy(http); return err; }
#endif
    mt_subs_register_routes(http, &d->subs_ctx);
    mt_httpd_route(http, "GET", "/api/v1/auth", mt_auth_status_handler, &d->auth_ctx);
    mt_httpd_route(http, "POST", "/api/v1/auth", mt_auth_login_handler, &d->auth_ctx);
    mt_httpd_set_middleware(http, mt_auth_middleware, &d->auth_ctx);
    mt_httpd_set_not_found(http, mt_static_handler, &d->static_ctx);
    err = mt_httpd_listen_tcp(http, app->http_web.host.address,
                             app->http_web.host.port ? app->http_web.host.port : fallback_port);
    if (err != MT_OK) { mt_httpd_destroy(http); return err; }
    *out = http;
    return MT_OK;
}

/* The controller calls this after the ACK handler unwound.
 * Prebind a newly enabled WebUI before changing the file. Disabling WebUI
 * happens only after persistence, without touching DNS listeners or routing. */
static mt_err_t daemon_apply_live_settings(mt_app_config_t *next, void *userdata)
{
    struct daemon *d = userdata;
    bool toggle_web = d->cfg->app.http_web.enabled != next->http_web.enabled;
    mt_httpd_t *new_web = NULL;
    mt_err_t err = MT_OK;
    if (toggle_web && next->http_web.enabled) {
        err = daemon_open_web(d, next, next->http_web.host.port, &new_web);
        if (err != MT_OK) { return err; }
    }
    mt_config_t persisted = *d->cfg; /* borrowed collections and non-live fields */
    mt_settings_overlay_live(&persisted.app, next);
    /* Legacy port 0 is projected to the actual bound port. Store that same
     * port when toggling so the accepted target revision remains accurate. */
    if (toggle_web && !persisted.app.http_web.host.port) {
        persisted.app.http_web.host.port = next->http_web.host.port;
    }
    err = mt_config_save_file(&persisted, MT_VERSION, d->config_path);
    if (err != MT_OK) { mt_httpd_destroy(new_web); return err; }
    char *previous_log_level = d->cfg->app.log_level;
    mt_settings_overlay_live(&d->cfg->app, next);
    if (toggle_web && !d->cfg->app.http_web.host.port) {
        d->cfg->app.http_web.host.port = next->http_web.host.port;
    }
    next->log_level = NULL; /* transfer this single owned string */
    free(previous_log_level);
    mt_log_set_level(mt_log_level_from_str(d->cfg->app.log_level));
    mt_dnsproxy_set_disable_flags(d->proxy, d->cfg->app.dns_proxy.disable_fake_ptr,
                                  d->cfg->app.dns_proxy.disable_drop_aaaa);
    mt_dns_pipeline_set_additional_ttl(d->pipeline,
        (uint32_t)(d->cfg->app.netfilter.ipset.additional_ttl / MT_DURATION_SEC));
    if (toggle_web) {
        mt_httpd_t *previous_web = d->http_tcp;
        d->http_tcp = new_web;
        d->ports.http = mt_httpd_tcp_port(new_web);
        mt_settings_bound_ports(d->settings, d->ports.http, d->ports.dns);
        mt_httpd_destroy(previous_web);
    }
    mt_httpd_revalidate_streams(d->http_tcp);
    return MT_OK;
}

/* Own all route contexts in the runtime: callbacks remain valid after this
 * initializer returns, including across another settings operation. */
static mt_err_t daemon_start(struct daemon *d, mt_config_t *cfg, const char *config_path,
                              mt_settings_t *settings, mt_cache_t *cache,
                              const mt_dns_upstream_t *prepared_upstream,
                              const struct bound_ports *restore_ports)
{
    memset(d, 0, sizeof(*d));
    d->cfg = cfg; d->config_path = config_path; d->settings = settings; d->cache = cache;
    mt_update_resume();
    mt_log_set_level(mt_log_level_from_str(cfg->app.log_level));
    mt_err_t err = MT_OK;
    d->failure_stage = "initialize event loop";
    if (mt_loop_create(&d->loop) != MT_OK) {
        MT_ERROR("failed to create event loop");
        return err != MT_OK ? err : MT_ERR_SYS;
    }

    mt_settings_attach_loop(settings, d->loop);

    uint32_t additional_ttl = (uint32_t)(cfg->app.netfilter.ipset.additional_ttl /
                                         MT_DURATION_SEC);
    d->failure_stage = "prepare DNS routing rules";
    d->pipeline = mt_dns_pipeline_create(d->cache, additional_ttl, on_match, d);
    if (d->pipeline == NULL) {
        MT_ERROR("failed to create DNS pipeline");
        return err != MT_OK ? err : MT_ERR_SYS;
    }
    mt_ruleset_snapshot_t *snapshot = mt_ruleset_snapshot_build(cfg);
    if (!snapshot) { return MT_ERR_NOMEM; }
    mt_dns_pipeline_set_snapshot(d->pipeline, snapshot);

    int cleanup_timer = 0;
    if (mt_loop_add_timer(d->loop, MT_CACHE_CLEANUP_INTERVAL_MS,
                          MT_CACHE_CLEANUP_INTERVAL_MS,
                          on_cache_cleanup_timer, d,
                          &cleanup_timer) != MT_OK) {
        MT_ERROR("failed to schedule cache cleanup");
        return err != MT_OK ? err : MT_ERR_SYS;
    }

    /* ---- netfilter: iptables engines, chain patches, cleanup ---------- */
    d->failure_stage = "prepare routing tables";

    if (!cfg->app.netfilter.disable_ipv4) {
        mt_ipt_executable_t *exe = mt_ipt_executable_real_new(MT_IPT_PROTO_IPV4);
        if (!exe) {
            MT_ERROR("failed to create iptables executable (ipv4)");
            return err != MT_OK ? err : MT_ERR_SYS;
        }
        d->ipt4 = mt_ipt_new(exe);
        if (!d->ipt4) {
            mt_ipt_executable_free(exe);
            MT_ERROR("netfilter helper init fail: out of memory");
            return err != MT_OK ? err : MT_ERR_SYS;
        }
    }
    if (!cfg->app.netfilter.disable_ipv6) {
        mt_ipt_executable_t *exe = mt_ipt_executable_real_new(MT_IPT_PROTO_IPV6);
        if (!exe) {
            MT_ERROR("failed to create iptables executable (ipv6)");
            return err != MT_OK ? err : MT_ERR_SYS;
        }
        d->ipt6 = mt_ipt_new(exe);
        if (!d->ipt6) {
            mt_ipt_executable_free(exe);
            MT_ERROR("netfilter helper init fail: out of memory");
            return err != MT_OK ? err : MT_ERR_SYS;
        }
    }

    err = mt_netfilter_register_base_chains(d->ipt4, d->ipt6);
    if (err != MT_OK) {
        MT_ERROR("failed to register chain patches: %s", mt_err_str(err));
        return err != MT_OK ? err : MT_ERR_SYS;
    }

    err = mt_netfilter_clean_iptables(d->ipt4, d->ipt6,
                                      cfg->app.netfilter.iptables.chain_prefix);
    if (err != MT_OK) {
        MT_ERROR("failed to clear iptables: %s", mt_err_str(err));
        return err != MT_OK ? err : MT_ERR_SYS;
    }

    d->rtnl = mt_rtnl_open();
    if (d->rtnl == NULL) {
        MT_ERROR("failed to open rtnetlink");
        return err != MT_OK ? err : MT_ERR_SYS;
    }

    if (mt_nl_watcher_create_with_routes(d->loop, on_link_up, d,
                                          on_addr_change, d, on_route_change, d,
                                          on_netlink_resync, d, &d->watcher) != MT_OK) {
        MT_ERROR("failed to subscribe to link/address/route updates");
        return err != MT_OK ? err : MT_ERR_SYS;
    }

    /* ---- DNS proxy ------------------------------------------------------ */
    d->failure_stage = "prepare DNS transport";

    mt_dnsproxy_config_t pcfg = {
        .listen_addr = cfg->app.dns_proxy.host.address,
        .listen_port = cfg->app.dns_proxy.host.port ? cfg->app.dns_proxy.host.port : restore_ports ? restore_ports->dns : 0,
        .upstream_addr = cfg->app.dns_proxy.upstream.address,
        .upstream_port = cfg->app.dns_proxy.upstream.port,
        .timeout_ms = (uint32_t)(cfg->app.dns_proxy.timeout / MT_DURATION_MS),
        .max_concurrent = (uint32_t)cfg->app.dns_proxy.max_concurrent,
        .max_idle_conns = (uint32_t)cfg->app.dns_proxy.max_idle_conns,
        .disable_fake_ptr = cfg->app.dns_proxy.disable_fake_ptr,
        .disable_drop_aaaa = cfg->app.dns_proxy.disable_drop_aaaa,
    };
    if (pcfg.max_concurrent == 0) {
        pcfg.max_concurrent = 100;
    }
    if (pcfg.timeout_ms == 0) {
        pcfg.timeout_ms = 5000;
    }

    d->query_log = mt_dns_query_log_create();
    if (!d->query_log) {
        /* Optional diagnostics must not turn low RAM into a DNS outage. */
        MT_WARN("DNS history unavailable (out of memory); DNS forwarding will continue");
    }
    if (prepared_upstream) { d->upstream = *prepared_upstream; }
    else { err = mt_dnsproxy_resolve_upstream(pcfg.upstream_addr, pcfg.upstream_port, &d->upstream); }
    if (err == MT_OK) { err = mt_dnsproxy_create_resolved(&pcfg, d->loop, on_response, d, &d->upstream, &d->proxy); }
    if (err != MT_OK) { MT_ERROR("failed to create DNS proxy: %s", mt_err_str(err)); return err; }
    mt_dnsproxy_set_query_log(d->proxy, d->query_log);
    d->failure_stage = "bind DNS listener";
    err = mt_dnsproxy_start(d->proxy);
    if (err != MT_OK) {
        MT_ERROR("failed to start DNS proxy: %s", mt_err_str(err));
        return err != MT_OK ? err : MT_ERR_SYS;
    }

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    if (mt_loop_add_signals(d->loop, &set, on_signal, d) != MT_OK) {
        MT_ERROR("failed to subscribe to signals");
        return err != MT_OK ? err : MT_ERR_SYS;
    }

    /* ---- port 53 DNAT remap ---------------------------------------------- */
    d->failure_stage = "configure DNS interception";

    if (!cfg->app.dns_proxy.disable_remap53) {
        mt_remap_addr_t *addrs = NULL;
        size_t n_addrs = 0;
        err = collect_link_addrs(cfg->app.link, cfg->app.n_link, &addrs, &n_addrs);
        if (err != MT_OK) {
        return err != MT_OK ? err : MT_ERR_SYS;
        }
        d->port_remap =
            mt_port_remap_new(cfg->app.netfilter.iptables.chain_prefix, 53,
                              mt_dnsproxy_listen_port(d->proxy), addrs, n_addrs, d->ipt4, d->ipt6);
        free(addrs);
        if (d->port_remap == NULL) {
            MT_ERROR("failed to override DNS: out of memory");
            return err != MT_OK ? err : MT_ERR_SYS;
        }
#ifndef MT_ENTWARE_KN
        err = mt_port_remap_enable(d->port_remap);
        if (err != MT_OK) {
            MT_ERROR("failed to override DNS: %s", mt_err_str(err));
            return err != MT_OK ? err : MT_ERR_SYS;
        }
#endif
    }

    /* ---- app layer: per-group netfilter rulesets + HTTP mutation API -------- */
    d->failure_stage = "apply routing definitions";

    mt_app_deps_t app_deps = {
        .cfg = cfg,
        .cache = d->cache,
        .pipeline = d->pipeline,
        .ipt4 = d->ipt4,
        .ipt6 = d->ipt6,
        .rtnl = d->rtnl,
        .start_idx = cfg->app.netfilter.start_mark_table_index,
    };
    d->app = mt_app_create(&app_deps);
    if (d->app == NULL) {
        MT_ERROR("failed to create app");
        return err != MT_OK ? err : MT_ERR_SYS;
    }
#ifdef MT_ENTWARE_KN
    d->interface_states = mt_kn_ifstate_new(on_ndm_state_changed, d);
    if (!d->interface_states) {
        MT_ERROR("failed to allocate NDM interface state");
        return err != MT_OK ? err : MT_ERR_SYS;
    }
    mt_app_set_interface_states(d->app, d->interface_states);
    /* Attach the unresolved (closed) policy scope before the first DNAT rule.
     * There must be no all-client DNS interception window on a saved boot. */
    mt_app_set_port_remap(d->app, d->port_remap);
    if (d->port_remap && (err = mt_port_remap_enable(d->port_remap)) != MT_OK) {
        MT_ERROR("failed to enable scoped DNS remap: %s", mt_err_str(err));
        return err != MT_OK ? err : MT_ERR_SYS;
    }
    if (mt_kn_policy_service_create(d->loop, d->app, config_path, MT_VERSION, NULL, &d->policy_service) != MT_OK) {
        MT_ERROR("failed to create Keenetic policy service");
        return err != MT_OK ? err : MT_ERR_SYS;
    }
#endif
    for (size_t i = 0; i < mt_app_user_group_count(d->app); i++) {
        mt_ruleset_t *rs = mt_app_user_group_at(d->app, i);
        err = mt_ruleset_enable(rs);
        if (err != MT_OK) {
            MT_ERROR("failed to enable group: %s", mt_err_str(err));
            return err != MT_OK ? err : MT_ERR_SYS;
        }
        err = mt_ruleset_sync(rs, d->cache, now_unix());
        if (err != MT_OK) {
            MT_ERROR("failed to sync group: %s", mt_err_str(err));
            return err != MT_OK ? err : MT_ERR_SYS;
        }
    }
    for (size_t i = 0; i < mt_app_subscription_ruleset_count(d->app); i++) {
        mt_ruleset_t *rs = mt_app_subscription_ruleset_at(d->app, i);
        err = mt_ruleset_enable(rs);
        if (err != MT_OK) {
            MT_ERROR("failed to enable subscription: %s", mt_err_str(err));
            return err != MT_OK ? err : MT_ERR_SYS;
        }
        err = mt_ruleset_sync(rs, d->cache, now_unix());
        if (err != MT_OK) {
            MT_ERROR("failed to sync subscription: %s", mt_err_str(err));
            return err != MT_OK ? err : MT_ERR_SYS;
        }
    }
    /* Only now mark the app "running" -- matches Go's Start() CAS
     * happening before this same enable/sync loop, so a group added later
     * via the HTTP API (mt_app_add_group) gets its own immediate
     * enable+sync, while the initial set was already brought up above. */
    mt_app_set_running(d->app, true);

    /* Signals were blocked above; worker threads inherit that mask. */
    if (mt_sub_fetcher_create(d->loop, &d->fetcher) != MT_OK) {
        MT_ERROR("failed to create subscription fetch workers");
        return err != MT_OK ? err : MT_ERR_SYS;
    }

    /* ---- subscription auto-update: 1-minute tick, fires once now too ------- */

    int sub_auto_update_timer = 0;
    if (mt_loop_add_timer(d->loop, 0, MT_SUBSCRIPTION_AUTO_UPDATE_INTERVAL_MS,
                          on_subscription_auto_update_timer, d,
                          &sub_auto_update_timer) != MT_OK) {
        MT_ERROR("failed to schedule subscription auto-update");
        return err != MT_OK ? err : MT_ERR_SYS;
    }

    /* Signals are already blocked. Device discovery is optional, read-only,
     * and off-loop; failure falls back to addresses instead of stopping DNS. */
    if (mt_dns_clients_create(d->loop, NULL, NULL, &d->dns_clients) != MT_OK) {
        MT_WARN("device names unavailable; DNS journal will use IP addresses");
    }

    /* ---- HTTP API: Unix socket (always on) + TCP WebUI (config-gated) ------ */
    d->failure_stage = "bind local administrative API";

    d->groups_ctx = (mt_groups_ctx_t){
        .app = d->app,
        .config_path = config_path,
        .config_version = MT_VERSION,
    };
    d->system_ctx = (mt_system_ctx_t){
        .app = d->app,
        .settings = settings,
        .query_log = d->query_log,
        .dns_clients = d->dns_clients,
        .config_path = config_path,
        .config_version = MT_VERSION,
    };
    d->subs_ctx = (mt_subs_ctx_t){
        .app = d->app,
        .fetcher = d->fetcher,
        .config_path = config_path,
        .config_version = MT_VERSION,
    };
    d->auth_ctx = (mt_auth_ctx_t){
        .enabled = auth_enabled_fn,
        .state_dir = auth_state_dir_fn,
        .ud = cfg,
    };

    if (mt_httpd_create(d->loop, &d->http_unix) != MT_OK) {
        MT_ERROR("failed to create Unix socket server");
        return err != MT_OK ? err : MT_ERR_SYS;
    }
    mt_groups_register_routes(d->http_unix, &d->groups_ctx);
    mt_system_register_routes(d->http_unix, &d->system_ctx);
#ifdef MT_ENTWARE_KN
    if (mt_kn_policy_register_routes(d->http_unix, d->policy_service) != MT_OK) {
        MT_ERROR("failed to register Keenetic policy routes");
        return err != MT_OK ? err : MT_ERR_SYS;
    }
    if (d->dns_clients && mt_dns_keenetic_register_hook(d->http_unix, d->dns_clients) != MT_OK) {
        MT_WARN("device-name notifications unavailable; periodic refresh remains active");
    }
#endif
    mt_subs_register_routes(d->http_unix, &d->subs_ctx);
    mt_httpd_route(d->http_unix, "GET", "/api/v1/auth", mt_auth_status_handler, &d->auth_ctx);
    mt_httpd_route(d->http_unix, "POST", "/api/v1/auth", mt_auth_login_handler, &d->auth_ctx);
    err = mt_httpd_listen_unix(d->http_unix, MT_SOCK_PATH);
    if (err != MT_OK) {
        MT_ERROR("failed to listen on Unix socket %s: %s", MT_SOCK_PATH, mt_err_str(err));
        return err != MT_OK ? err : MT_ERR_SYS;
    }
    MT_INFO("Unix socket API listening on %s", MT_SOCK_PATH);
#ifdef MT_ENTWARE_KN
    if (mt_kn_ifstate_service_create(d->loop, d->interface_states, MT_KN_IFSTATE_LOST_PATH,
                                      NULL, &d->interface_service) != MT_OK ||
        mt_kn_ifstate_register_hook(d->http_unix, d->interface_service) != MT_OK) {
        MT_ERROR("failed to start NDM interface notifications");
        return err != MT_OK ? err : MT_ERR_SYS;
    }
#endif

#ifdef MT_ENTWARE_KN
    d->system_ctx.interface_service = d->interface_service;
#endif

    d->static_ctx = (mt_static_ctx_t){
        .skins_dir = MT_APP_SHARE_DIR "/skins",
        .skin_name = static_skin_fn,
        .ud = cfg,
    };
    if (cfg->app.http_web.enabled) {
        d->failure_stage = "bind web interface";
        err = daemon_open_web(d, &cfg->app,
                              restore_ports ? restore_ports->http : 0, &d->http_tcp);
        if (err != MT_OK) {
            MT_ERROR("failed to listen HTTP %s:%u: %s", cfg->app.http_web.host.address,
                     cfg->app.http_web.host.port, mt_err_str(err));
            return err;
        }
        MT_INFO("HTTP WebUI listening on %s:%u", cfg->app.http_web.host.address,
                cfg->app.http_web.host.port);
    } else {
        MT_INFO("HTTP WebUI disabled by configuration");
    }

    /* Started last, once everything a rebuild reaches is up: the port
     * remap, every group's chains, and the API that can change them. On
     * builds without a committer this is a no-op and the netfilter.d hook
     * keeps committing in place. */
    mt_app_set_port_remap(d->app, d->port_remap);
    d->failure_stage = "start routing reconciliation";
    err = mt_app_start_netfilter_committer(d->app);
    if (err != MT_OK) {
        MT_ERROR("failed to start netfilter committer: %s", mt_err_str(err));
        return err != MT_OK ? err : MT_ERR_SYS;
    }

    d->ports = (struct bound_ports){mt_httpd_tcp_port(d->http_tcp), mt_dnsproxy_listen_port(d->proxy)};
    mt_settings_bound_ports(settings, d->ports.http, d->ports.dns);
    mt_settings_set_live_apply(settings, daemon_apply_live_settings, d);
    mt_settings_runtime_started(settings);
    d->failure_stage = NULL;
    MT_INFO("service started");
    return MT_OK;
}

/* If even the old routing runtime cannot be restored, keep administration
 * available for diagnosis/retry. It uses the old authentication policy and
 * exposes no model-mutating handlers backed by a missing app. */
static mt_err_t daemon_start_recovery(struct daemon *d, mt_config_t *cfg,
                                       const char *config_path, mt_settings_t *settings,
                                       mt_cache_t *cache, const mt_dns_upstream_t *upstream,
                                       const struct bound_ports *ports)
{
    memset(d, 0, sizeof(*d));
    d->cfg = cfg; d->config_path = config_path; d->settings = settings; d->cache = cache;
    mt_update_resume();
    d->upstream = *upstream;
    d->ports = *ports;
    mt_settings_bound_ports(settings, ports->http, ports->dns);
    mt_err_t err = mt_loop_create(&d->loop);
    if (err != MT_OK) { return err; }
    mt_settings_attach_loop(settings, d->loop);
    d->auth_ctx = (mt_auth_ctx_t){.enabled = auth_enabled_fn, .state_dir = auth_state_dir_fn, .ud = cfg};
    d->static_ctx = (mt_static_ctx_t){.skins_dir = MT_APP_SHARE_DIR "/skins", .skin_name = static_skin_fn, .ud = cfg};
    sigset_t signals;
    sigemptyset(&signals); sigaddset(&signals, SIGINT); sigaddset(&signals, SIGTERM);
    if ((err = mt_loop_add_signals(d->loop, &signals, on_signal, d)) != MT_OK) { return err; }
    if ((err = mt_httpd_create(d->loop, &d->http_unix)) != MT_OK) { return err; }
    mt_settings_register_routes(d->http_unix, settings);
    mt_httpd_route(d->http_unix, "GET", "/api/v1/auth", mt_auth_status_handler, &d->auth_ctx);
    mt_httpd_route(d->http_unix, "POST", "/api/v1/auth", mt_auth_login_handler, &d->auth_ctx);
    if ((err = mt_httpd_listen_unix(d->http_unix, MT_SOCK_PATH)) != MT_OK) { return err; }
    if (cfg->app.http_web.enabled && mt_httpd_create(d->loop, &d->http_tcp) == MT_OK) {
        mt_settings_register_routes(d->http_tcp, settings);
        mt_httpd_route(d->http_tcp, "GET", "/api/v1/auth", mt_auth_status_handler, &d->auth_ctx);
        mt_httpd_route(d->http_tcp, "POST", "/api/v1/auth", mt_auth_login_handler, &d->auth_ctx);
        mt_httpd_set_middleware(d->http_tcp, mt_auth_middleware, &d->auth_ctx);
        mt_httpd_set_not_found(d->http_tcp, mt_static_handler, &d->static_ctx);
        if (mt_httpd_listen_tcp(d->http_tcp, cfg->app.http_web.host.address,
                              cfg->app.http_web.host.port ? cfg->app.http_web.host.port : ports->http) != MT_OK) {
            mt_httpd_destroy(d->http_tcp); d->http_tcp = NULL;
        }
    }
    MT_ERROR("routing recovery failed; settings remain available through the administrative API");
    return MT_OK;
}

/* The loop has stopped after an acknowledged and prepared request. Only the
 * app projection moves; the latest groups/subscriptions/profiles stay owned by
 * cfg through success and rollback. No worker from the old graph survives stop. */
static mt_err_t daemon_apply_settings(struct daemon *d, mt_config_t *cfg,
                                      const char *path, mt_settings_t *settings,
                                      mt_cache_t *cache)
{
    mt_app_config_t next = {0};
    mt_dns_upstream_t upstream;
    if (!mt_settings_take(settings, &next, &upstream)) { return MT_ERR_INVAL; }
    mt_app_config_t previous = cfg->app;
    mt_dns_upstream_t previous_upstream = d->upstream;
    struct bound_ports previous_ports = d->ports;
    mt_err_t err = daemon_teardown(d);
    const char *stage = "stop previous routing";
    cfg->app = next;
    if (err == MT_OK) { err = daemon_start(d, cfg, path, settings, cache, &upstream, NULL); stage = d->failure_stage; }
    if (err == MT_OK) { stage = "save configuration file"; err = mt_config_save_file(cfg, MT_VERSION, path); }
    if (err == MT_OK) {
        mt_app_config_clear(&previous);
        mt_settings_finish(settings, NULL, false);
        return MT_OK;
    }
    char error[256];
    snprintf(error, sizeof(error), "Cannot %s (%s); previous settings restored", stage ? stage : "apply settings", mt_err_str(err));
    mt_err_t cleanup = daemon_teardown(d);
    mt_app_config_clear(&cfg->app); cfg->app = previous;
    mt_err_t rollback = daemon_start(d, cfg, path, settings, cache, &previous_upstream, &previous_ports);
    if (rollback == MT_OK && cleanup == MT_OK) {
        mt_settings_finish(settings, error, false);
        return MT_OK;
    }
    /* A cleanup failure can leave candidate kernel resources behind even
     * when the old listeners bind. Report it, never claim atomic rollback. */
    snprintf(error, sizeof(error), "Cannot apply settings (%s); routing recovery failed (%s)",
             mt_err_str(err), mt_err_str(rollback != MT_OK ? rollback : cleanup));
    mt_settings_finish(settings, error, true);
    if (rollback == MT_OK) { return MT_OK; }
    (void)daemon_teardown(d);
    return daemon_start_recovery(d, cfg, path, settings, cache, &previous_upstream, &previous_ports);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--version")) { printf("%s\n", MT_VERSION); return 0; }
    const char *path = MT_CONFIG_PATH;
    if (argc > 2 && !strcmp(argv[1], "--config")) { path = argv[2]; }
    MT_INFO("starting MagiTrickle daemon (C) version=%s", MT_VERSION);
    mt_sub_fetch_global_init();
    mt_config_t cfg;
    mt_err_t err = mt_config_init_defaults(&cfg);
    if (err != MT_OK) { mt_sub_fetch_global_cleanup(); return 1; }
    err = mt_config_load_file(&cfg, path);
    if (err == MT_ERR_NOENT) { MT_INFO("config file %s not found, using defaults", path); }
    else if (err != MT_OK) {
        MT_ERROR("failed to load config %s: %s", path, mt_err_str(err));
        mt_config_clear(&cfg); mt_sub_fetch_global_cleanup(); return 1;
    }
    mt_cache_t *cache = mt_cache_create(0);
    mt_settings_t *settings = mt_settings_create(&cfg);
    struct daemon d = {0};
    if (!cache || !settings) { err = MT_ERR_NOMEM; }
    else { err = daemon_start(&d, &cfg, path, settings, cache, NULL, NULL); }
    while (err == MT_OK) {
        err = mt_loop_run(d.loop);
        if (err != MT_OK || d.shutdown_requested || !mt_settings_ready(settings)) { break; }
        err = daemon_apply_settings(&d, &cfg, path, settings, cache);
    }
    if (err != MT_OK) { MT_ERROR("service failed: %s", mt_err_str(err)); }
    (void)daemon_teardown(&d);
    mt_settings_destroy(settings);
    mt_cache_destroy(cache);
    mt_config_clear(&cfg);
    mt_sub_fetch_global_cleanup();
    MT_INFO("service stopped");
    return err == MT_OK ? 0 : 1;
}
