#include "magitrickle/dns_query_log.h"
#include "magitrickle/dnswire.h"

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

/* Only variable-length names are interned. An inline binary IP is cheaper
 * than a second dictionary's keys/buckets, even with a 40-byte ring record.
 * Each distinct name owns one exact-sized allocation. This deliberately uses
 * the system allocator instead of a moving arena: churn at the byte limit
 * must never compact the entire retained history in the DNS event loop. */
typedef struct intern_key {
    uint8_t *name;
    uint32_t hash;
    uint16_t refs, len, next;
} intern_key_t;
typedef struct intern_table {
    intern_key_t *keys;
    uint16_t *buckets;
    size_t slots, bucket_count, bytes, count;
    uint16_t free_head;
} intern_table_t;
typedef struct stored_query {
    uint64_t id;
    int64_t time_ms;
    uint8_t addr[16];
    uint16_t domain, qtype;
    uint8_t addr_len, tcp;
} stored_query_t;
_Static_assert(sizeof(stored_query_t) == 40, "DNS ring layout changed");

struct mt_dns_query_log {
    stored_query_t *entries;
    size_t capacity, next, count, memory_limit;
    uint64_t sequence, revision, dropped;
    bool enabled;
    intern_table_t domains;
    struct { mt_dns_query_log_notify_fn notify; void *ud; }
        subscribers[MT_DNS_QUERY_LOG_MAX_SUBSCRIBERS];
};

