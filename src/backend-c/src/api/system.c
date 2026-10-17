/* See system.h. Port of handlers.go's ListInterfaces/SaveConfig/
 * NetfilterDHook. */
#include "magitrickle/system.h"
#include "magitrickle/profiles_api.h"
#include "magitrickle/update.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <yaml.h>

#include <cjson/cJSON.h>

#include "magitrickle/log.h"

static void must_route(mt_httpd_t *h, const char *method, const char *pattern, mt_http_handler_fn fn,
                       void *ud) {
    if (mt_httpd_route(h, method, pattern, fn, ud) != MT_OK) {
        MT_ERROR("failed to register route %s %s", method, pattern);
    }
}

static void handle_list_interfaces(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req;
    mt_system_ctx_t *ctx = ud;

    mt_iface_info_t *ifaces = NULL;
    size_t n = 0;
    mt_err_t err = mt_app_list_interfaces(ctx->app, &ifaces, &n);
    if (err != MT_OK) {
        mt_http_res_write_error(res, 500, mt_err_str(err));
        return;
    }

    cJSON *out = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(out, "interfaces");
    cJSON *blackhole = cJSON_CreateObject();
    cJSON_AddStringToObject(blackhole, "id", "blackhole");
    cJSON_AddItemToArray(arr, blackhole);
    for (size_t i = 0; i < n; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", ifaces[i].id);
#ifdef MT_ENTWARE_KN
        /* Keep the kernel ID; the optional RCI alias only enriches KN responses. */
        if (ifaces[i].name[0] != '\0') { cJSON_AddStringToObject(item, "name", ifaces[i].name); }
#endif
        cJSON_AddItemToArray(arr, item);
    }
    free(ifaces);
    mt_http_res_write_json(res, 200, out);
}

static void handle_save_config(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req;
    mt_system_ctx_t *ctx = ud;
    if (!ctx->config_path) {
        mt_http_res_write(res, 200, NULL, NULL, 0);
        return;
    }
    mt_err_t err =
        mt_app_save_config(ctx->app, ctx->config_path, ctx->config_version ? ctx->config_version : "");
    if (err != MT_OK) {
        mt_http_res_write_error(res, 500, mt_err_str(err));
        return;
    }
    mt_http_res_write(res, 200, NULL, NULL, 0);
}


/* The portable transfer API works with the live in-memory model, not the
 * possibly stale config file. A revision token binds preview and apply.
 * The serialized candidate is staged and validated before the running app
 * is swapped; on persistence failure the previous live state is reloaded. */
static const char *transfer_version(const mt_system_ctx_t *ctx) {
    const char *version = ctx->config_version;
    return version && strncmp(version, "0.", 2) == 0 ? version : "0.0.0";
}

static void config_revision(const char *data, size_t size, char out[17]) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; i++) {
        hash ^= (unsigned char)data[i];
        hash *= UINT64_C(1099511628211);
    }
    snprintf(out, 17, "%016" PRIx64, hash);
}

static cJSON *yaml_value_to_json(yaml_document_t *doc, yaml_node_t *node) {
    if (!node) return NULL;
    if (node->type == YAML_SEQUENCE_NODE) {
        cJSON *out = cJSON_CreateArray();
        if (!out) return NULL;
        for (yaml_node_item_t *item = node->data.sequence.items.start;
             item < node->data.sequence.items.top; item++) {
            cJSON *value = yaml_value_to_json(doc, yaml_document_get_node(doc, *item));
            if (!value || !cJSON_AddItemToArray(out, value)) {
                cJSON_Delete(value); cJSON_Delete(out); return NULL;
            }
        }
        return out;
    }
    if (node->type == YAML_MAPPING_NODE) {
        cJSON *out = cJSON_CreateObject();
        if (!out) return NULL;
        for (yaml_node_pair_t *entry = node->data.mapping.pairs.start;
             entry < node->data.mapping.pairs.top; entry++) {
            yaml_node_t *key = yaml_document_get_node(doc, entry->key);
            if (!key || key->type != YAML_SCALAR_NODE) { cJSON_Delete(out); return NULL; }
            const char *name = (const char *)key->data.scalar.value;
            cJSON *value = yaml_value_to_json(doc, yaml_document_get_node(doc, entry->value));
            if (!value || !cJSON_AddItemToObject(out, name, value)) {
                cJSON_Delete(value); cJSON_Delete(out); return NULL;
            }
        }
        return out;
    }
    if (node->type != YAML_SCALAR_NODE) return NULL;
    const char *value = (const char *)node->data.scalar.value;
    if (node->data.scalar.style == YAML_PLAIN_SCALAR_STYLE) {
        if (strcmp(value, "true") == 0) return cJSON_CreateBool(true);
        if (strcmp(value, "false") == 0) return cJSON_CreateBool(false);
        /* Numeric IDs are emitted as quoted strings by mt_config_save_buffer.
         * Only canonical unsigned decimal values fit for JSON numbers. */
        if (*value >= '0' && *value <= '9') {
            const char *p = value;
            while (*p >= '0' && *p <= '9') p++;
            if (!*p && (size_t)(p-value) < 16) return cJSON_CreateNumber(strtod(value,NULL));
        }
        if (strcmp(value, "null") == 0) return cJSON_CreateNull();
    }
    return cJSON_CreateString(value);
}

