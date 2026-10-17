/* Included by system.c; bounded snapshot chunks share its JSON conversion. */
#define DNS_SNAPSHOT_BATCH 32u

typedef enum { DNS_SNAPSHOT_BEGIN, DNS_SNAPSHOT_ROWS, DNS_SNAPSHOT_END, DNS_LIVE } dns_phase_t;
typedef struct dns_stream {
    mt_dns_query_log_t *log;
    mt_http_stream_t *http;
    int subscription;
    uint64_t cursor, revision, through;
    size_t snapshot_count, sent;
    dns_phase_t phase;
} dns_stream_t;

static char *dns_event(const char *event, cJSON *json, size_t *len) {
    *len = SIZE_MAX;
    if (!json) { return NULL; }
    char *data = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!data) { return NULL; }
    size_t size = strlen(data) + strlen(event) + 17;
    if (size > MT_HTTP_STREAM_MAX_FRAME) { free(data); return NULL; }
    char *frame = malloc(size);
    if (frame) {
        int n = snprintf(frame, size, "event: %s\ndata: %s\n\n", event, data);
        if (n < 0 || (size_t)n >= size) { free(frame); frame = NULL; }
        else { *len = (size_t)n; }
    }
    free(data);
    return frame;
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

static char *dns_stream_next(void *ud, size_t *len) {
    dns_stream_t *s = ud;
    uint64_t sequence = mt_dns_query_log_sequence(s->log);
    uint64_t revision = mt_dns_query_log_revision(s->log);
    size_t count = mt_dns_query_log_size(s->log);
    *len = 0;
    if (s->phase == DNS_SNAPSHOT_BEGIN) { return dns_snapshot_begin(s, len); }
    if (revision != s->revision) {
        if (s->phase != DNS_LIVE) {
            /* Abandon the incomplete snapshot. The next begin resets the
             * browser's staging area, never mixing two generations. */
            return dns_snapshot_begin(s, len);
        }
        s->revision = revision;
        s->cursor = sequence - count;
        return dns_event("clear", cJSON_CreateObject(), len);
    }
    if (s->phase == DNS_SNAPSHOT_ROWS) {
        uint64_t first = s->through - s->snapshot_count + s->sent + 1;
        if (first <= sequence - count) { return dns_snapshot_begin(s, len); }
        cJSON *data = cJSON_CreateObject();
        cJSON *rows = data ? cJSON_AddArrayToObject(data, "queries") : NULL;
        if (!rows || !cJSON_AddNumberToObject(data, "offset", (double)s->sent)) {
            cJSON_Delete(data); *len = SIZE_MAX; return NULL;
        }
        size_t batch = s->snapshot_count - s->sent;
        if (batch > DNS_SNAPSHOT_BATCH) { batch = DNS_SNAPSHOT_BATCH; }
        for (size_t i = 0; i < batch; i++) {
            mt_dns_query_entry_t entry;
            if (!mt_dns_query_log_get_id(s->log, first + i, &entry)) {
                cJSON_Delete(data); return dns_snapshot_begin(s, len);
            }
            cJSON *row = dns_query_json(&entry);
            if (!row || !cJSON_AddItemToArray(rows, row)) {
                cJSON_Delete(row); cJSON_Delete(data); *len = SIZE_MAX; return NULL;
            }
        }
        s->sent += batch;
        if (s->sent == s->snapshot_count) { s->phase = DNS_SNAPSHOT_END; }
        return dns_event("snapshot_chunk", data, len);
    }
    if (s->phase == DNS_SNAPSHOT_END) {
        /* We must still be able to catch up from the chosen high-water mark. */
        if (sequence - s->through > count) { return dns_snapshot_begin(s, len); }
        cJSON *data = cJSON_CreateObject();
        if (!data || !cJSON_AddNumberToObject(data, "through", (double)s->through)) {
            cJSON_Delete(data); *len = SIZE_MAX; return NULL;
        }
        s->cursor = s->through; s->phase = DNS_LIVE;
        return dns_event("snapshot_end", data, len);
    }
    if (sequence - s->cursor > count) { return dns_snapshot_begin(s, len); }
    if (s->cursor == sequence) { return NULL; }
    mt_dns_query_entry_t entry;
    if (!mt_dns_query_log_get_id(s->log, s->cursor + 1, &entry)) { *len = SIZE_MAX; return NULL; }
    s->cursor = entry.id;
    return dns_event("query", dns_query_json(&entry), len);
}

static void dns_stream_notify(void *ud) {
    dns_stream_t *s = ud;
    mt_http_stream_wake(s->http);
}
static void dns_stream_closed(void *ud) {
    dns_stream_t *s = ud;
    mt_dns_query_log_unsubscribe(s->log, s->subscription);
    free(s);
}
static void handle_dns_stream(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_system_ctx_t *ctx = ud;
    dns_stream_t *s = calloc(1, sizeof(*s));
    if (!s) { mt_http_res_write_error(res, 503, "out of memory"); return; }
    s->log = ctx->query_log;
    s->subscription = mt_dns_query_log_subscribe(s->log, dns_stream_notify, s);
    if (!s->subscription) {
        free(s); mt_http_res_write_error(res, 503, "DNS stream subscriber limit reached"); return;
    }
    s->http = mt_http_stream_open(req, res, dns_stream_next, dns_stream_closed, s);
    if (!s->http) {
        dns_stream_closed(s); mt_http_res_write_error(res, 503, "cannot start DNS stream");
    }
}
