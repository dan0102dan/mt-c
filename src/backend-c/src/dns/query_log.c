#include "magitrickle/dns_query_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mt_dns_query_log {
    mt_dns_query_entry_t entries[MT_DNS_QUERY_LOG_CAPACITY];
    size_t next;
    size_t count;
    uint64_t sequence;
};

mt_dns_query_log_t *mt_dns_query_log_create(void)
{
    return calloc(1, sizeof(mt_dns_query_log_t));
}

void mt_dns_query_log_destroy(mt_dns_query_log_t *log)
{
    if (!log) { return; }
    /* Avoid leaving DNS history in freed heap pages longer than necessary. */
    memset(log, 0, sizeof(*log));
    free(log);
}

void mt_dns_query_log_append(mt_dns_query_log_t *log, const char *client,
                             const char *domain, uint16_t qtype, bool tcp,
                             int64_t time_ms)
{
    if (!log || !domain || !*domain) { return; }
    mt_dns_query_entry_t *entry = &log->entries[log->next];
    memset(entry, 0, sizeof(*entry));
    entry->id = ++log->sequence;
    entry->time_ms = time_ms;
    entry->qtype = qtype;
    entry->tcp = tcp;
    snprintf(entry->client, sizeof(entry->client), "%s", client ? client : "unknown");
    snprintf(entry->domain, sizeof(entry->domain), "%s", domain);
    log->next = (log->next + 1) % MT_DNS_QUERY_LOG_CAPACITY;
    if (log->count < MT_DNS_QUERY_LOG_CAPACITY) { log->count++; }
}

size_t mt_dns_query_log_size(const mt_dns_query_log_t *log)
{
    return log ? log->count : 0;
}

bool mt_dns_query_log_get(const mt_dns_query_log_t *log, size_t newest_index,
                          mt_dns_query_entry_t *out)
{
    if (!log || !out || newest_index >= log->count) { return false; }
    const size_t slot =
        (log->next + MT_DNS_QUERY_LOG_CAPACITY - 1 - newest_index) %
        MT_DNS_QUERY_LOG_CAPACITY;
    *out = log->entries[slot];
    return true;
}

void mt_dns_query_log_clear(mt_dns_query_log_t *log)
{
    if (!log) { return; }
    memset(log->entries, 0, sizeof(log->entries));
    log->next = 0;
    log->count = 0;
    /* Keep sequence monotonic across clears so an old id is never reused. */
}
