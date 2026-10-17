/* Included by system.c; shared JSON conversion is private to that module. */
typedef struct dns_stream {
    mt_dns_query_log_t *log;
    mt_http_stream_t *http;
    int subscription;
    uint64_t cursor;
    uint64_t revision;
    bool initialized;
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

static char *dns_stream_next(void *ud, size_t *len) {
    dns_stream_t *s = ud;
    uint64_t sequence = mt_dns_query_log_sequence(s->log);
    uint64_t revision = mt_dns_query_log_revision(s->log);
    size_t count = mt_dns_query_log_size(s->log);
    *len = 0;
    if (!s->initialized ||
        (revision == s->revision && sequence - s->cursor > count)) {
        /* Also resynchronizes a slow reader after ring eviction. A fresh
         * connection always starts with a replacement snapshot, so stale
         * Last-Event-ID values across restarts cannot hide newer entries. */
        s->initialized = true;
        s->cursor = sequence;
        s->revision = revision;
        return dns_event("snapshot", dns_snapshot(s->log), len);
    }
    if (revision != s->revision) {
        s->revision = revision;
        s->cursor = sequence - count;
        return dns_event("clear", cJSON_CreateObject(), len);
    }
    if (s->cursor == sequence) { return NULL; }
    mt_dns_query_entry_t entry;
    size_t index = (size_t)(sequence - s->cursor - 1);
    if (!mt_dns_query_log_get(s->log, index, &entry)) {
        *len = SIZE_MAX;
        return NULL;
    }
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
        free(s);
        mt_http_res_write_error(res, 503, "DNS stream subscriber limit reached");
        return;
    }
    s->http = mt_http_stream_open(req, res, dns_stream_next, dns_stream_closed, s);
    if (!s->http) {
        dns_stream_closed(s);
        mt_http_res_write_error(res, 503, "cannot start DNS stream");
    }
}
