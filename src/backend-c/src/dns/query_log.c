#include "magitrickle/dns_query_log.h"
#include "magitrickle/dnswire.h"

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

/* Stable 16-bit handles, never pointers into a moving pool. One spare
 * dictionary slot allows acquiring the new keys BEFORE evicting a row. */
typedef struct intern_key {
    uint32_t offset, hash;
    uint16_t refs, len, next;
} intern_key_t;

typedef struct intern_table {
    intern_key_t *keys;
    uint16_t *buckets;
    uint8_t *pool;
    size_t slots, bucket_count, pool_cap, pool_top, pool_live, pool_limit, count;
    uint16_t free_head;
} intern_table_t;

typedef struct stored_query {
    uint64_t id;
    int64_t time_ms;
    uint16_t domain, client, qtype;
    uint8_t tcp, reserved;
} stored_query_t;
_Static_assert(sizeof(stored_query_t) == 24, "DNS ring entry layout changed");
_Static_assert(sizeof(intern_key_t) == 16, "DNS dictionary layout changed");

struct mt_dns_query_log {
    stored_query_t *entries;
    size_t capacity, next, count;
    uint64_t sequence, revision;
    intern_table_t domains, clients;
    struct { mt_dns_query_log_notify_fn notify; void *ud; }
        subscribers[MT_DNS_QUERY_LOG_MAX_SUBSCRIBERS];
};

/* Internal pool headers are byte-wise too: no unaligned casts on MIPS. */
static uint16_t read16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static void write16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static size_t block_size(size_t len) { return (len + 7u) & ~(size_t)3u; }
static uint32_t key_hash(const uint8_t *data, size_t len) {
    uint32_t h = UINT32_C(2166136261);
    for (size_t i = 0; i < len; i++) { h = (h ^ data[i]) * UINT32_C(16777619); }
    return h;
}
static void table_reset(intern_table_t *t) {
    free(t->pool); t->pool = NULL;
    t->pool_cap = t->pool_top = t->pool_live = t->count = 0;
    memset(t->keys, 0, t->slots * sizeof(*t->keys));
    memset(t->buckets, 0, t->bucket_count * sizeof(*t->buckets));
    t->free_head = 1;
    for (size_t i = 1; i + 1 < t->slots; i++) { t->keys[i].next = (uint16_t)(i + 1); }
}
static void table_destroy(intern_table_t *t) {
    free(t->pool); free(t->keys); free(t->buckets);
    memset(t, 0, sizeof(*t));
}
static bool table_init(intern_table_t *t, size_t capacity, size_t max_len) {
    t->slots = capacity + 2;
    t->bucket_count = 2;
    while (t->bucket_count < capacity * 2) { t->bucket_count *= 2; }
    t->pool_limit = (capacity + 1) * block_size(max_len);
    t->keys = calloc(t->slots, sizeof(*t->keys));
    t->buckets = calloc(t->bucket_count, sizeof(*t->buckets));
    if (!t->keys || !t->buckets) { table_destroy(t); return false; }
    table_reset(t);
    return true;
}
/* Compact in place, following the arena's length headers. Dead records
 * have owner=0. Handles are stable, and no second full-sized pool is made.
 * This runs only on arena pressure / substantial shrink, not every append. */