static uint32_t key_hash(const uint8_t *p, size_t len) {
    uint32_t h = UINT32_C(2166136261);
    for (size_t i = 0; i < len; i++) { h = (h ^ p[i]) * UINT32_C(16777619); }
    return h;
}
static void table_reset(intern_table_t *t) {
    for (size_t i = 1; i < t->slots; i++) { free(t->keys[i].name); }
    if (t->keys) { memset(t->keys, 0, t->slots * sizeof(*t->keys)); }
    if (t->buckets) { memset(t->buckets, 0, t->bucket_count * sizeof(*t->buckets)); }
    t->bytes = t->count = 0;
    t->free_head = t->slots ? 1 : 0;
    for (size_t i = 1; i + 1 < t->slots; i++) { t->keys[i].next = (uint16_t)(i + 1); }
}
static void table_destroy(intern_table_t *t) {
    if (t->keys) {
        for (size_t i = 1; i < t->slots; i++) { free(t->keys[i].name); }
    }
    free(t->keys);
    free(t->buckets);
    memset(t, 0, sizeof(*t));
}
static bool table_init(intern_table_t *t, size_t capacity) {
    /* Zero is the null handle. A spare key permits allocation BEFORE eviction. */
    t->slots = capacity + 2;
    t->bucket_count = 2;
    while (t->bucket_count < capacity * 2) { t->bucket_count *= 2; }
    t->keys = calloc(t->slots, sizeof(*t->keys));
    t->buckets = calloc(t->bucket_count, sizeof(*t->buckets));
    if (!t->keys || !t->buckets) { table_destroy(t); return false; }
    table_reset(t);
    return true;
}
static uint16_t table_find(const intern_table_t *t, const uint8_t *name, size_t len, uint32_t hash) {
    for (uint16_t id = t->buckets[hash & (t->bucket_count - 1)]; id; id = t->keys[id].next) {
        const intern_key_t *key = &t->keys[id];
        if (key->hash == hash && key->len == len && !memcmp(key->name, name, len)) { return id; }
    }
    return 0;
}
static uint16_t table_insert(intern_table_t *t, uint8_t *name, size_t len, uint32_t hash) {
    uint16_t id = t->free_head;
    intern_key_t *key = &t->keys[id];
    t->free_head = key->next;
    size_t bucket = hash & (t->bucket_count - 1);
    *key = (intern_key_t){.name = name, .hash = hash, .refs = 1,
        .len = (uint16_t)len, .next = t->buckets[bucket]};
    t->buckets[bucket] = id;
    t->bytes += len;
    t->count++;
    return id;
}
static void table_release(intern_table_t *t, uint16_t id) {
    intern_key_t *key = &t->keys[id];
    if (--key->refs) { return; }
    uint16_t *link = &t->buckets[key->hash & (t->bucket_count - 1)];
    while (*link != id) { link = &t->keys[*link].next; }
    *link = key->next;
    t->bytes -= key->len;
    t->count--;
    free(key->name);
    *key = (intern_key_t){.next = t->free_head};
    t->free_head = id;
}
static size_t allocated_bytes(const mt_dns_query_log_t *log) {
    return sizeof(*log) + (log->entries ? log->capacity * sizeof(*log->entries) : 0) +
        log->domains.slots * sizeof(intern_key_t) +
        log->domains.bucket_count * sizeof(uint16_t) + log->domains.bytes;
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
    if (!log->entries || !table_init(&log->domains, capacity)) {
        mt_dns_query_log_destroy(log);
        return NULL;
    }
    log->enabled = true;
    return log;
}
mt_dns_query_log_t *mt_dns_query_log_create(void) {
    mt_dns_query_log_t *log = calloc(1, sizeof(*log));
    if (log) {
        log->capacity = MT_DNS_QUERY_LOG_CAPACITY;
        log->memory_limit = MT_DNS_QUERY_LOG_MEMORY_LIMIT;
    }
    return log;
}
void mt_dns_query_log_destroy(mt_dns_query_log_t *log) {
    if (!log) { return; }
    table_destroy(&log->domains);
    free(log->entries);
    free(log);
}
bool mt_dns_query_log_enabled(const mt_dns_query_log_t *log) { return log && log->enabled; }
size_t mt_dns_query_log_memory_limit(const mt_dns_query_log_t *log) { return log ? log->memory_limit : 0; }
bool mt_dns_query_log_set_enabled(mt_dns_query_log_t *log, bool enabled) {
    if (!log) { return false; }
    if (log->enabled == enabled) { return true; }
    if (enabled) {
        mt_dns_query_log_t *fresh = mt_dns_query_log_create_sized(log->capacity);
        if (!fresh) { return false; }
        /* Keep one maximum name's headroom: a failed malloc never destroys
         * retained rows, and even the pre-eviction allocation fits the cap. */
        if (log->memory_limit && allocated_bytes(fresh) + 2 * MT_DNS_MAX_NAME > log->memory_limit) {
            mt_dns_query_log_destroy(fresh);
            return false;
        }
        log->entries = fresh->entries;
        log->domains = fresh->domains;
        free(fresh);
    } else {
        table_destroy(&log->domains);
        free(log->entries);
        log->entries = NULL;
    }
    log->enabled = enabled;
    log->count = log->next = 0;
    log->dropped = 0;
    log->revision++;
    notify_subscribers(log);
    return true;
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
static void evict_oldest(mt_dns_query_log_t *log) {
    size_t index = (log->next + log->capacity - log->count) % log->capacity;
    table_release(&log->domains, log->entries[index].domain);
    log->count--;
}
bool mt_dns_query_log_append_wire(mt_dns_query_log_t *log, const uint8_t *addr, size_t addr_len,
    const uint8_t *name, size_t name_len, uint16_t qtype, bool tcp, int64_t time_ms) {
    if (!log || !log->enabled || !valid_name(name, name_len) ||
        (addr_len != 0 && addr_len != 4 && addr_len != 16) ||
        (addr_len && !addr) || log->sequence == UINT64_MAX) { return false; }
    uint32_t hash = key_hash(name, name_len);
    uint16_t id = table_find(&log->domains, name, name_len, hash);
    if (id) {
        /* Pin before eviction, which may release the oldest reference. */
        log->domains.keys[id].refs++;
    } else {
        uint8_t *copy = malloc(name_len);
        if (!copy) {
            if (log->dropped != UINT64_MAX) { log->dropped++; }
            notify_subscribers(log);
            return false;
        }
        memcpy(copy, name, name_len);
        if (log->memory_limit) {
            while (allocated_bytes(log) + name_len > log->memory_limit - MT_DNS_MAX_NAME) {
                evict_oldest(log);
            }
        }
        id = table_insert(&log->domains, copy, name_len, hash);
    }
    if (log->count == log->capacity) { evict_oldest(log); }
    stored_query_t *entry = &log->entries[log->next];
    *entry = (stored_query_t){.id = ++log->sequence, .time_ms = time_ms,
        .domain = id, .qtype = qtype, .addr_len = (uint8_t)addr_len, .tcp = tcp};
    if (addr_len) { memcpy(entry->addr, addr, addr_len); }
    log->count++;
    log->next = (log->next + 1) % log->capacity;
    notify_subscribers(log);
    return true;
}
static size_t parse_name(const char *text, uint8_t name[MT_DNS_MAX_NAME]) {
    if (!text || !*text) return 0;
    if (!strcmp(text, ".")) { name[0] = 0; return 1; }
    size_t pos = 1, label = 0, n = 0;
    for (size_t i = 0; text[i];) {
        unsigned b = (unsigned char)text[i++];
        if (b == '.') {
            if (!n) return 0;
            name[label] = (uint8_t)n;
            if (!text[i]) break;
            if (pos >= MT_DNS_MAX_NAME - 1) return 0;
            label = pos++; n = 0; continue;
        }
        if (b == '\\') {
            if (!text[i]) return 0;
            b = (unsigned char)text[i++];
            if (b >= '0' && b <= '9' && text[i] >= '0' && text[i] <= '9' && text[i + 1] >= '0' && text[i + 1] <= '9') {
                b = (b - '0') * 100 + (unsigned)(text[i] - '0') * 10 + (unsigned)(text[i + 1] - '0');
                i += 2; if (b > 255) return 0;
            }
        }
        if (n == 63 || pos >= MT_DNS_MAX_NAME - 1) return 0;
        name[pos++] = (uint8_t)b; n++;
    }
    name[label] = (uint8_t)n; name[pos++] = 0; return pos;
}
void mt_dns_query_log_append(mt_dns_query_log_t *log, const char *client, const char *domain,
    uint16_t qtype, bool tcp, int64_t time_ms) {
    if (!mt_dns_query_log_enabled(log)) { return; }
    uint8_t name[MT_DNS_MAX_NAME], addr[16]; size_t len = parse_name(domain, name), addr_len = 0;
    if (!len) return;
    if (client && inet_pton(AF_INET, client, addr) == 1) addr_len = 4;
    else if (client && inet_pton(AF_INET6, client, addr) == 1) addr_len = 16;
    (void)mt_dns_query_log_append_wire(log, addr, addr_len, name, len, qtype, tcp, time_ms);
}
size_t mt_dns_query_log_size(const mt_dns_query_log_t *log) { return log ? log->count : 0; }
size_t mt_dns_query_log_capacity(const mt_dns_query_log_t *log) { return log ? log->capacity : 0; }
bool mt_dns_query_log_get(const mt_dns_query_log_t *log, size_t index, mt_dns_query_entry_t *out) {
    if (!log || !out || index >= log->count) { return false; }
    size_t slot = (log->next + log->capacity - 1 - index) % log->capacity;
    const stored_query_t *entry = &log->entries[slot];
    const intern_key_t *domain = &log->domains.keys[entry->domain];
    memset(out, 0, sizeof(*out));
    if (mt_dns_name_to_string(domain->name, domain->len, out->domain, sizeof(out->domain)) != MT_OK) {
        return false;
    }
    size_t len = strlen(out->domain);
    if (len > 1) { out->domain[len - 1] = '\0'; }
    if (!entry->addr_len) { memcpy(out->client, "unknown", 8); }
    else if (!inet_ntop(entry->addr_len == 4 ? AF_INET : AF_INET6,
        entry->addr, out->client, sizeof(out->client))) { return false; }
    out->id = entry->id;
    out->time_ms = entry->time_ms;
    out->qtype = entry->qtype;
    out->tcp = entry->tcp != 0;
    return true;
}
bool mt_dns_query_log_get_id(const mt_dns_query_log_t *log, uint64_t id, mt_dns_query_entry_t *out) {
    if (!log || !id || id > log->sequence || log->sequence - id >= log->count) { return false; }
    return mt_dns_query_log_get(log, (size_t)(log->sequence - id), out);
}
void mt_dns_query_log_clear(mt_dns_query_log_t *log) {
    if (!log) { return; }
    if (log->entries) { memset(log->entries, 0, log->capacity * sizeof(*log->entries)); }
    table_reset(&log->domains);
    log->count = log->next = 0;
    log->dropped = 0;
    log->revision++;
    notify_subscribers(log);
}
void mt_dns_query_log_stats(const mt_dns_query_log_t *log, mt_dns_query_log_stats_t *out) {
    if (!out) { return; }
    memset(out, 0, sizeof(*out));
    if (!log) { return; }
    out->capacity = log->capacity;
    out->count = log->count;
    out->entry_bytes = log->entries ? log->capacity * sizeof(*log->entries) : 0;
    out->index_bytes = log->domains.slots * sizeof(intern_key_t) +
        log->domains.bucket_count * sizeof(uint16_t);
    out->pool_bytes = log->domains.bytes;
    out->allocated_bytes = allocated_bytes(log);
    out->domains = log->domains.count;
    out->dropped = log->dropped;
}
int mt_dns_query_log_subscribe(mt_dns_query_log_t *log, mt_dns_query_log_notify_fn notify, void *ud) {
    if (!log || !notify) return 0;
    for (int i = 0; i < MT_DNS_QUERY_LOG_MAX_SUBSCRIBERS; i++) if (!log->subscribers[i].notify) {
        log->subscribers[i].notify = notify; log->subscribers[i].ud = ud; return i + 1;
    }
    return 0;
}
void mt_dns_query_log_unsubscribe(mt_dns_query_log_t *log, int subscription) {
    if (!log || subscription < 1 || subscription > MT_DNS_QUERY_LOG_MAX_SUBSCRIBERS) return;
    log->subscribers[subscription - 1].notify = NULL; log->subscribers[subscription - 1].ud = NULL;
}
uint64_t mt_dns_query_log_sequence(const mt_dns_query_log_t *log) { return log ? log->sequence : 0; }
uint64_t mt_dns_query_log_revision(const mt_dns_query_log_t *log) { return log ? log->revision : 0; }
