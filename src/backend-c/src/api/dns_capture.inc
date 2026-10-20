/* Included by system.c. Capture consent is runtime state, never config. */
static cJSON *dns_capture_json(mt_dns_query_log_t *log) {
    mt_dns_query_log_stats_t stats;
    mt_dns_query_log_stats(log, &stats);
    cJSON *out = cJSON_CreateObject();
    if (!out || !cJSON_AddBoolToObject(out, "enabled", mt_dns_query_log_enabled(log)) ||
        !cJSON_AddNumberToObject(out, "memory_limit", (double)mt_dns_query_log_memory_limit(log)) ||
        !cJSON_AddNumberToObject(out, "memory_used", (double)stats.allocated_bytes) ||
        !cJSON_AddNumberToObject(out, "count", (double)stats.count) ||
        !cJSON_AddNumberToObject(out, "capacity", (double)stats.capacity)) {
        cJSON_Delete(out); return NULL;
    }
    return out;
}
static void handle_dns_capture(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_system_ctx_t *ctx = ud;
    if (strcmp(mt_http_req_method(req), "PUT") == 0) {
        size_t len = 0;
        const uint8_t *body = mt_http_req_body(req, &len);
        if (!body || !len || len > 128 || memchr(body, '\0', len)) {
            mt_http_res_write_error(res, 400, "expected {\"enabled\":true|false}"); return;
        }
        const char *end = NULL;
        cJSON *value = cJSON_ParseWithLengthOpts((const char *)body, len, &end, false);
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(value, "enabled");
        while (end && end < (const char *)body + len &&
            (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t')) end++;
        bool valid = cJSON_IsObject(value) && cJSON_GetArraySize(value) == 1 &&
            cJSON_IsBool(enabled) && end == (const char *)body + len;
        bool start = cJSON_IsTrue(enabled);
        cJSON_Delete(value);
        if (!valid) { mt_http_res_write_error(res, 400, "expected {\"enabled\":true|false}"); return; }
        if (!mt_dns_query_log_set_enabled(ctx->query_log, start)) {
            mt_http_res_write_error(res, 503, "cannot allocate DNS history within its memory budget"); return;
        }
    }
    cJSON *out = dns_capture_json(ctx->query_log);
    if (!out) { mt_http_res_write_error(res, 500, "out of memory"); return; }
    mt_http_res_set_header(res, "Cache-Control", "no-store");
    mt_http_res_write_json(res, 200, out);
}