static void table_compact(intern_table_t *t) {
    size_t write = 0;
    for (size_t read = 0; read < t->pool_top;) {
        uint16_t id = read16(t->pool + read);
        size_t bytes = block_size(read16(t->pool + read + 2));
        if (id) {
            memmove(t->pool + write, t->pool + read, bytes);
            t->keys[id].offset = (uint32_t)write;
            write += bytes;
        }
        read += bytes;
    }
    t->pool_top = write;
}
static bool table_reserve(intern_table_t *t, size_t bytes) {
    if (bytes <= t->pool_cap - t->pool_top) { return true; }
    table_compact(t);
    if (bytes <= t->pool_cap - t->pool_top) { return true; }
    size_t needed = t->pool_top + bytes;
    if (needed > t->pool_limit) { return false; }
    size_t cap = t->pool_cap ? t->pool_cap : 512;
    while (cap < needed) { cap += cap / 2; }
    if (cap > t->pool_limit) { cap = t->pool_limit; }
    uint8_t *pool = realloc(t->pool, cap);
    if (!pool) { return false; }
    t->pool = pool; t->pool_cap = cap;
    return true;
}
static uint16_t table_acquire(intern_table_t *t, const uint8_t *data, size_t len) {
    uint32_t hash = key_hash(data, len);
    size_t bucket = hash & (t->bucket_count - 1);
    for (uint16_t id = t->buckets[bucket]; id; id = t->keys[id].next) {
        intern_key_t *key = &t->keys[id];
        if (key->hash == hash && key->len == len &&
            memcmp(t->pool + key->offset + 4, data, len) == 0) {
            key->refs++;
            return id;
        }
    }
    uint16_t id = t->free_head;
    if (!id || !table_reserve(t, block_size(len))) { return 0; }
    intern_key_t *key = &t->keys[id];
    t->free_head = key->next;
    key->offset = (uint32_t)t->pool_top; key->hash = hash;
    key->refs = 1; key->len = (uint16_t)len; key->next = t->buckets[bucket];
    t->buckets[bucket] = id;
    write16(t->pool + t->pool_top, id);
    write16(t->pool + t->pool_top + 2, (uint16_t)len);
    memcpy(t->pool + t->pool_top + 4, data, len);
    t->pool_top += block_size(len); t->pool_live += block_size(len); t->count++;
    return id;
}
static void table_release(intern_table_t *t, uint16_t id) {
    intern_key_t *key = &t->keys[id];
    if (--key->refs) { return; }
    size_t bucket = key->hash & (t->bucket_count - 1);
    uint16_t *link = &t->buckets[bucket];
    while (*link != id) { link = &t->keys[*link].next; }
    *link = key->next;
    write16(t->pool + key->offset, 0);
    t->pool_live -= block_size(key->len); t->count--;
    memset(key, 0, sizeof(*key)); key->next = t->free_head; t->free_head = id;
    if (!t->count) {
        free(t->pool); t->pool = NULL; t->pool_cap = t->pool_top = 0;
    } else if (t->pool_cap > 2048 && t->pool_live < t->pool_cap / 4) {
        table_compact(t);
        size_t cap = t->pool_live * 2;
        if (cap < 512) { cap = 512; }
        uint8_t *pool = realloc(t->pool, cap);
        if (pool) { t->pool = pool; t->pool_cap = cap; }
    }
}
static void notify_subscribers(mt_dns_query_log_t *log) {
    for (size_t i = 0; i < MT_DNS_QUERY_LOG_MAX_SUBSCRIBERS; i++) {
        if (log->subscribers[i].notify) { log->subscribers[i].notify(log->subscribers[i].ud); }
    }
}
mt_dns_query_log_t *mt_dns_query_log_create_sized(size_t capacity) {
    if (!capacity || capacity > MT_DNS_QUERY_LOG_MAX_CAPACITY) { return NULL; }
    mt_dns_query_log_t *log = calloc(1, sizeof(*log));
    if (!log) { return NULL; }
    log->capacity = capacity;
    log->entries = calloc(capacity, sizeof(*log->entries));
    if (!log->entries || !table_init(&log->domains, capacity, MT_DNS_MAX_NAME) ||
        !table_init(&log->clients, capacity, 17)) {
        mt_dns_query_log_destroy(log); return NULL;
    }
    return log;
}
mt_dns_query_log_t *mt_dns_query_log_create(void) {
    return mt_dns_query_log_create_sized(MT_DNS_QUERY_LOG_CAPACITY);
}
void mt_dns_query_log_destroy(mt_dns_query_log_t *log) {
    if (!log) { return; }
    table_destroy(&log->domains); table_destroy(&log->clients);
    free(log->entries); free(log);
}
static bool valid_name(const uint8_t *name, size_t len) {
    if (!name || !len || len > MT_DNS_MAX_NAME) { return false; }
    for (size_t pos = 0; pos < len;) {
        size_t n = name[pos++];
        if (!n) { return pos == len; }
        if (n > 63 || n >= len - pos) { return false; }
        pos += n;
    }
    return false;
}
bool mt_dns_query_log_append_wire(mt_dns_query_log_t *log,
    const uint8_t *addr, size_t addr_len, const uint8_t *name, size_t name_len,
    uint16_t qtype, bool tcp, int64_t time_ms) {
    if (!log || !valid_name(name, name_len) ||
        (addr_len != 0 && addr_len != 4 && addr_len != 16) ||
        (addr_len && !addr) || log->sequence == UINT64_MAX) { return false; }
    uint8_t client[17] = {0};
    client[0] = (uint8_t)addr_len;
    if (addr_len) { memcpy(client + 1, addr, addr_len); }
    uint16_t domain_id = table_acquire(&log->domains, name, name_len);
    if (!domain_id) { return false; }
    uint16_t client_id = table_acquire(&log->clients, client, addr_len + 1);
    if (!client_id) { table_release(&log->domains, domain_id); return false; }
    stored_query_t *entry = &log->entries[log->next];
    if (log->count == log->capacity) {
        table_release(&log->domains, entry->domain);
        table_release(&log->clients, entry->client);
    } else { log->count++; }
    *entry = (stored_query_t){.id = ++log->sequence, .time_ms = time_ms,
        .domain = domain_id, .client = client_id, .qtype = qtype, .tcp = tcp};
    log->next = (log->next + 1) % log->capacity;
    notify_subscribers(log);
    return true;
}
/* Inverse of dnswire's presentation escaping, including binary labels.
 * Exact case/bytes are retained: interning does not merge distinct queries. */