static cJSON *snapshot_json(const char *text, size_t length) {
    yaml_parser_t parser;
    yaml_document_t doc;
    if (!yaml_parser_initialize(&parser)) return NULL;
    yaml_parser_set_input_string(&parser, (const unsigned char *)text, length);
    int ok = yaml_parser_load(&parser, &doc);
    yaml_parser_delete(&parser);
    if (!ok) return NULL;
    cJSON *result = yaml_value_to_json(&doc, yaml_document_get_root_node(&doc));
    yaml_document_delete(&doc);
    return result;
}

static int write_stage(const char *content, size_t length, char path[32]) {
    strcpy(path, "/tmp/mt-c-import-XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    size_t done = 0;
    while (done < length) {
        ssize_t n = write(fd, content + done, length - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); unlink(path); return -1; }
        done += (size_t)n;
    }
    int sync_status = fsync(fd);
    int close_status = close(fd);
    if (sync_status != 0 || close_status != 0) { unlink(path); return -1; }
    return 0;
}

static void handle_config_snapshot(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req;
    mt_system_ctx_t *ctx = ud;
    char *yaml = NULL;
    size_t length = 0;
    mt_err_t err = mt_app_config_snapshot(ctx->app,
        transfer_version(ctx), &yaml, &length);
    if (err != MT_OK) { mt_http_res_write_error(res, 500, mt_err_str(err)); return; }
    char revision[17];
    config_revision(yaml, length, revision);
    cJSON *config = snapshot_json(yaml, length);
    free(yaml);
    if (!config) { mt_http_res_write_error(res, 500, "failed to encode configuration"); return; }
    cJSON *out = cJSON_CreateObject();
    if (!out) { cJSON_Delete(config); mt_http_res_write_error(res, 500, "out of memory"); return; }
    cJSON_AddStringToObject(out, "revision", revision);
    cJSON_AddItemToObject(out, "config", config);
    mt_http_res_write_json(res, 200, out);
}

static void handle_config_apply(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_system_ctx_t *ctx = ud;
    if (!ctx->config_path) {
        mt_http_res_write_error(res, 503, "configuration persistence unavailable");
        return;
    }
    size_t body_len;
    const uint8_t *body = mt_http_req_body(req, &body_len);
    if (!body || body_len == 0 || body_len > 16u*1024u*1024u ||
        memchr(body, '\0', body_len)) {
        mt_http_res_write_error(res, 400, "invalid import request");
        return;
    }
    cJSON *request = cJSON_ParseWithLength((const char *)body, body_len);
    const cJSON *base = cJSON_GetObjectItemCaseSensitive(request, "revision");
    const cJSON *config = cJSON_GetObjectItemCaseSensitive(request, "config");
    if (!cJSON_IsObject(request) || !cJSON_IsString(base) ||
        !cJSON_IsObject(config) ||
        !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(config,"app")) ||
        !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(config,"groups")) ||
        !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(config,"subscriptions")) ||
        !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(config,"profiles"))) {
        cJSON_Delete(request);
        mt_http_res_write_error(res, 400, "complete configuration and revision required");
        return;
    }
#ifndef MT_ENTWARE_KN
    /* Generic app YAML has exactly six recognized top-level settings.
     * Reject extra platform-only settings instead of silently dropping them. */
    if (cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(config, "app")) > 6) {
        cJSON_Delete(request);
        mt_http_res_write_error(res, 409, "Keenetic policy is unsupported on this platform");
        return;
    }
