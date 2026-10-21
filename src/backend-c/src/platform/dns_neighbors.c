/* Bounded, read-only neighbor discovery on the existing metadata worker. */
#include "magitrickle/dns_neighbors.h"
#include <arpa/inet.h>
#include <errno.h>
#include <linux/neighbour.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Older Entware headers may not name this flag, but the kernel can send it. */
#ifndef NLM_F_DUMP_INTR
#define NLM_F_DUMP_INTR 0x10
#endif

static size_t align4(size_t n) { return (n + 3u) & ~(size_t)3u; }

static mt_err_t append_neighbor(mt_dns_neighbors_t *t, const unsigned char *data, size_t len)
{
    struct ndmsg nd;
    if (len < sizeof(nd)) { return MT_ERR_PROTO; }
    memcpy(&nd, data, sizeof(nd));
    if (nd.ndm_family != AF_INET6 || nd.ndm_ifindex <= 0 || nd.ndm_type != RTN_UNICAST ||
        (nd.ndm_flags & NTF_PROXY)) { return MT_OK; }
    const unsigned usable = NUD_REACHABLE | NUD_STALE | NUD_DELAY | NUD_PROBE | NUD_PERMANENT | NUD_NOARP;
    if (!(nd.ndm_state & usable) || (nd.ndm_state & (NUD_FAILED | NUD_INCOMPLETE))) { return MT_OK; }
    struct in6_addr address = {0};
    unsigned char mac[6] = {0};
    bool have_ip = false, have_mac = false, seen_mac = false;
    size_t offset = align4(sizeof(nd));
    while (offset < len) {
        struct rtattr attr;
        if (len - offset < sizeof(attr)) { return MT_ERR_PROTO; }
        memcpy(&attr, data + offset, sizeof(attr));
        size_t size = attr.rta_len;
        if (size < sizeof(attr) || size > len - offset) { return MT_ERR_PROTO; }
        const unsigned char *value = data + offset + sizeof(attr);
        if (attr.rta_type == NDA_DST) {
            if (have_ip || size - sizeof(attr) != sizeof(address)) { return MT_ERR_PROTO; }
            memcpy(&address, value, sizeof(address)); have_ip = true;
        } else if (attr.rta_type == NDA_LLADDR) {
            if (seen_mac) { return MT_ERR_PROTO; }
            seen_mac = true;
            /* Other link types can legitimately have non-Ethernet addresses.
             * Ignore that neighbor, not the useful LAN entries in this dump. */
            if (size - sizeof(attr) == sizeof(mac)) {
                memcpy(mac, value, sizeof(mac)); have_mac = true;
            }
        }
        size_t step = align4(size);
        if (step > len - offset) {
            if (size != len - offset) { return MT_ERR_PROTO; }
            offset = len;
        } else { offset += step; }
    }
    const unsigned char zero[6] = {0};
    if (!have_ip || !have_mac || (mac[0] & 1u) || memcmp(mac, zero, sizeof(mac)) == 0 ||
        IN6_IS_ADDR_UNSPECIFIED(&address) || IN6_IS_ADDR_MULTICAST(&address) ||
        IN6_IS_ADDR_V4MAPPED(&address)) { return MT_OK; }
    if (t->count >= MT_DNS_NEIGHBOR_LIMIT) { return MT_ERR_LIMIT; }
    if (t->count == t->capacity) {
        size_t cap = t->capacity ? t->capacity * 2 : 16;
        if (cap > MT_DNS_NEIGHBOR_LIMIT) { cap = MT_DNS_NEIGHBOR_LIMIT; }
        mt_dns_neighbor_t *items = realloc(t->items, cap * sizeof(*items));
        if (!items) { return MT_ERR_NOMEM; }
        t->items = items; t->capacity = cap;
    }
    mt_dns_neighbor_t entry = {.ifindex = nd.ndm_ifindex};
    if (!inet_ntop(AF_INET6, &address, entry.ip, sizeof(entry.ip))) { return MT_ERR_PROTO; }
    memcpy(entry.mac, mac, sizeof(mac));
    t->items[t->count++] = entry;
    return MT_OK;
}

mt_err_t mt_dns_neighbors_parse(mt_dns_neighbors_t *table, const void *buffer,
                                size_t len, uint32_t sequence, bool *done)
{
    if (!table || !buffer || !len || !done || *done) { return MT_ERR_INVAL; }
    const unsigned char *data = buffer;
    size_t offset = 0;
    while (offset < len) {
        struct nlmsghdr header;
        if (len - offset < sizeof(header)) { return MT_ERR_PROTO; }
        memcpy(&header, data + offset, sizeof(header));
        size_t size = header.nlmsg_len;
        if (size < sizeof(header) || size > len - offset) { return MT_ERR_PROTO; }
        if (header.nlmsg_seq != sequence) { return MT_ERR_PROTO; }
        if (header.nlmsg_flags & NLM_F_DUMP_INTR) { return MT_ERR_AGAIN; }
        const unsigned char *payload = data + offset + sizeof(header);
        size_t payload_len = size - sizeof(header);
        if (header.nlmsg_type == NLMSG_DONE) {
            if (payload_len) {
                int error;
                if (payload_len < sizeof(error)) { return MT_ERR_PROTO; }
                memcpy(&error, payload, sizeof(error));
                if (error) { return MT_ERR_UPSTREAM; }
            }
            *done = true;
        } else if (header.nlmsg_type == NLMSG_ERROR) {
            struct nlmsgerr error;
            if (payload_len < sizeof(error)) { return MT_ERR_PROTO; }
            memcpy(&error, payload, sizeof(error));
            if (error.error) { return MT_ERR_UPSTREAM; }
        } else if (header.nlmsg_type == NLMSG_OVERRUN) { return MT_ERR_IO; }
        else if (header.nlmsg_type == RTM_NEWNEIGH) {
            mt_err_t err = append_neighbor(table, payload, payload_len);
            if (err != MT_OK) { return err; }
        } else if (header.nlmsg_type != NLMSG_NOOP) { return MT_ERR_PROTO; }
        size_t step = align4(size);
        if (step > len - offset) {
            if (size != len - offset) { return MT_ERR_PROTO; }
            offset = len;
        } else { offset += step; }
        if (*done && offset != len) { return MT_ERR_PROTO; }
    }
    return MT_OK;
}

