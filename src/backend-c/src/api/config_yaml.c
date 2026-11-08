/* Authenticated, revision-checked access to the configured YAML file. The
 * existing HTTP middleware protects these /api routes; the path is never
 * supplied by a browser. Raw bytes and live models both participate in the
 * revision, including accepted-but-not-persisted changes from other editors. */
#include "magitrickle/system.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "magitrickle/hash.h"
#include "magitrickle/json.h"
#include "magitrickle/log.h"

#define YAML_EDITOR_MAX_BYTES ((size_t)16 * 1024 * 1024)
#define YAML_EDITOR_MAX_REQUEST_BYTES ((size_t)24 * 1024 * 1024)

typedef struct yaml_snapshot {
    char *yaml;
    size_t length;
    bool exists;
    bool out_of_sync;
    cJSON *live;
    char revision[2 * MT_SHA256_DIGEST_LEN + 1];
} yaml_snapshot_t;

static void snapshot_clear(yaml_snapshot_t *snapshot)
{
    free(snapshot->yaml);
    cJSON_Delete(snapshot->live);
    memset(snapshot, 0, sizeof(*snapshot));
}

static mt_err_t read_yaml_file(const char *path, char **out, size_t *length, bool *exists)
{
    *out = NULL; *length = 0; *exists = false;
    /* O_NONBLOCK prevents a replaced FIFO/device from blocking the daemon. */
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) { return errno == ENOENT ? MT_OK : mt_err_from_errno(errno); }
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return MT_ERR_IO; }
    if (!S_ISREG(st.st_mode) || st.st_size < 0) { close(fd); return MT_ERR_INVAL; }
    if ((uintmax_t)st.st_size > YAML_EDITOR_MAX_BYTES) { close(fd); return MT_ERR_LIMIT; }
    size_t size = (size_t)st.st_size;
    char *data = malloc(size + 1);
    if (!data) { close(fd); return MT_ERR_NOMEM; }
    size_t offset = 0;
    mt_err_t err = MT_OK;
    while (offset < size) {
        ssize_t got = read(fd, data + offset, size - offset);
        if (got < 0 && errno == EINTR) { continue; }
        if (got <= 0) { err = MT_ERR_IO; break; }
        offset += (size_t)got;
    }
    /* The snapshot must cover the entire file, including concurrent growth. */
    char extra;
    ssize_t tail;
    do { tail = read(fd, &extra, 1); } while (tail < 0 && errno == EINTR);
    if (tail != 0) { err = MT_ERR_IO; }
    close(fd);
    if (err == MT_OK && memchr(data, 0, size)) { err = MT_ERR_INVAL; }
    if (err != MT_OK) { free(data); return err; }
    data[size] = '\0';
    *out = data; *length = size; *exists = true;
    return MT_OK;
}

static mt_err_t editor_revision(const char *yaml, size_t length, bool exists,
                                const cJSON *live, char out[65])
{
    char *json = cJSON_PrintUnformatted(live);
    if (!json) { return MT_ERR_NOMEM; }
    uint8_t file_hash[MT_SHA256_DIGEST_LEN], state_hash[MT_SHA256_DIGEST_LEN];
    mt_sha256((const uint8_t *)yaml, length, file_hash);
    mt_sha256((const uint8_t *)json, strlen(json), state_hash);
    free(json);
    mt_sha256_ctx_t hash;
    mt_sha256_init(&hash);
    const uint8_t present = exists ? 1 : 0;
    mt_sha256_update(&hash, &present, 1);
    mt_sha256_update(&hash, file_hash, sizeof(file_hash));
    mt_sha256_update(&hash, state_hash, sizeof(state_hash));
    mt_sha256_final(&hash, file_hash);
    for (size_t i = 0; i < sizeof(file_hash); i++) { snprintf(out + 2 * i, 3, "%02x", file_hash[i]); }
    return MT_OK;
}

static mt_err_t take_yaml_snapshot(mt_system_ctx_t *ctx, yaml_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    mt_err_t err = read_yaml_file(ctx->config_path, &snapshot->yaml,
                                &snapshot->length, &snapshot->exists);
    if (err == MT_OK) { err = mt_app_config_snapshot_json(ctx->app, "0.0.0", &snapshot->live); }
    if (err == MT_OK && !snapshot->exists) {
        const char *version = ctx->config_version;
        if (!version || strncmp(version, "0.", 2)) { version = "0.0.0"; }
        err = mt_app_config_snapshot_yaml(ctx->app, version, &snapshot->yaml, &snapshot->length);
        if (err == MT_OK && snapshot->length > YAML_EDITOR_MAX_BYTES) { err = MT_ERR_LIMIT; }
    }
    if (err == MT_OK && snapshot->exists) {
        mt_config_t disk = {0};
        cJSON *disk_json = NULL;
        mt_err_t parsed = mt_config_init_defaults(&disk);
        if (parsed == MT_OK) {
            /* Compare the same interpretation used at daemon startup. Strict
             * interactive validation belongs to PUT, not every read (which
             * must not recompile every cached subscription regex). */
            parsed = mt_config_load_buffer(&disk, snapshot->yaml, snapshot->length);
        }
        if (parsed == MT_OK) { parsed = mt_config_save_json(&disk, "0.0.0", &disk_json); }
        /* A broken existing file stays editable. An explicit acknowledgement
         * is required before it can replace a different live configuration. */
        snapshot->out_of_sync = parsed != MT_OK || !cJSON_Compare(disk_json, snapshot->live, true);
        if (parsed == MT_ERR_NOMEM) { err = parsed; }
        cJSON_Delete(disk_json);
        mt_config_clear(&disk);
    }
    if (err == MT_OK) {
        err = editor_revision(snapshot->yaml, snapshot->length, snapshot->exists,
                              snapshot->live, snapshot->revision);
    }
    if (err != MT_OK) { snapshot_clear(snapshot); }
    return err;
}