static size_t parse_name(const char *text, uint8_t name[MT_DNS_MAX_NAME]) {
    if (!text || !*text) { return 0; }
    if (strcmp(text, ".") == 0) { name[0] = 0; return 1; }
    size_t pos = 1, label = 0, n = 0;
    for (size_t i = 0; text[i];) {
        unsigned b = (unsigned char)text[i++];
        if (b == '.') {
            if (!n) { return 0; }
            name[label] = (uint8_t)n;
            if (!text[i]) { break; }
            if (pos >= MT_DNS_MAX_NAME - 1) { return 0; }
            label = pos++; n = 0;
            continue;
        }
        if (b == '\\') {
            if (!text[i]) { return 0; }
            b = (unsigned char)text[i++];
            if (b >= '0' && b <= '9' && text[i] >= '0' && text[i] <= '9' &&
                text[i + 1] >= '0' && text[i + 1] <= '9') {
                b = (b - '0') * 100 + (unsigned)(text[i] - '0') * 10 + (unsigned)(text[i + 1] - '0');
                i += 2;
                if (b > 255) { return 0; }
            }
        }
        if (n == 63 || pos >= MT_DNS_MAX_NAME - 1) { return 0; }
        name[pos++] = (uint8_t)b; n++;
    }
    name[label] = (uint8_t)n; name[pos++] = 0;
    return pos;
}
void mt_dns_query_log_append(mt_dns_query_log_t *log, const char *client,
    const char *domain, uint16_t qtype, bool tcp, int64_t time_ms) {
    uint8_t name[MT_DNS_MAX_NAME], addr[16];
    size_t len = parse_name(domain, name), addr_len = 0;
    if (!len) { return; }
    if (client && inet_pton(AF_INET, client, addr) == 1) { addr_len = 4; }
    else if (client && inet_pton(AF_INET6, client, addr) == 1) { addr_len = 16; }
    (void)mt_dns_query_log_append_wire(log, addr, addr_len, name, len, qtype, tcp, time_ms);
}
size_t mt_dns_query_log_size(const mt_dns_query_log_t *log) { return log ? log->count : 0; }
size_t mt_dns_query_log_capacity(const mt_dns_query_log_t *log) { return log ? log->capacity : 0; }
bool mt_dns_query_log_get(const mt_dns_query_log_t *log, size_t index, mt_dns_query_entry_t *out) {
    if (!log || !out || index >= log->count) { return false; }
    size_t slot = (log->next + log->capacity - 1 - index) % log->capacity;
    const stored_query_t *entry = &log->entries[slot];
    const intern_key_t *domain = &log->domains.keys[entry->domain];
    const intern_key_t *client = &log->clients.keys[entry->client];
    memset(out, 0, sizeof(*out));
    if (mt_dns_name_to_string(log->domains.pool + domain->offset + 4, domain->len,
        out->domain, sizeof(out->domain)) != MT_OK) { return false; }
    size_t len = strlen(out->domain);
    if (len > 1) { out->domain[len - 1] = '\0'; }
    const uint8_t *addr = log->clients.pool + client->offset + 4;
    if (!addr[0]) { memcpy(out->client, "unknown", 8); }
    else if (!inet_ntop(addr[0] == 4 ? AF_INET : AF_INET6, addr + 1,
        out->client, sizeof(out->client))) { return false; }
    out->id = entry->id; out->time_ms = entry->time_ms;
    out->qtype = entry->qtype; out->tcp = entry->tcp != 0;
    return true;
}
bool mt_dns_query_log_get_id(const mt_dns_query_log_t *log, uint64_t id, mt_dns_query_entry_t *out) {
    if (!log || !id || id > log->sequence || log->sequence - id >= log->count) { return false; }
    return mt_dns_query_log_get(log, (size_t)(log->sequence - id), out);
}
void mt_dns_query_log_clear(mt_dns_query_log_t *log) {
    if (!log) { return; }
    memset(log->entries, 0, log->capacity * sizeof(*log->entries));
    table_reset(&log->domains); table_reset(&log->clients);
    log->count = log->next = 0; log->revision++;
    notify_subscribers(log);
}
void mt_dns_query_log_stats(const mt_dns_query_log_t *log, mt_dns_query_log_stats_t *out) {
    if (!out) { return; }
    memset(out, 0, sizeof(*out));
    if (!log) { return; }
    out->capacity = log->capacity; out->count = log->count;
    out->entry_bytes = log->capacity * sizeof(*log->entries);
    out->index_bytes = (log->domains.slots + log->clients.slots) * sizeof(intern_key_t) +
        (log->domains.bucket_count + log->clients.bucket_count) * sizeof(uint16_t);
    out->pool_bytes = log->domains.pool_cap + log->clients.pool_cap;
    out->allocated_bytes = sizeof(*log) + out->entry_bytes + out->index_bytes + out->pool_bytes;
    out->domains = log->domains.count; out->clients = log->clients.count;
}
int mt_dns_query_log_subscribe(mt_dns_query_log_t *log, mt_dns_query_log_notify_fn notify, void *ud) {
    if (!log || !notify) { return 0; }
    for (int i = 0; i < MT_DNS_QUERY_LOG_MAX_SUBSCRIBERS; i++) {
        if (!log->subscribers[i].notify) {
            log->subscribers[i].notify = notify; log->subscribers[i].ud = ud; return i + 1;
        }
    }
    return 0;
}
void mt_dns_query_log_unsubscribe(mt_dns_query_log_t *log, int subscription) {
    if (!log || subscription < 1 || subscription > MT_DNS_QUERY_LOG_MAX_SUBSCRIBERS) { return; }
    log->subscribers[subscription - 1].notify = NULL;
    log->subscribers[subscription - 1].ud = NULL;
}
uint64_t mt_dns_query_log_sequence(const mt_dns_query_log_t *log) { return log ? log->sequence : 0; }
uint64_t mt_dns_query_log_revision(const mt_dns_query_log_t *log) { return log ? log->revision : 0; }
