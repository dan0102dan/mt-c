#include "magitrickle/netlink_watcher.h"
#include "magitrickle/log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <libmnl/libmnl.h>
#include <linux/if.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>

#define MT_NL_WATCHER_RECVBUF 8192
#define MT_NL_RECOVERY_MAX_MS 30000u

/* IFLA_CARRIER is attribute 33 in the Linux rtnetlink wire ABI. Some
 * older Entware SDK headers omit its enum name even though the running
 * kernel may report it. Keep this trace-only attribute header-independent;
 * routing and link-state callbacks continue to use ifi_flags. */
enum { MT_NL_IFLA_CARRIER = 33 };

struct mt_nl_watcher {
    struct mnl_socket *nl;
    mt_loop_t *loop;
    mt_nl_link_cb link_cb;
    void *link_ud;
    mt_nl_addr_cb addr_cb;
    void *addr_ud;
    mt_nl_change_cb route_cb;
    void *route_ud;
    mt_nl_change_cb resync_cb;
    void *resync_ud;
    int reconnect_timer;
    unsigned reconnect_delay_ms;
};

static void notify_resync(mt_nl_watcher_t *w)
{
    if (w->resync_cb) { w->resync_cb(w->resync_ud); }
}

/* These are attributes of *this* notification, not a persisted state
 * transition. None of them is used for routing or callback selection. */
typedef struct link_log_attributes {
    char name[IFNAMSIZ];
    uint8_t operstate, carrier;
    uint32_t mtu;
    bool have_operstate, have_carrier, have_mtu, wireless_attr;
} link_log_attributes_t;

static int link_attributes_cb(const struct nlattr *attr, void *ud)
{
    link_log_attributes_t *a = ud;
    switch (mnl_attr_get_type(attr)) {
    case IFLA_IFNAME:
        if (mnl_attr_validate(attr, MNL_TYPE_NUL_STRING) != 0 ||
            mnl_attr_get_payload_len(attr) > IFNAMSIZ) { return MNL_CB_ERROR; }
        snprintf(a->name, sizeof(a->name), "%s", mnl_attr_get_str(attr));
        break;
    case IFLA_OPERSTATE:
        if (mnl_attr_validate(attr, MNL_TYPE_U8) == 0) {
            a->operstate = mnl_attr_get_u8(attr); a->have_operstate = true;
        }
        break;
    case MT_NL_IFLA_CARRIER:
        if (mnl_attr_validate(attr, MNL_TYPE_U8) == 0) {
            a->carrier = mnl_attr_get_u8(attr); a->have_carrier = true;
        }
        break;
    case IFLA_MTU:
        if (mnl_attr_validate(attr, MNL_TYPE_U32) == 0) {
            a->mtu = mnl_attr_get_u32(attr); a->have_mtu = true;
        }
        break;
    case IFLA_WIRELESS:
        a->wireless_attr = true;
        break;
    default:
        break;
    }
    return MNL_CB_OK;
}

static const char *operstate_name(uint8_t state)
{
    /* Linux IF_OPER_* values. Unknown values remain distinguishable. */
    switch (state) {
    case 0: return "unknown";
    case 1: return "not-present";
    case 2: return "down";
    case 3: return "lower-layer-down";
    case 4: return "testing";
    case 5: return "dormant";
    case 6: return "up";
    default: return "unrecognized";
    }
}

static bool handle_link_msg(mt_nl_watcher_t *w, const struct nlmsghdr *h)
{
    if (mnl_nlmsg_get_payload_len(h) < sizeof(struct ifinfomsg)) { return false; }
    const struct ifinfomsg *ifi = mnl_nlmsg_get_payload(h);
    bool up = h->nlmsg_type != RTM_DELLINK && (ifi->ifi_flags & IFF_UP) != 0;
    link_log_attributes_t a = {0};
    if (mnl_attr_parse(h, sizeof(*ifi), link_attributes_cb, &a) < 0) { return false; }
    if (!a.name[0]) { return false; }
    if (mt_log_level() <= MT_LOG_TRACE) {
        char mtu[20] = "unreported";
        if (a.have_mtu) { snprintf(mtu, sizeof(mtu), "%u", a.mtu); }
        MT_TRACE("netlink link notification: action=%s interface=%s index=%d "
                 "admin=%s oper=%s carrier=%s mtu=%s wireless_attr=%s "
                 "flags=0x%x change_mask=0x%x",
                 h->nlmsg_type == RTM_DELLINK ? "removed" : "updated",
                 a.name, ifi->ifi_index, up ? "up" : "down",
                 a.have_operstate ? operstate_name(a.operstate) : "unreported",
                 a.have_carrier ? (a.carrier ? "up" : "down") : "unreported",
                 mtu, a.wireless_attr ? "present" : "absent",
                 (unsigned)ifi->ifi_flags, (unsigned)ifi->ifi_change);
    }
    /* Callbacks run for every notification, including identical flags. */
    if (w->link_cb) { w->link_cb(a.name, up, w->link_ud); }
    return true;
}