static cJSON *yaml_response(const char *yaml, const char *revision,
                            bool restart_required, bool out_of_sync)
{
    cJSON *out = cJSON_CreateObject();
    if (!out || !cJSON_AddStringToObject(out, "yaml", yaml) ||
        !cJSON_AddStringToObject(out, "revision", revision) ||
        !cJSON_AddBoolToObject(out, "restart_required", restart_required) ||
        !cJSON_AddBoolToObject(out, "out_of_sync", out_of_sync)) {
        cJSON_Delete(out); return NULL;
    }
    return out;
}

static void yaml_error(mt_http_res_t *res, int status, const char *code,
                       const char *message, const mt_config_diagnostic_t *diagnostic)
{
    cJSON *out = mt_json_error(message);
    bool ok = out && cJSON_AddStringToObject(out, "code", code);
    if (ok && diagnostic && diagnostic->line) {
        ok = cJSON_AddNumberToObject(out, "line", (double)diagnostic->line) &&
             cJSON_AddNumberToObject(out, "column", (double)diagnostic->column);
    }
    if (!ok) { cJSON_Delete(out); mt_http_res_write_error(res, 500, "out of memory"); return; }
    mt_http_res_write_json(res, status, out);
}

static void snapshot_error(mt_http_res_t *res, mt_err_t err)
{
    yaml_error(res, err == MT_ERR_LIMIT ? 413 : 500, "CONFIG_READ_FAILED",
               err == MT_ERR_LIMIT ? "YAML exceeds the 16 MiB editor limit" :
               "Could not read the configuration file", NULL);
}

static void handle_yaml_get(mt_http_req_t *req, mt_http_res_t *res, void *userdata)
{
    (void)req;
    mt_system_ctx_t *ctx = userdata;
    mt_http_res_set_header(res, "Cache-Control", "no-store");
    if (!ctx->config_path) {
        yaml_error(res, 503, "CONFIG_UNAVAILABLE", "Configuration persistence unavailable", NULL); return;
    }
    yaml_snapshot_t snapshot;
    mt_err_t err = take_yaml_snapshot(ctx, &snapshot);
    if (err != MT_OK) { snapshot_error(res, err); return; }
    cJSON *out = yaml_response(snapshot.yaml, snapshot.revision,
                               mt_app_config_restart_required(ctx->app, NULL), snapshot.out_of_sync);
    snapshot_clear(&snapshot);
    if (!out) { mt_http_res_write_error(res, 500, "out of memory"); return; }
    mt_http_res_write_json(res, 200, out);
}

typedef struct yaml_commit {
    const char *path;
    const yaml_snapshot_t *previous;
    const char *yaml;
    size_t length;
} yaml_commit_t;

static mt_err_t persist_yaml(const mt_config_t *cfg, void *userdata)
{
    (void)cfg;
    const yaml_commit_t *commit = userdata;
    char *current = NULL;
    size_t length = 0;
    bool exists = false;
    mt_err_t err = read_yaml_file(commit->path, &current, &length, &exists);
    if (err == MT_OK && (exists != commit->previous->exists ||
        (exists && (length != commit->previous->length ||
                    memcmp(current, commit->previous->yaml, length))))) { err = MT_ERR_STATE; }
    free(current);
    if (err != MT_OK) { return err; }
    return mt_config_write_buffer(commit->path, commit->yaml, commit->length);
}

static bool valid_request(const cJSON *request)
{
    if (!cJSON_IsObject(request)) { return false; }
    bool have_yaml = false, have_revision = false, have_override = false;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, request) {
        if (strcmp(item->string, "yaml") == 0 && !have_yaml && cJSON_IsString(item)) { have_yaml = true; }
        else if (strcmp(item->string, "revision") == 0 && !have_revision && cJSON_IsString(item)) {
            have_revision = true;
        } else if (strcmp(item->string, "allow_out_of_sync") == 0 && !have_override && cJSON_IsBool(item)) {
            have_override = true;
        } else { return false; }
    }
    return have_yaml && have_revision;
}

