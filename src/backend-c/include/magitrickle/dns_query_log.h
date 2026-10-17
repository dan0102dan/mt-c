/* Bounded, volatile DNS request history. Accessed only on mt_loop's thread.
 * Contains request metadata, never DNS response payloads or packet contents.
 * Never written to persistent storage or exported in router configuration.
 */
#ifndef MAGITRICKLE_DNS_QUERY_LOG_H
#define MAGITRICKLE_DNS_QUERY_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MT_DNS_QUERY_LOG_CAPACITY 1000
/* mt_dns_name_to_string may expand escaped bytes to four chars each. */
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

mt_dns_query_log_t *mt_dns_query_log_create(void);
void mt_dns_query_log_destroy(mt_dns_query_log_t *log);
void mt_dns_query_log_append(mt_dns_query_log_t *log, const char *client,
                             const char *domain, uint16_t qtype, bool tcp,
                             int64_t time_ms);
/* Newest first. Returns false if index >= size. */
size_t mt_dns_query_log_size(const mt_dns_query_log_t *log);
bool mt_dns_query_log_get(const mt_dns_query_log_t *log, size_t newest_index,
                          mt_dns_query_entry_t *out);
void mt_dns_query_log_clear(mt_dns_query_log_t *log);

/* Loop-local notifications: callbacks may unsubscribe themselves, but must
 * not mutate the journal. Stop all subscribers before destroying the log. */
typedef void (*mt_dns_query_log_notify_fn)(void *ud);
int mt_dns_query_log_subscribe(mt_dns_query_log_t *log,
    mt_dns_query_log_notify_fn notify, void *ud);
void mt_dns_query_log_unsubscribe(mt_dns_query_log_t *log, int subscription);
uint64_t mt_dns_query_log_sequence(const mt_dns_query_log_t *log);
uint64_t mt_dns_query_log_revision(const mt_dns_query_log_t *log);
#endif /* MAGITRICKLE_DNS_QUERY_LOG_H */
