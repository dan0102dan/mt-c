#include "magitrickle/dns_query_log.h"
#include "magitrickle/dnswire.h"

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

/* One byte-budgeted allocation, split into nonmoving 4 KiB pages. Pages are
 * shared by chronological records and size-class slabs; there is no row quota,
 * moving arena, per-query heap allocation or capacity-sized key array.
 * Handles are 16-byte offsets + 1, never native pointers or packed structs. */
#define PAGE_BYTES 4096u
#define HANDLE_UNIT 16u
#define MAX_PAGES (MT_DNS_QUERY_LOG_MEMORY_LIMIT / PAGE_BYTES)
#define HASH_BUCKETS 4096u
#define RECORD_BYTES MT_DNS_QUERY_LOG_RECORD_BYTES
#define RECORDS_PER_PAGE ((PAGE_BYTES - sizeof(int64_t)) / RECORD_BYTES)
#define CLASS_COUNT 11u
#define PAGE_FREE 255u
#define KEY_DOMAIN 1u
#define KEY_IPV4 2u
#define KEY_IPV6 3u
#define CODE_TCP 1u
#define CODE_ABSOLUTE 2u
#define MAX_DELTA (UINT32_MAX >> 2)

static const uint16_t slot_sizes[CLASS_COUNT] = {16, 32, 48, 64, 80, 96, 128, 160, 192, 224, 272};
typedef struct intern_key {
    uint32_t hash, refs;
    uint16_t next;
    uint8_t len, kind;
    uint8_t data[];
} intern_key_t;
_Static_assert(sizeof(intern_key_t) == 12, "DNS key layout changed");
typedef struct page_meta {
    uint16_t next, prev, free_slot, used;
    uint8_t kind; /* 0 = record page; 1..CLASS_COUNT = slab; 255 = free */
} page_meta_t;
typedef struct storage {
    uint16_t buckets[HASH_BUCKETS];
    uint16_t directory[MAX_PAGES]; /* absolute sequence block -> record page */
    page_meta_t pages[MAX_PAGES];
    uint16_t classes[CLASS_COUNT], free_page;
    size_t page_count, record_pages, slab_pages, domains;
    _Alignas(uint32_t) uint8_t data[];
} storage_t;
struct mt_dns_query_log {
    storage_t *store;
    size_t memory_limit, count;
    uint64_t sequence, revision, dropped;
    struct { mt_dns_query_log_notify_fn notify; void *ud; }
        subscribers[MT_DNS_QUERY_LOG_MAX_SUBSCRIBERS];
};
/* Four pages always suffice for a record, a pinned domain, client and rare
 * absolute timestamp, even when all previous rows have been evicted. */
#define MIN_BUDGET (sizeof(mt_dns_query_log_t) + sizeof(storage_t) + 4 * PAGE_BYTES)
_Static_assert((MT_DNS_QUERY_LOG_MEMORY_LIMIT - sizeof(mt_dns_query_log_t) - sizeof(storage_t)) /
    PAGE_BYTES <= UINT16_MAX / (PAGE_BYTES / HANDLE_UNIT), "DNS handles overflow");
_Static_assert(MIN_BUDGET <= MT_DNS_QUERY_LOG_MEMORY_LIMIT, "DNS budget too small");