static int compare_ip(const void *a, const void *b)
{
    return strcmp(((const mt_dns_neighbor_t *)a)->ip, ((const mt_dns_neighbor_t *)b)->ip);
}
static int compare_mac(const void *a, const void *b)
{
    const mt_dns_neighbor_t *left = a, *right = b;
    int cmp = memcmp(left->mac, right->mac, sizeof(left->mac));
    return cmp ? cmp : strcmp(left->ip, right->ip);
}
void mt_dns_neighbors_finish(mt_dns_neighbors_t *t)
{
    if (t->count > 1) { qsort(t->items, t->count, sizeof(*t->items), compare_ip); }
    size_t n = 0;
    for (size_t i = 0; i < t->count;) {
        size_t end = i + 1;
        bool ambiguous = false;
        while (end < t->count && strcmp(t->items[i].ip, t->items[end].ip) == 0) {
            if (t->items[i].ifindex != t->items[end].ifindex ||
                memcmp(t->items[i].mac, t->items[end].mac, sizeof(t->items[i].mac))) { ambiguous = true; }
            end++;
        }
        /* DNS rows currently have no ingress ifindex: never guess the scope. */
        if (!ambiguous) { t->items[n++] = t->items[i]; }
        i = end;
    }
    t->count = n;
    if (n > 1) { qsort(t->items, n, sizeof(*t->items), compare_mac); }
}

static int64_t monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) { return -1; }
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
mt_err_t mt_dns_neighbors_read(mt_dns_neighbor_t **neighbors, size_t *count,
                               const atomic_bool *stop)
{
    if (!neighbors || !count) { return MT_ERR_INVAL; }
    *neighbors = NULL; *count = 0;
    if (stop && atomic_load(stop)) { return MT_ERR_CANCELED; }
    int64_t start = monotonic_ms();
    if (start < 0) { return MT_ERR_IO; }
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE);
    if (fd < 0) { return MT_ERR_IO; }
    struct sockaddr_nl local = {.nl_family = AF_NETLINK};
    struct sockaddr_nl kernel = {.nl_family = AF_NETLINK};
    struct { struct nlmsghdr header; struct ndmsg nd; } request = {
        .header = {.nlmsg_len = sizeof(request), .nlmsg_type = RTM_GETNEIGH,
                   .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP, .nlmsg_seq = 1},
        .nd = {.ndm_family = AF_INET6},
    };
    mt_dns_neighbors_t table = {0};
    mt_err_t err = MT_ERR_IO;
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0 ||
        sendto(fd, &request, sizeof(request), 0, (struct sockaddr *)&kernel, sizeof(kernel)) != (ssize_t)sizeof(request)) { goto out; }
    bool done = false;
    size_t received = 0;
    while (!done) {
        if (stop && atomic_load(stop)) { err = MT_ERR_CANCELED; goto out; }
        int64_t now = monotonic_ms();
        if (now < 0) { err = MT_ERR_IO; goto out; }
        if (now - start >= 500) { err = MT_ERR_TIMEOUT; goto out; }
        struct pollfd pollfd = {.fd = fd, .events = POLLIN};
        int wait = (int)(500 - (now - start));
        if (wait > 50) { wait = 50; }
        int ready = poll(&pollfd, 1, wait);
        if (ready < 0 && errno == EINTR) { continue; }
        if (ready < 0 || (pollfd.revents & (POLLERR | POLLHUP | POLLNVAL))) { err = MT_ERR_IO; goto out; }
        if (!ready) { continue; }
        unsigned char data[16384];
        struct sockaddr_nl sender = {0};
        struct iovec iov = {.iov_base = data, .iov_len = sizeof(data)};
        struct msghdr msg = {.msg_name = &sender, .msg_namelen = sizeof(sender), .msg_iov = &iov, .msg_iovlen = 1};
        ssize_t n = recvmsg(fd, &msg, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) { continue; }
        if (n <= 0 || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
            msg.msg_namelen < sizeof(sender) || sender.nl_family != AF_NETLINK || sender.nl_pid != 0) { err = MT_ERR_IO; goto out; }
        if ((size_t)n > MT_DNS_CLIENT_BODY_LIMIT - received) { err = MT_ERR_LIMIT; goto out; }
        received += (size_t)n;
        err = mt_dns_neighbors_parse(&table, data, (size_t)n, 1, &done);
        if (err != MT_OK) { goto out; }
    }
    if (stop && atomic_load(stop)) { err = MT_ERR_CANCELED; goto out; }
    mt_dns_neighbors_finish(&table);
    if (table.count) { *neighbors = table.items; *count = table.count; table.items = NULL; }
    err = MT_OK;
out:
    free(table.items);
    close(fd);
    return err;
}
