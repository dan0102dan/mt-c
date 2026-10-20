#include "dns_clients_internal.h"
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef MT_PLATFORM_OPENWRT
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

bool mt_dns_client_address(const char *address, char *out, size_t size)
{
    if (!address || !out || !size || size > UINT_MAX) { return false; }
    struct in_addr v4;
    struct in6_addr v6;
    if (inet_pton(AF_INET, address, &v4) == 1) {
        return inet_ntop(AF_INET, &v4, out, (socklen_t)size) != NULL;
    }
    if (inet_pton(AF_INET6, address, &v6) != 1) { return false; }
    if (IN6_IS_ADDR_V4MAPPED(&v6)) {
        memcpy(&v4, v6.s6_addr + 12, sizeof(v4));
        return inet_ntop(AF_INET, &v4, out, (socklen_t)size) != NULL;
    }
    return inet_ntop(AF_INET6, &v6, out, (socklen_t)size) != NULL;
}

bool mt_dns_client_label(const char *name, char out[MT_DNS_CLIENT_NAME_LEN])
{
    if (!name || !out) { return false; }
    size_t len = strnlen(name, MT_DNS_CLIENT_NAME_LEN);
    if (!len || len >= MT_DNS_CLIENT_NAME_LEN) { return false; }
    while (len && *name == ' ') { name++; len--; }
    while (len && name[len - 1] == ' ') { len--; }
    if (!len || (len == 1 && name[0] == '*')) { return false; }
    for (size_t i = 0; i < len;) {
        const unsigned char c = (unsigned char)name[i++];
        if (c < 32 || c == 127) { return false; }
        if (c < 128) { continue; }
        unsigned remaining;
        uint32_t cp, minimum;
        if (c >= 0xc2 && c <= 0xdf) { remaining = 1; cp = c & 0x1fu; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { remaining = 2; cp = c & 0x0fu; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { remaining = 3; cp = c & 7u; minimum = 0x10000; }
        else { return false; }
        if (remaining > len - i) { return false; }
        while (remaining--) {
            const unsigned char next = (unsigned char)name[i++];
            if ((next & 0xc0u) != 0x80u) { return false; }
            cp = (cp << 6) | (next & 0x3fu);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
            (cp >= 0x80 && cp <= 0x9f)) { return false; }
    }
    memcpy(out, name, len); out[len] = '\0';
    return true;
}

mt_err_t mt_dns_client_table_add(mt_dns_client_table_t *t, const char *ip, const char *name)
{
    mt_dns_client_t row = {0};
    if (!mt_dns_client_address(ip, row.ip, sizeof(row.ip)) ||
        !mt_dns_client_label(name, row.name) ||
        strcmp(row.ip, "0.0.0.0") == 0 || strcmp(row.ip, "::") == 0) { return MT_OK; }
    if (t->count == MT_DNS_CLIENT_LIMIT) { return MT_ERR_LIMIT; }
    if (t->count == t->capacity) {
        size_t cap = t->capacity ? t->capacity * 2 : 16;
        if (cap > MT_DNS_CLIENT_LIMIT) { cap = MT_DNS_CLIENT_LIMIT; }
        mt_dns_client_t *items = realloc(t->items, cap * sizeof(*items));
        if (!items) { return MT_ERR_NOMEM; }
        t->items = items; t->capacity = cap;
    }
    t->items[t->count++] = row;
    return MT_OK;
}
static int compare_clients(const void *a, const void *b)
{
    return strcmp(((const mt_dns_client_t *)a)->ip, ((const mt_dns_client_t *)b)->ip);
}
void mt_dns_client_table_finish(mt_dns_client_table_t *t, mt_dns_client_t **out, size_t *count)
{
    if (t->count > 1) { qsort(t->items, t->count, sizeof(*t->items), compare_clients); }
    size_t n = 0;
    for (size_t i = 0; i < t->count;) {
        size_t end = i + 1;
        bool ambiguous = false;
        while (end < t->count && strcmp(t->items[i].ip, t->items[end].ip) == 0) {
            if (strcmp(t->items[i].name, t->items[end].name) != 0) { ambiguous = true; }
            end++;
        }
        /* An ambiguous address must fall back to IP, not an arbitrary device. */
        if (!ambiguous) { t->items[n++] = t->items[i]; }
        i = end;
    }
    if (!n) { free(t->items); t->items = NULL; }
    else {
        mt_dns_client_t *small = realloc(t->items, n * sizeof(*small));
        if (small) { t->items = small; }
    }
    *out = t->items; *count = n;
    *t = (mt_dns_client_table_t){0};
}

mt_err_t mt_dns_clients_parse_leases(const char *text, size_t len, int64_t now,
                                    mt_dns_client_t **clients, size_t *count)
{
    if (!clients || !count) { return MT_ERR_INVAL; }
    *clients = NULL; *count = 0;
    if (len > MT_DNS_CLIENT_BODY_LIMIT) { return MT_ERR_LIMIT; }
    if (!text && len) { return MT_ERR_INVAL; }
    if (!len) { return MT_OK; }
    if (memchr(text, 0, len)) { return MT_ERR_PROTO; }
    char *copy = malloc(len + 1);
    if (!copy) { return MT_ERR_NOMEM; }
    memcpy(copy, text, len); copy[len] = '\0';
    mt_dns_client_table_t table = {0};
    mt_err_t err = MT_OK;
    char *lines = NULL;
    for (char *line = strtok_r(copy, "\n", &lines); line; line = strtok_r(NULL, "\n", &lines)) {
        char *fields = NULL;
        char *expires = strtok_r(line, " \t\r", &fields);
        char *mac = strtok_r(NULL, " \t\r", &fields);
        char *ip = strtok_r(NULL, " \t\r", &fields);
        char *name = strtok_r(NULL, " \t\r", &fields);
        if (!expires || !mac || !ip || !name || *expires < '0' || *expires > '9') { continue; }
        errno = 0; char *end = NULL;
        unsigned long long expiry = strtoull(expires, &end, 10);
        if (errno || !end || *end || expiry > INT64_MAX || (expiry && (int64_t)expiry <= now)) { continue; }
        err = mt_dns_client_table_add(&table, ip, name);
        if (err != MT_OK) { break; }
    }
    free(copy);
    if (err != MT_OK) { free(table.items); return err; }
    mt_dns_client_table_finish(&table, clients, count);
    return MT_OK;
}

#ifdef MT_PLATFORM_OPENWRT
mt_err_t mt_dns_clients_fetch_openwrt(mt_dns_client_t **clients, size_t *count,
                                     const atomic_bool *stop, void *ud)
{
    (void)ud;
    *clients = NULL; *count = 0;
    if (stop && atomic_load(stop)) { return MT_ERR_CANCELED; }
    /* Read only dnsmasq's standard IPv4 lease file. No ubus or shell process,
     * and no guesses for IPv6, missing files or customized lease paths. */
    FILE *file = fopen("/tmp/dhcp.leases", "r");
    if (!file) { return MT_ERR_NOENT; }
    struct stat st;
    if (fstat(fileno(file), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uintmax_t)st.st_size > MT_DNS_CLIENT_BODY_LIMIT) { fclose(file); return MT_ERR_LIMIT; }
    size_t size = (size_t)st.st_size;
    char *text = malloc(size + 1);
    if (!text) { fclose(file); return MT_ERR_NOMEM; }
    size_t len = fread(text, 1, size + 1, file);
    bool failed = ferror(file) != 0 || len != size;
    fclose(file);
    mt_err_t err = failed ? MT_ERR_IO :
        stop && atomic_load(stop) ? MT_ERR_CANCELED :
        mt_dns_clients_parse_leases(text, len, (int64_t)time(NULL), clients, count);
    free(text);
    return err;
}
#endif