static void handle_yaml_put(mt_http_req_t *req, mt_http_res_t *res, void *userdata)
{
    mt_system_ctx_t *ctx = userdata;
    mt_http_res_set_header(res, "Cache-Control", "no-store");
    if (!ctx->config_path) {
        yaml_error(res, 503, "CONFIG_UNAVAILABLE", "Configuration persistence unavailable", NULL); return;
    }
    const char *type = mt_http_req_header(req, "Content-Type");
    const char *site = mt_http_req_header(req, "Sec-Fetch-Site");
    if (!type || strncmp(type, "application/json", 16) ||
        (type[16] && type[16] != ';' && type[16] != ' ') ||
        (site && strcmp(site, "cross-site") == 0)) {
        yaml_error(res, 400, "INVALID_CONFIG", "Expected a same-origin JSON request", NULL); return;
    }
    size_t body_length = 0;
    const uint8_t *body = mt_http_req_body(req, &body_length);
    cJSON *request = body_length <= YAML_EDITOR_MAX_REQUEST_BYTES ?
        mt_json_parse_document((const char *)body, body_length) : NULL;
    if (!valid_request(request)) {
        cJSON_Delete(request);
        yaml_error(res, 400, "INVALID_CONFIG", "YAML and revision are required", NULL); return;
    }
    const char *yaml = cJSON_GetObjectItemCaseSensitive(request, "yaml")->valuestring;
    const char *base = cJSON_GetObjectItemCaseSensitive(request, "revision")->valuestring;
    bool override = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "allow_out_of_sync"));
    size_t length = strlen(yaml);
    if (length > YAML_EDITOR_MAX_BYTES) {
        cJSON_Delete(request);
        yaml_error(res, 413, "INVALID_CONFIG", "YAML exceeds the 16 MiB editor limit", NULL); return;
    }
    yaml_snapshot_t snapshot;
    mt_err_t err = take_yaml_snapshot(ctx, &snapshot);
    if (err != MT_OK) { cJSON_Delete(request); snapshot_error(res, err); return; }
    if (strcmp(base, snapshot.revision) != 0) {
        yaml_error(res, 409, "CONFIG_CONFLICT", "Configuration changed; reload before saving", NULL);
        goto done;
    }
    if (snapshot.out_of_sync && !override) {
        yaml_error(res, 409, "CONFIG_OUT_OF_SYNC",
                   "The file and running configuration differ; confirm replacement or save pending settings first", NULL);
        goto done;
    }
    mt_config_t candidate = {0};
    mt_config_diagnostic_t diagnostic = {0};
    err = mt_config_init_defaults(&candidate);
    if (err == MT_OK) { err = mt_config_load_editor_buffer(&candidate, yaml, length, &diagnostic); }
    if (err != MT_OK) {
        mt_config_clear(&candidate);
        yaml_error(res, err == MT_ERR_NOMEM ? 500 : 400, "INVALID_CONFIG",
                   diagnostic.message[0] ? diagnostic.message : "Could not validate configuration", &diagnostic);
        goto done;
    }
    /* Build the entire success response before committing: allocation failure
     * must not turn a successful file replacement into an ambiguous 500. */
    cJSON *next = NULL, *out = NULL;
    char revision[65];
    err = mt_config_save_json(&candidate, "0.0.0", &next);
    if (err == MT_OK) { err = editor_revision(yaml, length, true, next, revision); }
    if (err == MT_OK) {
        out = yaml_response(yaml, revision, mt_app_config_restart_required(ctx->app, &candidate.app), false);
        if (!out) { err = MT_ERR_NOMEM; }
    }
    bool same_models = next && cJSON_Compare(next, snapshot.live, true);
    cJSON_Delete(next);
    yaml_commit_t commit = {.path = ctx->config_path, .previous = &snapshot, .yaml = yaml, .length = length};
    if (err == MT_OK && same_models) {
        /* Comments and formatting do not rebuild routes or invalidate syncs. */
        err = snapshot.exists && length == snapshot.length && !memcmp(yaml, snapshot.yaml, length) ?
            MT_OK : persist_yaml(&candidate, &commit);
    } else if (err == MT_OK) {
        err = mt_app_apply_config_with_persistence(ctx->app, &candidate, persist_yaml, &commit);
    }
    mt_config_clear(&candidate);
    if (err != MT_OK) {
        cJSON_Delete(out);
        yaml_error(res, err == MT_ERR_STATE ? 409 : 500,
                   err == MT_ERR_STATE ? "CONFIG_CONFLICT" : "CONFIG_SAVE_FAILED",
                   err == MT_ERR_STATE ? "Configuration file changed; reload before saving" :
                   "Could not save configuration; previous models retained and routing rollback attempted", NULL);
    } else {
        mt_http_res_write_json(res, 200, out);
    }
done:
    snapshot_clear(&snapshot);
    cJSON_Delete(request);
}

void mt_system_register_yaml_routes(mt_httpd_t *server, mt_system_ctx_t *ctx)
{
    if (mt_httpd_route(server, "GET", "/api/v1/system/config/yaml", handle_yaml_get, ctx) != MT_OK ||
        mt_httpd_route(server, "PUT", "/api/v1/system/config/yaml", handle_yaml_put, ctx) != MT_OK) {
        MT_ERROR("failed to register YAML configuration editor routes");
    }
}