#endif
    char *old_yaml = NULL;
    size_t old_len = 0;
    mt_err_t err = mt_app_config_snapshot(ctx->app,
        transfer_version(ctx), &old_yaml, &old_len);
    if (err != MT_OK) {
        cJSON_Delete(request);
        mt_http_res_write_error(res, 500, "failed to snapshot current state");
        return;
    }
    char revision[17];
    config_revision(old_yaml, old_len, revision);
    if (strcmp(base->valuestring, revision) != 0) {
        cJSON_Delete(request); free(old_yaml);
        mt_http_res_write_error(res, 409, "configuration changed; refresh import preview");
        return;
    }
    cJSON *document = cJSON_Duplicate(config, 1);
    if (!document) { cJSON_Delete(request); free(old_yaml); mt_http_res_write_error(res, 500, "out of memory"); return; }
    cJSON_DeleteItemFromObjectCaseSensitive(document, "configVersion");
    cJSON_AddStringToObject(document, "configVersion",
        transfer_version(ctx));
    char *serialized = cJSON_PrintUnformatted(document);
    cJSON_Delete(document);
    cJSON_Delete(request);
    if (!serialized) {
        free(old_yaml); mt_http_res_write_error(res, 500, "out of memory"); return;
    }
    mt_config_t candidate = {0};
    err = mt_config_init_defaults(&candidate);
    if (err == MT_OK) err = mt_config_load_buffer(&candidate, serialized, strlen(serialized));
    mt_config_clear(&candidate);
    if (err != MT_OK) {
        free(serialized); free(old_yaml);
        mt_http_res_write_error(res, 400, "invalid or unsupported configuration");
        return;
    }
    char new_path[32] = {0}, rollback_path[32] = {0};
    bool staged = write_stage(serialized, strlen(serialized), new_path) == 0;
    bool backed_up = write_stage(old_yaml, old_len, rollback_path) == 0;
    free(serialized); free(old_yaml);
    if (!staged || !backed_up) {
        if (staged) unlink(new_path);
        if (backed_up) unlink(rollback_path);
        mt_http_res_write_error(res, 500, "failed to stage import"); return;
    }
    err = mt_app_reload_config(ctx->app, new_path);
    unlink(new_path);
    if (err == MT_OK) {
        err = mt_app_save_config(ctx->app, ctx->config_path,
            transfer_version(ctx));
        if (err != MT_OK) {
            mt_err_t restored = mt_app_reload_config(ctx->app, rollback_path);
            if (restored != MT_OK) {
                MT_ERROR("config import rollback failed: %s", mt_err_str(restored));
            }
        }
    }
    unlink(rollback_path);
    if (err != MT_OK) {
        mt_http_res_write_error(res, 500, "import failed; previous configuration retained or rollback attempted");
        return;
    }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    mt_http_res_write_json(res, 200, out);
}

static void handle_netfilterd_hook(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_system_ctx_t *ctx = ud;
    size_t body_len;
    const uint8_t *body = mt_http_req_body(req, &body_len);
    cJSON *json = body_len > 0 ? cJSON_ParseWithLength((const char *)body, body_len) : NULL;
    if (!json) {
        mt_http_res_write_error(res, 400, "failed to parse request");
        return;
    }
    cJSON *type_j = cJSON_GetObjectItemCaseSensitive(json, "type");
    cJSON *table_j = cJSON_GetObjectItemCaseSensitive(json, "table");
    MT_DEBUG("received netfilter.d event: type=%s table=%s",
            cJSON_IsString(type_j) ? type_j->valuestring : "",
            cJSON_IsString(table_j) ? table_j->valuestring : "");
    cJSON_Delete(json);

    /* Never turned into an HTTP error response: the firmware fires this
     * hook while it is still rewriting tables and has nothing to do with
     * a failure. Where a committer runs (Keenetic `_kn`), the call only
     * queues a rebuild and cannot fail at all -- see
     * mt_app_force_commit_iptables. */
    mt_err_t err = mt_app_force_commit_iptables(ctx->app);
    if (err != MT_OK) { MT_ERROR("error fixing iptables after netfilter.d: %s", mt_err_str(err)); }
    mt_http_res_write(res, 200, NULL, NULL, 0);
}

void mt_system_register_routes(mt_httpd_t *h, mt_system_ctx_t *ctx) {
    mt_profiles_register_routes(h, ctx);
    mt_update_register_routes(h, ctx);
    must_route(h, "GET", "/api/v1/system/interfaces", handle_list_interfaces, ctx);
    must_route(h, "POST", "/api/v1/system/config/save", handle_save_config, ctx);
    must_route(h, "GET", "/api/v1/system/config/snapshot", handle_config_snapshot, ctx);
    must_route(h, "POST", "/api/v1/system/config/apply", handle_config_apply, ctx);
    must_route(h, "POST", "/api/v1/system/hooks/netfilterd", handle_netfilterd_hook, ctx);
}
