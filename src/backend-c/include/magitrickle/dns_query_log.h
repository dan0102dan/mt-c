/* Bounded RAM-only history; all operations run on mt_loop's thread.
 * Public entries are materialized views, NOT the ring's storage layout.
 */
#ifndef MAGITRICKLE_DNS_QUERY_LOG_H
#define MAGITRICKLE_DNS_QUERY_LOG_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef MT_DNS_QUERY_LOG_CAPACITY
#define MT_DNS_QUERY_LOG_CAPACITY 1000
#endif
#define MT_DNS_QUERY_LOG_MAX_CAPACITY 60000
#define MT_DNS_QUERY_LOG_DOMAIN_LEN 1024
#define MT_DNS_QUERY_LOG_CLIENT_LEN 48
#define MT_DNS_QUERY_LOG_MAX_SUBSCRIBERS 8

typedef struct mt_dns_query_log mt_dns_query_log_t;
typedef struct mt_dns_query_entry {
    uint64_t id;
    int64_t time_ms;
    char client[MT_DNS_QUERY_LOG_CLIENT_LEN];
    char domain[MT_DNS_QUERY_LOG_DOMAIN_LEN];
    uint16_t qtype;
    bool tcp;
} mt_dns_query_entry_t;

typedef struct mt_dns_query_log_stats {
    size_t capacity, count;
    size_t entry_bytes, index_bytes, pool_bytes, allocated_bytes;
    size_t domains, clients;
} mt_dns_query_log_stats_t;

mt_dns_query_log_t *mt_dns_query_log_create(void);
/* 1..MAX_CAPACITY; sizing for embedders/tests, not a new YAML setting. */
mt_dns_query_log_t *mt_dns_query_log_create_sized(size_t capacity);
void mt_dns_query_log_destroy(mt_dns_query_log_t *log);
/* Presentation-form adapter used by existing capture call sites. Invalid
 * names are rejected, not truncated. IPs and names are stored in binary. */
void mt_dns_query_log_append(mt_dns_query_log_t *log, const char *client,
    const char *domain, uint16_t qtype, bool tcp, int64_t time_ms);
/* addr_len is 0 (unknown), 4 (IPv4) or 16 (IPv6). name is a decompressed
 * DNS wire name including root, at most 255 bytes. Copies input. On OOM
 * or invalid input returns false without changing the retained history. */
bool mt_dns_query_log_append_wire(mt_dns_query_log_t *log,
    const uint8_t *addr, size_t addr_len, const uint8_t *name, size_t name_len,
    uint16_t qtype, bool tcp, int64_t time_ms);
size_t mt_dns_query_log_size(const mt_dns_query_log_t *log);
size_t mt_dns_query_log_capacity(const mt_dns_query_log_t *log);
bool mt_dns_query_log_get(const mt_dns_query_log_t *log, size_t newest_index,
    mt_dns_query_entry_t *out);
bool mt_dns_query_log_get_id(const mt_dns_query_log_t *log, uint64_t id,
    mt_dns_query_entry_t *out);
void mt_dns_query_log_clear(mt_dns_query_log_t *log);
/* Exact bytes requested from the allocator, including unused pool space;
 * excludes allocator bookkeeping, HTTP buffers and caller-owned views. */
void mt_dns_query_log_stats(const mt_dns_query_log_t *log,
    mt_dns_query_log_stats_t *out);

typedef void (*mt_dns_query_log_notify_fn)(void *ud);
/* Callbacks may unsubscribe themselves, but must not mutate the journal.
 * Stop subscribers before destroying the log. */
int mt_dns_query_log_subscribe(mt_dns_query_log_t *log,
    mt_dns_query_log_notify_fn notify, void *ud);
void mt_dns_query_log_unsubscribe(mt_dns_query_log_t *log, int subscription);
uint64_t mt_dns_query_log_sequence(const mt_dns_query_log_t *log);
uint64_t mt_dns_query_log_revision(const mt_dns_query_log_t *log);
#endif
