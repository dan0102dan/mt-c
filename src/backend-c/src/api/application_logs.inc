/* Included by system.c. Global recording is opt-in; these reads never start it.
 * HTTP backpressure retains one small frame, not another copy of the journal. */
#include "magitrickle/log_history.h"
#include "magitrickle/hash.h"

#define APPLICATION_LOG_CHUNK (12u * 1024u)
typedef struct application_log_stream {
    mt_http_stream_t *http;
    int pipes[2], subscription;
    char session[MT_LOG_SESSION_SIZE];
    uint64_t cursor, revision, through, dropped;
    size_t offset;
    bool known, ready;
} application_log_stream_t;

static bool logs_add_u64(cJSON *json, const char *key, uint64_t number) {
    char text[21];
    snprintf(text, sizeof(text), "%" PRIu64, number);
    return cJSON_AddStringToObject(json, key, text) != NULL;
}
static cJSON *application_capture_json(const mt_log_history_state_t *state) {
    cJSON *json = cJSON_CreateObject();
    if (!json || !cJSON_AddBoolToObject(json, "enabled", state->enabled) ||
        !cJSON_AddStringToObject(json, "session", state->session) ||
        !logs_add_u64(json, "through", state->count) ||
        !logs_add_u64(json, "dropped", state->dropped)) {
        cJSON_Delete(json); return NULL;
    }
    return json;
}
static char *application_log_frame(const char *event, cJSON *json, size_t *len) {
    *len = SIZE_MAX;
    if (!json) return NULL;
    char *data = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!data) return NULL;
    size_t cap = strlen(event) + strlen(data) + 17;
    if (cap > MT_HTTP_STREAM_MAX_FRAME) { free(data); return NULL; }
    char *frame = malloc(cap);
    if (frame) {
        int n = snprintf(frame, cap, "event: %s\ndata: %s\n\n", event, data);
        if (n < 0 || (size_t)n >= cap) { free(frame); frame = NULL; }
        else *len = (size_t)n;
    }
    free(data);
    return frame;
}

static void handle_application_capture(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)ud;
    if (strcmp(mt_http_req_method(req), "PUT") == 0) {
        size_t len;
        const uint8_t *body = mt_http_req_body(req, &len);
        cJSON *json = body && len && len <= 128 && !memchr(body, '\0', len)
            ? mt_json_parse_document((const char *)body, len) : NULL;
        cJSON *enabled = cJSON_GetObjectItemCaseSensitive(json, "enabled");
        if (!cJSON_IsObject(json) || cJSON_GetArraySize(json) != 1 || !cJSON_IsBool(enabled)) {
            cJSON_Delete(json);
            mt_http_res_write_error(res, 400, "expected only a boolean enabled field");
            return;
        }
        bool start = cJSON_IsTrue(enabled);
        cJSON_Delete(json);
        if (!mt_log_history_enable(start)) {
            mt_http_res_write_error(res, 503, "cannot start application log recording");
            return;
        }
    }
    mt_log_history_state_t state;
    mt_log_history_state(&state);
    cJSON *json = application_capture_json(&state);
    if (!json) { mt_http_res_write_error(res, 503, "capture status unavailable"); return; }
    mt_http_res_set_header(res, "Cache-Control", "no-store");
    mt_http_res_write_json(res, 200, json);
}

