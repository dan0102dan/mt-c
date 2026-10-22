#include "dns_clients_internal.h"
#include <arpa/inet.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

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