static bool handle_addr_msg(mt_nl_watcher_t *w, const struct nlmsghdr *h)
{
    if (mnl_nlmsg_get_payload_len(h) < sizeof(struct ifaddrmsg)) { return false; }
    const struct ifaddrmsg *ifa = mnl_nlmsg_get_payload(h);
    char name[IFNAMSIZ] = {0};
    if (!if_indextoname(ifa->ifa_index, name)) {
        /* A DELADDR can race with DELLINK. Name lookup is not authoritative. */
        return false;
    }
    if (mt_log_level() <= MT_LOG_TRACE) {
        MT_TRACE("netlink address notification: action=%s interface=%s index=%u "
                 "family=%u prefix=%u scope=%u flags=0x%x",
                 h->nlmsg_type == RTM_DELADDR ? "removed" : "updated",
                 name, ifa->ifa_index, ifa->ifa_family, ifa->ifa_prefixlen,
                 ifa->ifa_scope, (unsigned)ifa->ifa_flags);
    }
    if (w->addr_cb) { w->addr_cb(name, w->addr_ud); }
    return true;
}

static void watcher_drop_socket(mt_nl_watcher_t *w)
{
    if (!w->nl) { return; }
    (void)mt_loop_del_fd(w->loop, mnl_socket_get_fd(w->nl));
    mnl_socket_close(w->nl);
    w->nl = NULL;
}

static void on_readable(mt_loop_t *loop, int fd, uint32_t events, void *ud);

static mt_err_t watcher_open_socket(mt_nl_watcher_t *w)
{
    struct mnl_socket *nl = mnl_socket_open(NETLINK_ROUTE);
    if (!nl) { return mt_err_from_errno(errno); }
    unsigned int groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR |
                          RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE;
    if (mnl_socket_bind(nl, groups, MNL_SOCKET_AUTOPID) < 0) {
        mt_err_t err = mt_err_from_errno(errno);
        mnl_socket_close(nl);
        return err;
    }
    int fd = mnl_socket_get_fd(nl);
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        mt_err_t err = mt_err_from_errno(errno);
        mnl_socket_close(nl);
        return err;
    }
    mt_err_t err = mt_loop_add_fd(w->loop, fd, EPOLLIN | EPOLLERR | EPOLLHUP, on_readable, w);
    if (err != MT_OK) {
        mnl_socket_close(nl);
        return err;
    }
    w->nl = nl;
    return MT_OK;
}

static void on_reconnect(mt_loop_t *loop, void *ud);

static void watcher_schedule_reconnect(mt_nl_watcher_t *w)
{
    if (w->reconnect_timer) { return; }
    unsigned delay = w->reconnect_delay_ms ? w->reconnect_delay_ms : 100u;
    if (delay > MT_NL_RECOVERY_MAX_MS) { delay = MT_NL_RECOVERY_MAX_MS; }
    if (mt_loop_add_timer(w->loop, delay, 0, on_reconnect, w, &w->reconnect_timer) != MT_OK) {
        MT_ERROR("cannot schedule netlink watcher recovery; stopping daemon");
        mt_loop_stop(w->loop);
        return;
    }
    w->reconnect_delay_ms = delay >= MT_NL_RECOVERY_MAX_MS / 2u ?
                            MT_NL_RECOVERY_MAX_MS : delay * 2u;
}

static void on_reconnect(mt_loop_t *loop, void *ud)
{
    (void)loop;
    mt_nl_watcher_t *w = ud;
    w->reconnect_timer = 0;
    mt_err_t err = watcher_open_socket(w);
    if (err != MT_OK) {
        MT_WARN("netlink watcher reconnect failed: %s", mt_err_str(err));
        watcher_schedule_reconnect(w);
        return;
    }
    w->reconnect_delay_ms = 0;
    notify_resync(w); /* State may have changed while the socket was absent. */
}