static char *application_log_next(void *ud, size_t *len) {
    application_log_stream_t *s = ud;
    /* Ack before reading current state: a writer racing after this point
     * leaves a new wake byte. The pipe callback itself only wakes the loop. */
    mt_log_history_ack(s->subscription);
    mt_log_history_state_t state;
    mt_log_history_state(&state);
    *len = 0;
    if (!s->known || state.revision != s->revision) {
        if (strcmp(s->session, state.session) != 0 || !state.enabled) s->cursor = 0;
        memcpy(s->session, state.session, sizeof(s->session));
        s->known = true; s->revision = state.revision; s->offset = 0;
        s->through = state.count; s->ready = false; s->dropped = state.dropped;
        return application_log_frame("capture", application_capture_json(&state), len);
    }
    if (s->dropped != state.dropped) {
        s->dropped = state.dropped;
        return application_log_frame("capture", application_capture_json(&state), len);
    }
    if (!s->ready && s->cursor >= s->through && s->offset == 0) {
        cJSON *json = cJSON_CreateObject();
        if (!json || !cJSON_AddStringToObject(json, "session", s->session) ||
            !logs_add_u64(json, "through", s->through)) {
            cJSON_Delete(json); *len = SIZE_MAX; return NULL;
        }
        s->ready = true;
        return application_log_frame("ready", json, len);
    }
    if (!state.enabled || s->cursor >= state.count) return NULL;
    uint8_t bytes[APPLICATION_LOG_CHUNK];
    mt_log_history_piece_t piece;
    if (!mt_log_history_read(s->session, s->cursor + 1, s->offset,
                             bytes, sizeof(bytes), &piece)) {
        /* Stop/new session wins a race: the state notification will wake us. */
        return NULL;
    }
    char encoded[(APPLICATION_LOG_CHUNK / 3u) * 4u + 1u];
    mt_base64_encode(bytes, piece.copied, encoded);
    bool final = s->offset + piece.copied == piece.length;
    cJSON *json = cJSON_CreateObject();
    if (!json || !cJSON_AddStringToObject(json, "session", s->session) ||
        !logs_add_u64(json, "id", s->cursor + 1) ||
        !cJSON_AddNumberToObject(json, "offset", (double)s->offset) ||
        !cJSON_AddNumberToObject(json, "length", (double)piece.length) ||
        !cJSON_AddNumberToObject(json, "level", (double)piece.level) ||
        !cJSON_AddBoolToObject(json, "end", final) ||
        !cJSON_AddStringToObject(json, "data", encoded)) {
        cJSON_Delete(json); *len = SIZE_MAX; return NULL;
    }
    char *frame = application_log_frame("log", json, len);
    if (frame) {
        s->offset += piece.copied;
        if (final) { s->cursor++; s->offset = 0; }
    }
    return frame;
}
static void application_log_closed(void *ud) {
    application_log_stream_t *s = ud;
    mt_log_history_unsubscribe(s->subscription);
    if (s->pipes[1] >= 0) close(s->pipes[1]);
    if (s->pipes[0] >= 0) close(s->pipes[0]);
    free(s);
}
static bool application_cursor(const char *text, uint64_t *out) {
    if (!text || !*text || (text[0] == '0' && text[1])) return false;
    uint64_t value = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p < '0' || *p > '9' || value > (UINT64_MAX - (unsigned)(*p - '0')) / 10) return false;
        value = value * 10 + (unsigned)(*p - '0');
    }
    *out = value;
    return true;
}
static bool application_session(const char *text) {
    if (!text || strlen(text) != 32) return false;
    return strspn(text, "0123456789abcdef") == 32;
}
static void handle_application_stream(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)ud;
    const char *session = mt_http_req_query(req, "session");
    const char *after = mt_http_req_query(req, "after");
    uint64_t cursor = 0;
    if ((session || after) && (!application_session(session) || !application_cursor(after, &cursor))) {
        mt_http_res_write_error(res, 400, "invalid log session or cursor"); return;
    }
    mt_log_history_state_t state;
    mt_log_history_state(&state);
    if (session && state.enabled && strcmp(session, state.session) == 0 && cursor > state.count) {
        mt_http_res_write_error(res, 400, "log cursor is ahead of the journal"); return;
    }
    application_log_stream_t *s = calloc(1, sizeof(*s));
    if (!s) { mt_http_res_write_error(res, 503, "out of memory"); return; }
    s->pipes[0] = s->pipes[1] = -1;
    if (session) { memcpy(s->session, session, 33); s->cursor = cursor; }
    if (pipe(s->pipes) != 0) goto fail;
    for (size_t i = 0; i < 2; i++) {
        int flags = fcntl(s->pipes[i], F_GETFL);
        if (flags < 0 || fcntl(s->pipes[i], F_SETFL, flags | O_NONBLOCK) < 0 ||
            fcntl(s->pipes[i], F_SETFD, FD_CLOEXEC) < 0) goto fail;
    }
    s->subscription = mt_log_history_subscribe(s->pipes[1]);
    if (!s->subscription) goto fail;
    s->http = mt_http_stream_open(req, res, application_log_next, application_log_closed, s);
    if (!s->http) goto fail;
    mt_http_stream_watch_fd(s->http, s->pipes[0]);
    return;
fail:
    application_log_closed(s);
    mt_http_res_write_error(res, 503, "cannot open application log stream");
}
static void application_logs_register(mt_httpd_t *h) {
    must_route(h, "GET", "/api/v1/logs/capture", handle_application_capture, NULL);
    must_route(h, "PUT", "/api/v1/logs/capture", handle_application_capture, NULL);
    must_route(h, "GET", "/api/v1/logs/stream", handle_application_stream, NULL);
}
