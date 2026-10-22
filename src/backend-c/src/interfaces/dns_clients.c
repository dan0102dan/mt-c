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

static int hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}
bool mt_dns_client_mac(const char *text, char out[18])
{
    if (!text || !out || strnlen(text, 18) != 17) { return false; }
    const char hex[] = "0123456789abcdef";
    unsigned any = 0;
    for (size_t i = 0; i < 6; i++) {
        int hi = hex_digit((unsigned char)text[3 * i]);
        int lo = hex_digit((unsigned char)text[3 * i + 1]);
        if (hi < 0 || lo < 0 || (i < 5 && text[3 * i + 2] != ':')) { return false; }
        unsigned byte = (unsigned)(hi * 16 + lo);
        if (!i && (byte & 1u)) { return false; }
        any |= byte;
        out[3 * i] = hex[hi]; out[3 * i + 1] = hex[lo];
        if (i < 5) { out[3 * i + 2] = ':'; }
    }
    out[17] = '\0';
    return any != 0;
}
mt_err_t mt_dns_client_table_add_owned(mt_dns_client_table_t *t, const char *ip,
    const char *name, const char *owner, const char *scope)
{
    mt_dns_client_observation_t row = {0};
    if (!mt_dns_client_address(ip, row.client.ip, sizeof(row.client.ip)) ||
        strcmp(row.client.ip, "0.0.0.0") == 0 || strcmp(row.client.ip, "::") == 0) { return MT_OK; }
    struct in_addr v4;
    struct in6_addr v6;
    if ((inet_pton(AF_INET, row.client.ip, &v4) == 1 &&
         (IN_MULTICAST(ntohl(v4.s_addr)) || v4.s_addr == INADDR_BROADCAST)) ||
        (inet_pton(AF_INET6, row.client.ip, &v6) == 1 && IN6_IS_ADDR_MULTICAST(&v6))) { return MT_OK; }
    if (owner && *owner && !mt_dns_client_mac(owner, row.owner)) { return MT_OK; }
    if (scope && *scope) {
        size_t len = strnlen(scope, sizeof(row.scope));
        if (len >= sizeof(row.scope)) { return MT_ERR_LIMIT; }
        memcpy(row.scope, scope, len + 1);
    }
    bool named = mt_dns_client_label(name, row.client.name);
    if (!named && !row.owner[0] && !row.scope[0]) { return MT_OK; }
    if (t->count == MT_DNS_CLIENT_LIMIT) { return MT_ERR_LIMIT; }
    if (t->count == t->capacity) {
        size_t cap = t->capacity ? t->capacity * 2 : 16;
        if (cap > MT_DNS_CLIENT_LIMIT) { cap = MT_DNS_CLIENT_LIMIT; }
        mt_dns_client_observation_t *items = realloc(t->items, cap * sizeof(*items));
        if (!items) { return MT_ERR_NOMEM; }
        t->items = items; t->capacity = cap;
    }
    t->items[t->count++] = row;
    return MT_OK;
}
mt_err_t mt_dns_client_table_add(mt_dns_client_table_t *t, const char *ip, const char *name)
{
    return mt_dns_client_table_add_owned(t, ip, name, NULL, NULL);
}
static int compare_clients(const void *a, const void *b)
{
    return strcmp(((const mt_dns_client_observation_t *)a)->client.ip,
                  ((const mt_dns_client_observation_t *)b)->client.ip);
}
void mt_dns_client_table_finish(mt_dns_client_table_t *t, mt_dns_client_t **out, size_t *count)
{
    if (t->count > 1) { qsort(t->items, t->count, sizeof(*t->items), compare_clients); }
    size_t n = 0;
    for (size_t i = 0; i < t->count;) {
        size_t end = i;
        bool ambiguous = false;
        const char *name = NULL, *owner = NULL, *scope = NULL;
        while (end < t->count && strcmp(t->items[i].client.ip, t->items[end].client.ip) == 0) {
            const mt_dns_client_observation_t *row = &t->items[end];
            if (row->client.name[0]) {
                if (name && strcmp(name, row->client.name)) { ambiguous = true; }
                name = row->client.name;
            }
            if (row->owner[0]) {
                if (owner && strcmp(owner, row->owner)) { ambiguous = true; }
                owner = row->owner;
            }
            if (row->scope[0]) {
                if (scope && strcmp(scope, row->scope)) { ambiguous = true; }
                scope = row->scope;
            }
            end++;
        }
        /* An ambiguous address must fall back to IP, not an arbitrary device. */
        if (!ambiguous && name) {
            mt_dns_client_t client = t->items[i].client;
            memcpy(client.name, name, strlen(name) + 1);
            /* Compact into the same allocation only after consuming the group.
             * Output records are smaller, so no unread observation is touched. */
            memcpy((unsigned char *)t->items + n * sizeof(client), &client, sizeof(client));
            n++;
        }
        i = end;
    }
    mt_dns_client_t *items = (mt_dns_client_t *)t->items;
    if (!n) { free(items); items = NULL; }
    else {
        mt_dns_client_t *small = realloc(items, n * sizeof(*small));
        if (small) { items = small; }
    }
    *out = items; *count = n;
    *t = (mt_dns_client_table_t){0};
}