static void watcher_socket_failure(mt_nl_watcher_t *w)
{
    watcher_drop_socket(w);
    notify_resync(w);
    watcher_schedule_reconnect(w);
}

static void on_readable(mt_loop_t *loop, int fd, uint32_t events, void *ud)
{
    (void)loop;
    (void)fd;
    mt_nl_watcher_t *w = ud;
    if (events & (EPOLLERR | EPOLLHUP)) {
        watcher_socket_failure(w);
        return;
    }
    uint8_t buf[MT_NL_WATCHER_RECVBUF];
    for (;;) {
        ssize_t ret = mnl_socket_recvfrom(w->nl, buf, sizeof(buf));
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { return; }
            if (errno == EINTR) { continue; }
            MT_WARN("netlink watcher recv error: %s", strerror(errno));
            if (errno == ENOBUFS) {
                notify_resync(w);
                continue;
            }
            watcher_socket_failure(w);
            return;
        }
        if (ret == 0) {
            watcher_socket_failure(w);
            return;
        }
        int len = (int)ret;
        struct nlmsghdr *nh = (struct nlmsghdr *)buf;
        while (mnl_nlmsg_ok(nh, len)) {
            switch (nh->nlmsg_type) {
            case RTM_NEWLINK:
            case RTM_DELLINK:
                if (!handle_link_msg(w, nh)) { notify_resync(w); }
                break;
            case RTM_NEWADDR:
            case RTM_DELADDR:
                if (!handle_addr_msg(w, nh)) { notify_resync(w); }
                break;
            case RTM_NEWROUTE:
            case RTM_DELROUTE:
                if (mnl_nlmsg_get_payload_len(nh) < sizeof(struct rtmsg)) {
                    notify_resync(w);
                } else if (w->route_cb) {
                    w->route_cb(w->route_ud);
                }
                break;
            case NLMSG_ERROR:
                if (mnl_nlmsg_get_payload_len(nh) < sizeof(struct nlmsgerr) ||
                    ((const struct nlmsgerr *)mnl_nlmsg_get_payload(nh))->error != 0) {
                    notify_resync(w);
                }
                break;
            case NLMSG_OVERRUN:
                notify_resync(w);
                break;
            default:
                break;
            }
            nh = mnl_nlmsg_next(nh, &len);
        }
        if (len != 0) { notify_resync(w); }
    }
}

mt_err_t mt_nl_watcher_create_with_routes(mt_loop_t *loop, mt_nl_link_cb link_cb,
                                          void *link_ud, mt_nl_addr_cb addr_cb,
                                          void *addr_ud, mt_nl_change_cb route_cb,
                                          void *route_ud, mt_nl_change_cb resync_cb,
                                          void *resync_ud, mt_nl_watcher_t **out)
{
    if (!loop || !out) { return MT_ERR_INVAL; }
    *out = NULL;
    mt_nl_watcher_t *w = calloc(1, sizeof(*w));
    if (!w) { return MT_ERR_NOMEM; }
    w->loop = loop;
    w->link_cb = link_cb;
    w->link_ud = link_ud;
    w->addr_cb = addr_cb;
    w->addr_ud = addr_ud;
    w->route_cb = route_cb;
    w->route_ud = route_ud;
    w->resync_cb = resync_cb;
    w->resync_ud = resync_ud;
    mt_err_t err = watcher_open_socket(w);
    if (err != MT_OK) {
        free(w);
        return err;
    }
    *out = w;
    return MT_OK;
}

mt_err_t mt_nl_watcher_create(mt_loop_t *loop, mt_nl_link_cb link_cb, void *link_ud,
                              mt_nl_addr_cb addr_cb, void *addr_ud, mt_nl_watcher_t **out)
{
    return mt_nl_watcher_create_with_routes(loop, link_cb, link_ud, addr_cb, addr_ud,
                                           NULL, NULL, NULL, NULL, out);
}

void mt_nl_watcher_destroy(mt_nl_watcher_t *w)
{
    if (!w) { return; }
    if (w->reconnect_timer) { (void)mt_loop_del_timer(w->loop, w->reconnect_timer); }
    watcher_drop_socket(w);
    free(w);
}
