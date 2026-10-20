/* Included by system.c. No stream read ever enables capture. */
#define DNS_SNAPSHOT_BATCH 32u
#define DNS_SNAPSHOT_MAX_RESTARTS 3u
#define DNS_SNAPSHOT_DEADLINE_MS 30000u

typedef enum { DNS_SNAPSHOT_BEGIN, DNS_SNAPSHOT_ROWS, DNS_SNAPSHOT_END, DNS_LIVE } dns_phase_t;
typedef struct dns_stream {
    mt_dns_query_log_t *log;
    mt_dns_clients_t *clients, *available_clients;
    uint64_t clients_revision;
    size_t clients_count, clients_sent;
    enum { CLIENTS_BEGIN, CLIENTS_ROWS, CLIENTS_END, CLIENTS_IDLE } clients_phase;
    int clients_subscription;
    mt_http_stream_t *http;
    int subscription;
    uint64_t cursor, revision, through, dropped, snapshot_deadline;
    unsigned snapshot_restarts;
    size_t snapshot_count, sent;
    dns_phase_t phase;
    bool capture_known, capturing;
} dns_stream_t;

static char *dns_event(const char *event, cJSON *json, size_t *len) {
    *len = SIZE_MAX;
    if (!json) return NULL;
    char *data = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!data) return NULL;
    size_t size = strlen(data) + strlen(event) + 17;
    if (size > MT_HTTP_STREAM_MAX_FRAME) { free(data); return NULL; }
    char *frame = malloc(size);
    if (frame) {
        int n = snprintf(frame, size, "event: %s\ndata: %s\n\n", event, data);
        if (n < 0 || (size_t)n >= size) { free(frame); frame = NULL; }
        else *len = (size_t)n;
    }
    free(data); return frame;
}
static void dns_stream_notify(void *ud) {
    dns_stream_t *s = ud;
    mt_http_stream_wake(s->http);
}
static uint64_t dns_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) { return 0; }
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
/* Current aliases are separate metadata, not historical ownership of an IP.
 * Changing one name must not replay/re-encode thousands of DNS requests. */
