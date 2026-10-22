/* Native OpenWrt inventory. One worker owns all libubus state. */
#ifndef MAGITRICKLE_DNS_OPENWRT_H
#define MAGITRICKLE_DNS_OPENWRT_H
#include "magitrickle/dns_clients.h"
#ifdef MT_PLATFORM_OPENWRT
struct blob_attr;
typedef struct {
    bool active, refresh;
    uint64_t epoch;
} mt_dns_source_command_t;
typedef struct {
    int control_fd;
    const atomic_bool *stop;
    void *ud;
    mt_dns_source_command_t (*command)(void *ud);
    void (*publish)(void *ud, uint64_t epoch, mt_dns_client_t *clients,
                    size_t count, mt_err_t error);
    /* NULL uses libubus' platform default. Tests use an isolated real ubusd. */
    const char *socket_path;
} mt_dns_openwrt_worker_t;
void mt_dns_openwrt_run(const mt_dns_openwrt_worker_t *worker);
/* Parse luci-rpc.getDHCPLeases, including DHCPv6 /128 addresses. */
mt_err_t mt_dns_openwrt_parse(const struct blob_attr *reply, size_t len,
    mt_dns_client_t **clients, size_t *count, uint64_t *expires_in_ms);
#endif
#endif