static uint16_t read16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8); }
static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void write16(uint8_t *p, uint16_t n) { p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8); }
static void write32(uint8_t *p, uint32_t n) {
    p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8); p[2] = (uint8_t)(n >> 16); p[3] = (uint8_t)(n >> 24);
}
static uint8_t *page_data(storage_t *s, uint16_t page) { return s->data + (size_t)(page - 1) * PAGE_BYTES; }
static uint8_t *handle_data(storage_t *s, uint16_t handle) { return s->data + (size_t)(handle - 1) * HANDLE_UNIT; }
static intern_key_t *key_at(storage_t *s, uint16_t handle) { return (intern_key_t *)handle_data(s, handle); }
static size_t directory_slot(uint64_t id) { return (size_t)(((id - 1) / RECORDS_PER_PAGE) % MAX_PAGES); }
static uint8_t *record_at(storage_t *s, uint64_t id) {
    uint16_t page = s->directory[directory_slot(id)];
    return page_data(s, page) + sizeof(int64_t) + (size_t)((id - 1) % RECORDS_PER_PAGE) * RECORD_BYTES;
}
static void reset_storage(mt_dns_query_log_t *log) {
    storage_t *s = log->store;
    memset(s, 0, log->memory_limit - sizeof(*log));
    s->page_count = (log->memory_limit - sizeof(*log) - sizeof(*s)) / PAGE_BYTES;
    s->free_page = 1;
    for (size_t i = 0; i < s->page_count; i++) {
        s->pages[i].kind = PAGE_FREE;
        s->pages[i].next = i + 1 < s->page_count ? (uint16_t)(i + 2) : 0;
    }
}
static void page_release(storage_t *s, uint16_t page) {
    s->pages[page - 1] = (page_meta_t){.next = s->free_page, .kind = PAGE_FREE};
    s->free_page = page;
}
static void evict_oldest(mt_dns_query_log_t *log);
static uint16_t page_acquire(mt_dns_query_log_t *log, uint8_t kind) {
    storage_t *s = log->store;
    /* Eviction only touches rows leaving retention. At most one record page's
     * worth of oldest rows needs to leave before a page can be reused. */
    while (!s->free_page) { evict_oldest(log); }
    uint16_t page = s->free_page;
    s->free_page = s->pages[page - 1].next;
    s->pages[page - 1] = (page_meta_t){.kind = kind};
    return page;
}
static void class_remove(storage_t *s, uint16_t page) {
    page_meta_t *m = &s->pages[page - 1];
    if (m->prev) { s->pages[m->prev - 1].next = m->next; }
    else { s->classes[m->kind - 1] = m->next; }
    if (m->next) { s->pages[m->next - 1].prev = m->prev; }
    m->next = m->prev = 0;
}
static void class_add(storage_t *s, uint16_t page) {
    page_meta_t *m = &s->pages[page - 1];
    m->next = s->classes[m->kind - 1]; m->prev = 0;
    if (m->next) { s->pages[m->next - 1].prev = page; }
    s->classes[m->kind - 1] = page;
}
static uint16_t slot_acquire(mt_dns_query_log_t *log, size_t bytes) {
    storage_t *s = log->store;
    size_t cls = 0;
    while (slot_sizes[cls] < bytes) { cls++; }
    while (!s->classes[cls] && !s->free_page) { evict_oldest(log); }
    uint16_t page = s->classes[cls];
    if (!page) {
        page = page_acquire(log, (uint8_t)(cls + 1));
        size_t slots = PAGE_BYTES / slot_sizes[cls];
        uint8_t *data = page_data(s, page);
        for (size_t i = 0; i < slots; i++) {
            write16(data + i * slot_sizes[cls], i + 1 < slots ? (uint16_t)(i + 2) : 0);
        }
        s->pages[page - 1].free_slot = 1;
        s->slab_pages++;
        class_add(s, page);
    }
    page_meta_t *m = &s->pages[page - 1];
    size_t offset = (size_t)(m->free_slot - 1) * slot_sizes[cls];
    m->free_slot = read16(page_data(s, page) + offset);
    m->used++;
    if (!m->free_slot) { class_remove(s, page); }
    return (uint16_t)(((size_t)(page - 1) * PAGE_BYTES + offset) / HANDLE_UNIT + 1);
}
static void slot_release(storage_t *s, uint16_t handle) {
    size_t offset = (size_t)(handle - 1) * HANDLE_UNIT;
    uint16_t page = (uint16_t)(offset / PAGE_BYTES + 1);
    page_meta_t *m = &s->pages[page - 1];
    size_t cls = (size_t)m->kind - 1;
    if (!m->free_slot) { class_add(s, page); }
    write16(handle_data(s, handle), m->free_slot);
    m->free_slot = (uint16_t)((offset % PAGE_BYTES) / slot_sizes[cls] + 1);
    if (--m->used == 0) {
        class_remove(s, page);
        s->slab_pages--;
        page_release(s, page);
    }
}
static uint32_t key_hash(uint8_t kind, const uint8_t *data, size_t len) {
    uint32_t hash = (UINT32_C(2166136261) ^ kind) * UINT32_C(16777619);
    for (size_t i = 0; i < len; i++) { hash = (hash ^ data[i]) * UINT32_C(16777619); }
    return hash;
}
static uint16_t key_acquire(mt_dns_query_log_t *log, uint8_t kind, const uint8_t *data, size_t len) {
    storage_t *s = log->store;
    uint32_t hash = key_hash(kind, data, len);
    size_t bucket = hash & (HASH_BUCKETS - 1);
    for (uint16_t h = s->buckets[bucket]; h; h = key_at(s, h)->next) {
        intern_key_t *key = key_at(s, h);
        if (key->hash == hash && key->kind == kind && key->len == len && !memcmp(key->data, data, len)) {
            key->refs++; /* Pin before any allocation can evict its last row. */
            return h;
        }
    }
    uint16_t handle = slot_acquire(log, sizeof(intern_key_t) + len);
    intern_key_t *key = key_at(s, handle);
    *key = (intern_key_t){.hash = hash, .refs = 1, .next = s->buckets[bucket], .len = (uint8_t)len, .kind = kind};
    memcpy(key->data, data, len);
    s->buckets[bucket] = handle;
    if (kind == KEY_DOMAIN) { s->domains++; }
    return handle;
}
static void key_release(storage_t *s, uint16_t handle) {
    if (!handle) { return; }
    intern_key_t *key = key_at(s, handle);
    if (--key->refs) { return; }
    uint16_t *link = &s->buckets[key->hash & (HASH_BUCKETS - 1)];
    while (*link != handle) { link = &key_at(s, *link)->next; }
    *link = key->next;
    if (key->kind == KEY_DOMAIN) { s->domains--; }
    slot_release(s, handle);
}
static void evict_oldest(mt_dns_query_log_t *log) {
    storage_t *s = log->store;
    uint64_t id = log->sequence - log->count + 1;
    size_t dir = directory_slot(id);
    uint16_t page = s->directory[dir];
    uint8_t *record = record_at(s, id);
    uint32_t code = read32(record);
    key_release(s, read16(record + 4));
    key_release(s, read16(record + 6));
    if (code & CODE_ABSOLUTE) { slot_release(s, (uint16_t)(code >> 2)); }
    if (--s->pages[page - 1].used == 0) {
        s->directory[dir] = 0;
        s->record_pages--;
        page_release(s, page);
    }
    log->count--;
}
static void notify_subscribers(mt_dns_query_log_t *log) {
    for (size_t i = 0; i < MT_DNS_QUERY_LOG_MAX_SUBSCRIBERS; i++) {
        if (log->subscribers[i].notify) { log->subscribers[i].notify(log->subscribers[i].ud); }
    }
}
mt_dns_query_log_t *mt_dns_query_log_create_budget(size_t bytes) {
    if (bytes < MIN_BUDGET || bytes > MT_DNS_QUERY_LOG_MEMORY_LIMIT) { return NULL; }
    mt_dns_query_log_t *log = calloc(1, sizeof(*log));
    if (log) { log->memory_limit = bytes; }
    return log;
}
mt_dns_query_log_t *mt_dns_query_log_create(void) {
    return mt_dns_query_log_create_budget(MT_DNS_QUERY_LOG_MEMORY_LIMIT);
}
void mt_dns_query_log_destroy(mt_dns_query_log_t *log) {
    if (!log) { return; }
    free(log->store);
    free(log);
}
bool mt_dns_query_log_enabled(const mt_dns_query_log_t *log) { return log && log->store; }
size_t mt_dns_query_log_memory_limit(const mt_dns_query_log_t *log) { return log ? log->memory_limit : 0; }
bool mt_dns_query_log_set_enabled(mt_dns_query_log_t *log, bool enabled) {
    if (!log) { return false; }
    if (mt_dns_query_log_enabled(log) == enabled) { return true; }
    if (enabled) {
        storage_t *s = malloc(log->memory_limit - sizeof(*log));
        if (!s) { return false; }
        log->store = s;
        reset_storage(log);
    } else {
        free(log->store); log->store = NULL;
    }
    log->count = 0; log->dropped = 0; log->revision++;
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
static bool delta_fits(int64_t time_ms, int64_t base) {
    /* Unsigned subtraction avoids signed overflow even across INT64_MIN/MAX. */
    return time_ms >= base && (uint64_t)time_ms - (uint64_t)base <= MAX_DELTA;
}
bool mt_dns_query_log_append_wire(mt_dns_query_log_t *log, const uint8_t *addr, size_t addr_len,
    const uint8_t *name, size_t name_len, uint16_t qtype, bool tcp, int64_t time_ms) {
    if (!mt_dns_query_log_enabled(log) || !valid_name(name, name_len) ||
        (addr_len != 0 && addr_len != 4 && addr_len != 16) ||
        (addr_len && !addr) || log->sequence == UINT64_MAX) { return false; }
    storage_t *s = log->store;
    uint16_t domain = key_acquire(log, KEY_DOMAIN, name, name_len);
    uint16_t client = addr_len ? key_acquire(log, addr_len == 4 ? KEY_IPV4 : KEY_IPV6, addr, addr_len) : 0;
    uint64_t id = log->sequence + 1;
    size_t dir = directory_slot(id);
    uint16_t page = s->directory[dir], absolute = 0;
    int64_t base = time_ms;
    if (page) { memcpy(&base, page_data(s, page), sizeof(base)); }
    if (!delta_fits(time_ms, base)) {
        absolute = slot_acquire(log, sizeof(time_ms));
        memcpy(handle_data(s, absolute), &time_ms, sizeof(time_ms));
    }
    /* Acquiring keys/timestamp may have evicted the last row of this page. */
    page = s->directory[dir];
    if (!page) {
        page = page_acquire(log, 0);
        s->directory[dir] = page; s->record_pages++;
        memcpy(page_data(s, page), &time_ms, sizeof(time_ms));
    }
    memcpy(&base, page_data(s, page), sizeof(base));
    uint32_t code;
    if (delta_fits(time_ms, base)) {
        if (absolute) { slot_release(s, absolute); }
        code = (uint32_t)((uint64_t)time_ms - (uint64_t)base) << 2;
    } else {
        code = (uint32_t)absolute << 2 | CODE_ABSOLUTE;
    }
    if (tcp) { code |= CODE_TCP; }
    uint8_t *record = record_at(s, id);
    write32(record, code); write16(record + 4, domain);
    write16(record + 6, client); write16(record + 8, qtype);
    s->pages[page - 1].used++;
    log->sequence = id; log->count++;
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
size_t mt_dns_query_log_capacity(const mt_dns_query_log_t *log) {
    /* Protocol validation upper bound derived from bytes, NOT an eviction rule
     * or an array reserved for this many records. Slabs share these pages. */
    return log ? (log->memory_limit - sizeof(*log) - sizeof(storage_t)) / PAGE_BYTES * RECORDS_PER_PAGE : 0;
}
bool mt_dns_query_log_get(const mt_dns_query_log_t *log, size_t index, mt_dns_query_entry_t *out) {
    if (!log || !out || index >= log->count) { return false; }
    storage_t *s = log->store;
    uint64_t id = log->sequence - index;
    uint8_t *record = record_at(s, id);
    const intern_key_t *domain = key_at(s, read16(record + 4));
    memset(out, 0, sizeof(*out));
    if (mt_dns_name_to_string(domain->data, domain->len, out->domain, sizeof(out->domain)) != MT_OK) { return false; }
    size_t len = strlen(out->domain);
    if (len > 1) { out->domain[len - 1] = '\0'; }
    uint16_t client = read16(record + 6);
    if (!client) { memcpy(out->client, "unknown", 8); }
    else {
        const intern_key_t *key = key_at(s, client);
        if (!inet_ntop(key->kind == KEY_IPV4 ? AF_INET : AF_INET6, key->data,
                      out->client, sizeof(out->client))) { return false; }
    }
    uint32_t code = read32(record);
    if (code & CODE_ABSOLUTE) { memcpy(&out->time_ms, handle_data(s, (uint16_t)(code >> 2)), sizeof(out->time_ms)); }
    else {
        int64_t base;
        memcpy(&base, page_data(s, s->directory[directory_slot(id)]), sizeof(base));
        out->time_ms = base + (int64_t)(code >> 2);
    }
    out->id = id; out->qtype = read16(record + 8); out->tcp = (code & CODE_TCP) != 0;
    return true;
}
bool mt_dns_query_log_get_id(const mt_dns_query_log_t *log, uint64_t id, mt_dns_query_entry_t *out) {
    if (!log || !id || id > log->sequence || log->sequence - id >= log->count) { return false; }
    return mt_dns_query_log_get(log, (size_t)(log->sequence - id), out);
}
void mt_dns_query_log_clear(mt_dns_query_log_t *log) {
    if (!log) { return; }
    if (log->store) { reset_storage(log); }
    log->count = 0; log->dropped = 0; log->revision++;
    notify_subscribers(log);
}
void mt_dns_query_log_stats(const mt_dns_query_log_t *log, mt_dns_query_log_stats_t *out) {
    if (!out) { return; }
    memset(out, 0, sizeof(*out));
    if (!log) { return; }
    out->capacity = mt_dns_query_log_capacity(log); out->count = log->count;
    if (log->store) {
        out->entry_bytes = log->store->record_pages * PAGE_BYTES;
        out->index_bytes = sizeof(storage_t);
        out->pool_bytes = log->store->slab_pages * PAGE_BYTES;
        out->domains = log->store->domains;
    }
    out->allocated_bytes = log->store ? log->memory_limit : sizeof(*log);
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