static char *dns_clients_event(dns_stream_t *s, size_t *len) {
    uint64_t revision = mt_dns_clients_revision(s->clients);
    if (revision != s->clients_revision) { s->clients_phase = CLIENTS_BEGIN; }
    cJSON *data = cJSON_CreateObject();
    if (!data) { *len = SIZE_MAX; return NULL; }
    if (s->clients_phase == CLIENTS_BEGIN) {
        s->clients_revision = revision;
        s->clients_count = mt_dns_clients_count(s->clients);
        s->clients_sent = 0;
        s->clients_phase = s->clients_count ? CLIENTS_ROWS : CLIENTS_END;
        if (!cJSON_AddNumberToObject(data, "count", (double)s->clients_count)) { goto fail; }
        return dns_event("clients_begin", data, len);
    }
    if (s->clients_phase == CLIENTS_ROWS) {
        cJSON *rows = cJSON_AddArrayToObject(data, "clients");
        if (!rows || !cJSON_AddNumberToObject(data, "offset", (double)s->clients_sent)) { goto fail; }
        size_t batch = s->clients_count - s->clients_sent;
        if (batch > DNS_SNAPSHOT_BATCH) { batch = DNS_SNAPSHOT_BATCH; }
        for (size_t i = 0; i < batch; i++) {
            const mt_dns_client_t *entry = mt_dns_clients_at(s->clients, s->clients_sent + i);
            cJSON *row = cJSON_CreateObject();
            if (!entry || !row || !cJSON_AddStringToObject(row, "ip", entry->ip) ||
                !cJSON_AddStringToObject(row, "name", entry->name) || !cJSON_AddItemToArray(rows, row)) {
                cJSON_Delete(row); goto fail;
            }
        }
        s->clients_sent += batch;
        if (s->clients_sent == s->clients_count) { s->clients_phase = CLIENTS_END; }
        return dns_event("clients_chunk", data, len);
    }
    s->clients_phase = CLIENTS_IDLE;
    return dns_event("clients_end", data, len);
fail:
    cJSON_Delete(data); *len = SIZE_MAX; return NULL;
}
static char *dns_snapshot_begin(dns_stream_t *s, size_t *len) {
    s->revision = mt_dns_query_log_revision(s->log);
    s->through = mt_dns_query_log_sequence(s->log);
    s->snapshot_count = mt_dns_query_log_size(s->log);
    s->sent = 0;
    s->phase = s->snapshot_count ? DNS_SNAPSHOT_ROWS : DNS_SNAPSHOT_END;
    cJSON *data = cJSON_CreateObject();
    if (!data || !cJSON_AddNumberToObject(data, "capacity", (double)mt_dns_query_log_capacity(s->log)) ||
        !cJSON_AddNumberToObject(data, "count", (double)s->snapshot_count) ||
        !cJSON_AddNumberToObject(data, "through", (double)s->through)) {
        cJSON_Delete(data); *len = SIZE_MAX; return NULL;
    }
    return dns_event("snapshot_begin", data, len);
}
static char *dns_snapshot_start(dns_stream_t *s, size_t *len) {
    s->snapshot_restarts = 0;
    s->snapshot_deadline = dns_now_ms() + DNS_SNAPSHOT_DEADLINE_MS;
    return dns_snapshot_begin(s, len);
}
static char *dns_snapshot_retry(dns_stream_t *s, size_t *len) {
    if (++s->snapshot_restarts > DNS_SNAPSHOT_MAX_RESTARTS) { *len = SIZE_MAX; return NULL; }
    return dns_snapshot_begin(s, len);
}
static char *dns_stream_next(void *ud, size_t *len) {
    dns_stream_t *s = ud;
    uint64_t sequence = mt_dns_query_log_sequence(s->log);
    uint64_t revision = mt_dns_query_log_revision(s->log);
    size_t count = mt_dns_query_log_size(s->log);
    bool enabled = mt_dns_query_log_enabled(s->log);
    *len = 0;
    if (!s->capture_known || s->capturing != enabled) {
        s->capture_known = true; s->capturing = enabled;
        s->phase = DNS_SNAPSHOT_BEGIN;
        s->clients_phase = CLIENTS_BEGIN;
        /* Read-only viewers do not trigger inventory lookup while disabled. */
        if (!enabled && s->clients_subscription) {
            mt_dns_clients_unsubscribe(s->clients, s->clients_subscription);
            s->clients = NULL; s->clients_subscription = 0;
        } else if (enabled && s->available_clients && !s->clients_subscription) {
            s->clients_subscription = mt_dns_clients_subscribe(s->available_clients, dns_stream_notify, s);
            if (s->clients_subscription) s->clients = s->available_clients;
        }
        mt_dns_query_log_stats_t stats;
        mt_dns_query_log_stats(s->log, &stats);
        s->dropped = stats.dropped;
        return dns_event("capture", dns_capture_json(s->log), len);
    }
    mt_dns_query_log_stats_t stats;
    mt_dns_query_log_stats(s->log, &stats);
    if (s->dropped != stats.dropped) {
        s->dropped = stats.dropped;
        return dns_event("capture", dns_capture_json(s->log), len);
    }
    if (s->clients_phase != CLIENTS_IDLE || s->clients_revision != mt_dns_clients_revision(s->clients)) {
        return dns_clients_event(s, len);
    }
    if (s->phase == DNS_SNAPSHOT_BEGIN) { return dns_snapshot_start(s, len); }
    if (s->phase != DNS_LIVE && dns_now_ms() >= s->snapshot_deadline) { *len = SIZE_MAX; return NULL; }
    if (revision != s->revision) {
        if (s->phase != DNS_LIVE) return dns_snapshot_start(s, len);
        s->revision = revision; s->cursor = sequence - count;
        return dns_event("clear", dns_capture_json(s->log), len);
    }
    if (s->phase == DNS_SNAPSHOT_ROWS) {
        uint64_t first = s->through - s->snapshot_count + s->sent + 1;
        if (first <= sequence - count) return dns_snapshot_retry(s, len);
        cJSON *data = cJSON_CreateObject();
        cJSON *rows = data ? cJSON_AddArrayToObject(data, "queries") : NULL;
        if (!rows || !cJSON_AddNumberToObject(data, "offset", (double)s->sent)) {
            cJSON_Delete(data); *len = SIZE_MAX; return NULL;
        }
        size_t batch = s->snapshot_count - s->sent;
        if (batch > DNS_SNAPSHOT_BATCH) batch = DNS_SNAPSHOT_BATCH;
        for (size_t i = 0; i < batch; i++) {
            mt_dns_query_entry_t entry;
            if (!mt_dns_query_log_get_id(s->log, first + i, &entry)) {
                cJSON_Delete(data); return dns_snapshot_retry(s, len);
            }
            cJSON *row = dns_query_json(&entry, NULL);
            if (!row || !cJSON_AddItemToArray(rows, row)) {
                cJSON_Delete(row); cJSON_Delete(data); *len = SIZE_MAX; return NULL;
            }
        }
        s->sent += batch;
        if (s->sent == s->snapshot_count) s->phase = DNS_SNAPSHOT_END;
        return dns_event("snapshot_chunk", data, len);
    }
    if (s->phase == DNS_SNAPSHOT_END) {
        if (sequence - s->through > count) return dns_snapshot_retry(s, len);
        cJSON *data = cJSON_CreateObject();
        if (!data || !cJSON_AddNumberToObject(data, "through", (double)s->through)) {
            cJSON_Delete(data); *len = SIZE_MAX; return NULL;
        }
        s->cursor = s->through; s->phase = DNS_LIVE;
        return dns_event("snapshot_end", data, len);
    }
    if (sequence - s->cursor > count) return dns_snapshot_start(s, len);
    if (s->cursor == sequence) return NULL;
    mt_dns_query_entry_t entry;
    if (!mt_dns_query_log_get_id(s->log, s->cursor + 1, &entry)) { *len = SIZE_MAX; return NULL; }
    s->cursor = entry.id;
    cJSON *row = dns_query_json(&entry, NULL);
    /* Byte pressure can evict before the slot limit: viewers discard the
     * same oldest IDs, rather than keeping rows already gone on the router. */
    if (!row || !cJSON_AddNumberToObject(row, "retained_from", (double)(sequence - count + 1)) ||
        !cJSON_AddNumberToObject(row, "memory_used", (double)stats.allocated_bytes)) {
        cJSON_Delete(row); *len = SIZE_MAX; return NULL;
    }
    return dns_event("query", row, len);
}
static void dns_stream_closed(void *ud) {
    dns_stream_t *s = ud;
    mt_dns_query_log_unsubscribe(s->log, s->subscription);
    mt_dns_clients_unsubscribe(s->clients, s->clients_subscription);
    free(s);
}
static void handle_dns_stream(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_system_ctx_t *ctx = ud;
    if (!dns_require_log(ctx->query_log, res)) { return; }
    dns_stream_t *s = calloc(1, sizeof(*s));
    if (!s) { mt_http_res_write_error(res, 503, "out of memory"); return; }
    s->log = ctx->query_log; s->available_clients = ctx->dns_clients;
    s->subscription = mt_dns_query_log_subscribe(s->log, dns_stream_notify, s);
    if (!s->subscription) { free(s); mt_http_res_write_error(res, 503, "DNS stream subscriber limit reached"); return; }
    s->http = mt_http_stream_open(req, res, dns_stream_next, dns_stream_closed, s);
    if (!s->http) { dns_stream_closed(s); mt_http_res_write_error(res, 503, "cannot start DNS stream"); }
}
