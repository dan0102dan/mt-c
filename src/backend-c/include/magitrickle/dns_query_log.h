/* Loop-local, RAM-only DNS history. Production capture starts disabled. */
#ifndef MAGITRICKLE_DNS_QUERY_LOG_H
#define MAGITRICKLE_DNS_QUERY_LOG_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifndef MT_DNS_QUERY_LOG_CAPACITY
#define MT_DNS_QUERY_LOG_CAPACITY 8192
#endif
#define MT_DNS_QUERY_LOG_MEMORY_LIMIT ((size_t)1024 * 1024)
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
/* Allocates only the small control object, not the history or dictionaries. */
mt_dns_query_log_t *mt_dns_query_log_create(void);
/* Explicit enabled, row-bounded constructor for tests/embedders. */
mt_dns_query_log_t *mt_dns_query_log_create_sized(size_t capacity);
void mt_dns_query_log_destroy(mt_dns_query_log_t *log);
bool mt_dns_query_log_set_enabled(mt_dns_query_log_t *log, bool enabled);
bool mt_dns_query_log_enabled(const mt_dns_query_log_t *log);
size_t mt_dns_query_log_memory_limit(const mt_dns_query_log_t *log);
void mt_dns_query_log_append(mt_dns_query_log_t *log, const char *client,
    const char *domain, uint16_t qtype, bool tcp, int64_t time_ms);
/* Valid decompressed wire name, including root; IP length 0/4/16. Disabled
 * capture is a no-op. Memory pressure evicts oldest rows, never blocks DNS. */
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
/* Requested allocation bytes including reserved pool/index space. Excludes
 * allocator bookkeeping, temporary realloc/HTTP buffers and browser RAM. */
void mt_dns_query_log_stats(const mt_dns_query_log_t *log, mt_dns_query_log_stats_t *out);
typedef void (*mt_dns_query_log_notify_fn)(void *ud);
int mt_dns_query_log_subscribe(mt_dns_query_log_t *log, mt_dns_query_log_notify_fn notify, void *ud);
void mt_dns_query_log_unsubscribe(mt_dns_query_log_t *log, int subscription);
uint64_t mt_dns_query_log_sequence(const mt_dns_query_log_t *log);
uint64_t mt_dns_query_log_revision(const mt_dns_query_log_t *log);
#endif
