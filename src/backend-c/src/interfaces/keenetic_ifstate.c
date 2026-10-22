#include "magitrickle/keenetic_ifstate.h"
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include "magitrickle/json.h"

typedef struct {
    mt_kn_ifinfo_t info;
    uint64_t field_revision[MT_KN_IF_FIELDS], identity_revision, lifetime_revision, baseline_revision;
    bool removed, snapshot, layer_baseline, layer_events, legacy_events;
} entry_t;
struct mt_kn_ifstate {
    entry_t *items;
    size_t count, capacity;
    uint64_t revision;
    mt_kn_ifchanged_fn changed;
    void *ud;
};

static bool token_string(const char *s, size_t max, bool slash)
{
    if (!s || !*s || strlen(s) >= max) { return false; }
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
            (*c >= '0' && *c <= '9') || *c == '_' || *c == '-' ||
            *c == '.' || *c == ':' || (slash && *c == '/')) { continue; }
        return false;
    }
    return true;
}
static bool unique_keys(const cJSON *object)
{
    if (!object) { return true; }
    if (!cJSON_IsObject(object) || cJSON_GetArraySize(object) > (int)MT_KN_IFSTATE_LIMIT) { return false; }
    const cJSON *a, *b;
    cJSON_ArrayForEach(a, object) {
        for (b = a->next; b; b = b->next) {
            if (!strcmp(a->string, b->string)) { return false; }
        }
    }
    return true;
}
static cJSON *parse_document(const char *body, size_t len)
{
    cJSON *json = mt_json_parse_document(body, len);
    if (!json || !cJSON_IsObject(json) ||
        cJSON_GetArraySize(json) > (int)MT_KN_IFSTATE_LIMIT) { cJSON_Delete(json); return NULL; }
    if (!unique_keys(json)) { cJSON_Delete(json); return NULL; }
    return json;
}
static mt_kn_iflevel_t level(const cJSON *value, bool modern, bool connected)
{
    if (!value) { return MT_KN_IF_UNSET; }
    if (!cJSON_IsString(value)) { return MT_KN_IF_UNKNOWN; }
    const char *s = value->valuestring;
    if (!strcmp(s, modern ? "running" : connected ? "yes" : "up")) { return MT_KN_IF_RUNNING; }
    if (modern) {
        if (!strcmp(s, "disabled") || !strcmp(s, "pending") || !strcmp(s, "detached") ||
            !strcmp(s, "connecting")) { return MT_KN_IF_BLOCKED; }
    } else if (!strcmp(s, connected ? "no" : "down")) { return MT_KN_IF_BLOCKED; }
    return MT_KN_IF_UNKNOWN;
}
static bool copy_string(const cJSON *obj, const char *key, char *dst, size_t size,
                         bool required, bool slash)
{
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!s) { return !required; }
    if (!cJSON_IsString(s) || !token_string(s->valuestring, size, slash)) { return false; }
    memcpy(dst, s->valuestring, strlen(s->valuestring) + 1);
    return true;
}
static const char *const layer_names[] = {"conf", "link", "ipv4", "ipv6", "ctrl"};

mt_err_t mt_kn_ifevent_parse(const char *body, size_t len, mt_kn_ifevent_t *out)
{
    if (!out) { return MT_ERR_INVAL; }
    memset(out, 0, sizeof(*out));
    if (len > 1024) { return MT_ERR_LIMIT; }
    cJSON *root = parse_document(body, len);
    if (!root) { return MT_ERR_PROTO; }
    mt_err_t err = MT_ERR_INVAL;
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(root, "kind");
    if (!cJSON_IsString(kind) ||
        !copy_string(root, "id", out->info.id, sizeof(out->info.id), true, true) ||
        !copy_string(root, "system_name", out->info.name, sizeof(out->info.name), true, false)) { goto done; }
    const cJSON *sync = cJSON_GetObjectItemCaseSensitive(root, "resync");
    if (sync && !cJSON_IsBool(sync)) { goto done; }
    out->resync = cJSON_IsTrue(sync);
    if (!strcmp(kind->valuestring, "layer")) {
        out->kind = MT_KN_IF_LAYER;
        out->info.layered = true;
        const cJSON *field = cJSON_GetObjectItemCaseSensitive(root, "layer");
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "level");
        if (!cJSON_IsString(field) || !cJSON_IsString(value)) { goto done; }
        size_t i = 0;
        for (; i < 5; i++) { if (!strcmp(field->valuestring, layer_names[i])) { break; } }
        if (i == 5) { out->resync = true; }
        else {
            out->info.fields[i] = level(value, true, false);
            if (out->info.fields[i] == MT_KN_IF_UNKNOWN) { out->resync = true; }
        }
    } else if (!strcmp(kind->valuestring, "state")) {
        out->kind = MT_KN_IF_LEGACY;
        const char *const keys[] = {"up", "link", "connected"};
        for (size_t i = 0; i < 3; i++) {
            const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, keys[i]);
            if (!cJSON_IsString(value)) { goto done; }
            out->info.fields[MT_KN_IF_UP + i] = level(value, false, i == 2);
            if (out->info.fields[MT_KN_IF_UP + i] == MT_KN_IF_UNKNOWN) { out->resync = true; }
        }
    } else if (!strcmp(kind->valuestring, "created")) { out->kind = MT_KN_IF_CREATED; }
    else if (!strcmp(kind->valuestring, "destroyed")) { out->kind = MT_KN_IF_DESTROYED; }
    else { goto done; }
    err = MT_OK;
done:
    cJSON_Delete(root);
    return err;
}

static void copy_label(char *out, size_t size, const cJSON *value)
{
    out[0] = '\0';
    if (!cJSON_IsString(value)) { return; }
    const char *text = value->valuestring;
    while (*text && strchr(" \t\r\n\v\f", *text)) { text++; }
    size_t n = strlen(text);
    while (n && strchr(" \t\r\n\v\f", text[n - 1])) { n--; }
    if (n >= size) {
        n = size - 1;
        while (n && ((unsigned char)text[n] & 0xc0u) == 0x80u) { n--; }
    }
    memcpy(out, text, n); out[n] = '\0';
}

mt_err_t mt_kn_ifinfo_parse(const char *body, size_t len, mt_kn_ifinfo_t **out, size_t *count)
{
    if (!out || !count) { return MT_ERR_INVAL; }
    *out = NULL; *count = 0;
    if (len > MT_KN_IFSTATE_BODY_LIMIT) { return MT_ERR_LIMIT; }
    cJSON *root = parse_document(body, len);
    if (!root) { return MT_ERR_PROTO; }
    if (cJSON_GetObjectItemCaseSensitive(root, "status")) { cJSON_Delete(root); return MT_ERR_UPSTREAM; }
    size_t n = (size_t)cJSON_GetArraySize(root);
    if (n > MT_KN_IFSTATE_LIMIT) { cJSON_Delete(root); return MT_ERR_LIMIT; }
    if (!n) { cJSON_Delete(root); return MT_OK; }
    mt_kn_ifinfo_t *items = calloc(n, sizeof(*items));
    if (!items) { cJSON_Delete(root); return MT_ERR_NOMEM; }
    mt_err_t err = MT_OK;
    size_t i = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, root) {
        if (!unique_keys(item) || !token_string(item->string, sizeof(items[i].id), true)) {
            err = MT_ERR_PROTO; break;
        }
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
        const char *canonical = id ? (cJSON_IsString(id) ? id->valuestring : NULL) : item->string;
        if (!canonical || !token_string(canonical, sizeof(items[i].id), true)) { err = MT_ERR_PROTO; break; }
        for (size_t prev = 0; prev < i; prev++) {
            if (!strcmp(items[prev].id, canonical)) { err = MT_ERR_PROTO; break; }
        }
        if (err != MT_OK) { break; }
        strcpy(items[i].id, canonical);
        copy_label(items[i].alias, sizeof(items[i].alias), cJSON_GetObjectItemCaseSensitive(item, "description"));
        if (!items[i].alias[0]) {
            copy_label(items[i].alias, sizeof(items[i].alias), cJSON_GetObjectItemCaseSensitive(item, "interface-name"));
        }
        if (!copy_string(item, "system-name", items[i].name, sizeof(items[i].name), false, false)) {
            err = MT_ERR_PROTO; break;
        }
        const cJSON *summary = cJSON_GetObjectItemCaseSensitive(item, "summary");
        const cJSON *layers = cJSON_GetObjectItemCaseSensitive(summary, "layer");
        if (!unique_keys(summary) || !unique_keys(layers)) { err = MT_ERR_PROTO; break; }
        if (layers) {
            for (size_t f = 0; f < 5; f++) {
                items[i].fields[f] = level(cJSON_GetObjectItemCaseSensitive(layers, layer_names[f]), true, false);
                if (items[i].fields[f] != MT_KN_IF_UNSET) { items[i].layered = true; }
            }
        }
        items[i].fields[MT_KN_IF_UP] = level(cJSON_GetObjectItemCaseSensitive(item, "state"), false, false);
        items[i].fields[MT_KN_IF_LEGACY_LINK] = level(cJSON_GetObjectItemCaseSensitive(item, "link"), false, false);
        items[i].fields[MT_KN_IF_CONNECTED] = level(cJSON_GetObjectItemCaseSensitive(item, "connected"), false, true);
        i++;
    }
    cJSON_Delete(root);
    if (err != MT_OK) { free(items); return err; }
    *out = items; *count = n;
    return MT_OK;
}

mt_kn_ifstate_t *mt_kn_ifstate_new(mt_kn_ifchanged_fn changed, void *ud)
{
    mt_kn_ifstate_t *state = calloc(1, sizeof(*state));
    if (state) { state->changed = changed; state->ud = ud; }
    return state;
}
void mt_kn_ifstate_free(mt_kn_ifstate_t *state)
{
    if (state) { free(state->items); free(state); }
}
uint64_t mt_kn_ifstate_revision(const mt_kn_ifstate_t *state) { return state ? state->revision : 0; }
static bool blocked(const entry_t *entry, int family)
{
    if (entry->removed) { return true; }
    if (entry->info.layered && (entry->layer_events || !entry->legacy_events)) {
        const unsigned fields[] = {MT_KN_IF_CONF, MT_KN_IF_LINK, MT_KN_IF_CTRL,
                                   family == AF_INET ? MT_KN_IF_IPV4 : MT_KN_IF_IPV6};
        bool complete = true;
        for (size_t i = 0; i < 4; i++) {
            if (entry->info.fields[fields[i]] == MT_KN_IF_BLOCKED) { return true; }
            if (entry->info.fields[fields[i]] != MT_KN_IF_RUNNING) { complete = false; }
        }
        /* Switching from a legacy snapshot to a first layer event must not
         * prematurely clear its known negative status. A complete modern
         * baseline (or all relevant running events) releases this fallback. */
        if (!entry->layer_baseline && !complete) {
            for (size_t f = MT_KN_IF_UP; f < MT_KN_IF_FIELDS; f++) {
                if (entry->info.fields[f] == MT_KN_IF_BLOCKED) { return true; }
            }
        }
    } else {
        for (size_t i = MT_KN_IF_UP; i < MT_KN_IF_FIELDS; i++) {
            if (entry->info.fields[i] == MT_KN_IF_BLOCKED) { return true; }
        }
    }
    return false;
}
static const entry_t *select_interface(const mt_kn_ifstate_t *state, const char *name)
{
    if (!state || !name) { return NULL; }
    /* Several NDM records (a switch and its ports, Wi-Fi master and AP)
     * may legitimately share one kernel name. Prefer the parent NDM record
     * rather than whichever child emitted the last notification. */
    const entry_t *selected = NULL;
    for (size_t i = 0; i < state->count; i++) {
        const entry_t *candidate = &state->items[i];
        if (strcmp(candidate->info.name, name)) { continue; }
        if (!selected) { selected = candidate; continue; }
        const char *a = candidate->info.id, *b = selected->info.id;
        size_t da = 0, db = 0;
        for (const char *p = a; *p; p++) { da += *p == '/'; }
        for (const char *p = b; *p; p++) { db += *p == '/'; }
        if (da < db || (da == db && strcmp(a, b) < 0)) { selected = candidate; }
    }
    return selected;
}
bool mt_kn_ifstate_blocks(const mt_kn_ifstate_t *state, const char *name, int family)
{
    if (family != AF_INET && family != AF_INET6) { return false; }
    const entry_t *entry = select_interface(state, name);
    return entry ? blocked(entry, family) : false;
}
const char *mt_kn_ifstate_alias(const mt_kn_ifstate_t *state, const char *name)
{
    const entry_t *entry = select_interface(state, name);
    return entry && !entry->removed && entry->info.alias[0] && strcmp(entry->info.alias, name)
        ? entry->info.alias : NULL;
}
static void changed(mt_kn_ifstate_t *state, const char *name)
{
    if (*name && state->changed) { state->changed(state->ud, name); }
}
static void notify_difference(mt_kn_ifstate_t *state, const entry_t *before, const entry_t *after)
{
    if (strcmp(before->info.name, after->info.name)) {
        changed(state, before->info.name); changed(state, after->info.name);
    } else if (blocked(before, AF_INET) != blocked(after, AF_INET) ||
               blocked(before, AF_INET6) != blocked(after, AF_INET6)) {
        changed(state, after->info.name);
    }
}
static entry_t *find(mt_kn_ifstate_t *state, const char *id)
{
    for (size_t i = 0; i < state->count; i++) {
        if (!strcmp(state->items[i].info.id, id)) { return &state->items[i]; }
    }
    return NULL;
}
static entry_t *get(mt_kn_ifstate_t *state, const char *id)
{
    entry_t *entry = find(state, id);
    if (entry) { return entry; }
    if (state->count == MT_KN_IFSTATE_LIMIT) { return NULL; }
    if (state->count == state->capacity) {
        size_t cap = state->capacity ? state->capacity * 2 : 8;
        entry_t *p = realloc(state->items, cap * sizeof(*p));
        if (!p) { return NULL; }
        state->items = p; state->capacity = cap;
    }
    entry = &state->items[state->count++];
    memset(entry, 0, sizeof(*entry));
    strcpy(entry->info.id, id);
    return entry;
}
static bool same_ndm_tree(const char *a, const char *b)
{
    size_t root_len = strcspn(a, "/");
    return root_len == strcspn(b, "/") && !strncmp(a, b, root_len);
}
/* Related NDM records (a switch and its ports, a Wi-Fi master and AP) may
 * share a kernel name. An unrelated replacement must still retire the old
 * association, and a snapshot older than that reassignment must not steal it. */
static void map_name(mt_kn_ifstate_t *state, entry_t *entry, const char *name, uint64_t revision)
{
    if (!*name) { return; }
    for (size_t i = 0; i < state->count; i++) {
        const entry_t *other = &state->items[i];
        if (other == entry || strcmp(other->info.name, name) ||
            same_ndm_tree(other->info.id, entry->info.id)) { continue; }
        if (other->identity_revision > revision) { return; }
    }
    for (size_t i = 0; i < state->count; i++) {
        entry_t *other = &state->items[i];
        if (other == entry || strcmp(other->info.name, name) ||
            same_ndm_tree(other->info.id, entry->info.id)) { continue; }
        changed(state, name);
        other->info.name[0] = '\0';
        other->identity_revision = revision;
    }
    strcpy(entry->info.name, name); entry->identity_revision = revision;
}
mt_err_t mt_kn_ifstate_event(mt_kn_ifstate_t *state, const mt_kn_ifevent_t *event, bool *resync)
{
    if (!state || !event || !resync) { return MT_ERR_INVAL; }
    *resync = false;
    /* Reject incomplete identities before allocating a cache entry. Hooks
     * provide both names; only snapshots may omit the kernel mapping. */
    if (!event->info.id[0] || !event->info.name[0]) { return MT_ERR_INVAL; }
    *resync = event->resync;
    entry_t *entry = get(state, event->info.id);
    if (!entry) { return state->count == MT_KN_IFSTATE_LIMIT ? MT_ERR_LIMIT : MT_ERR_NOMEM; }
    entry_t before = *entry;
    uint64_t revision = ++state->revision;
    map_name(state, entry, event->info.name, revision);
    if (event->kind == MT_KN_IF_CREATED || event->kind == MT_KN_IF_DESTROYED) {
        if (event->kind == MT_KN_IF_CREATED) {
            entry->info.alias[0] = '\0';
            memset(entry->info.fields, 0, sizeof(entry->info.fields));
            entry->info.layered = false; entry->snapshot = false; entry->layer_baseline = false;
            entry->baseline_revision = revision;
            entry->layer_events = entry->legacy_events = false;
            *resync = true;
        }
        entry->removed = event->kind == MT_KN_IF_DESTROYED;
        entry->lifetime_revision = revision;
    } else {
        /* On modern firmware the companion legacy callback is stale for a
         * portion of the layer transition. It must not override modern data. */
        if (event->kind == MT_KN_IF_LAYER) {
            if (!entry->layer_events && (entry->legacy_events || !entry->info.layered)) {
                /* Discard stale layer values on the switch from legacy data.
                 * Freeze the legacy fallback at this generation as well:
                 * an older in-flight snapshot must not replace connected=no
                 * with yes while the modern baseline is still incomplete. */
                for (size_t f = 0; f < MT_KN_IF_FIELDS; f++) {
                    if (f < MT_KN_IF_UP) { entry->info.fields[f] = MT_KN_IF_UNSET; }
                    entry->field_revision[f] = revision;
                }
                entry->layer_baseline = false;
                entry->baseline_revision = revision;
                *resync = true;
            }
            if (!entry->info.layered) { *resync = true; }
            entry->info.layered = entry->layer_events = true;
        } else { entry->legacy_events = true; }
        for (size_t i = 0; i < MT_KN_IF_FIELDS; i++) {
            /* Companion legacy hooks cannot release the frozen fallback;
             * only a fresh snapshot or complete modern recovery can do so. */
            if (event->kind == MT_KN_IF_LEGACY && entry->layer_events) { continue; }
            if (event->info.fields[i] == MT_KN_IF_UNSET) { continue; }
            entry->field_revision[i] = revision;
            if (event->info.fields[i] != MT_KN_IF_UNKNOWN) { entry->info.fields[i] = event->info.fields[i]; }
        }
        if (!entry->snapshot || entry->removed) { *resync = true; }
    }
    notify_difference(state, &before, entry);
    return MT_OK;
}
mt_err_t mt_kn_ifstate_snapshot(mt_kn_ifstate_t *state, const mt_kn_ifinfo_t *items,
                               size_t count, uint64_t token)
{
    if (!state || (count && !items) || count > MT_KN_IFSTATE_LIMIT || token > state->revision) { return MT_ERR_INVAL; }
    /* Remove absent old objects only after an authoritative snapshot. Events
     * (including deletion tombstones) newer than the read always win. */
    for (size_t i = 0; i < state->count;) {
        entry_t *entry = &state->items[i];
        bool found = false;
        for (size_t j = 0; j < count; j++) { if (!strcmp(entry->info.id, items[j].id)) { found = true; break; } }
        uint64_t latest = entry->identity_revision;
        if (entry->lifetime_revision > latest) { latest = entry->lifetime_revision; }
        for (size_t f = 0; f < MT_KN_IF_FIELDS; f++) { if (entry->field_revision[f] > latest) { latest = entry->field_revision[f]; } }
        if (!found && latest <= token) {
            changed(state, entry->info.name);
            state->items[i] = state->items[--state->count];
        } else { i++; }
    }
    for (size_t i = 0; i < count; i++) {
        entry_t *entry = get(state, items[i].id);
        if (!entry) { return state->count == MT_KN_IFSTATE_LIMIT ? MT_ERR_LIMIT : MT_ERR_NOMEM; }
        if (entry->lifetime_revision > token) { continue; }
        entry_t before = *entry;
        if (entry->identity_revision <= token) {
            map_name(state, entry, items[i].name, token);
            memcpy(entry->info.alias, items[i].alias, sizeof(entry->info.alias));
        }
        entry->removed = false;
        entry->snapshot = true;
        if (items[i].layered && entry->baseline_revision <= token) {
            entry->info.layered = true; entry->layer_baseline = true;
        }
        for (size_t f = 0; f < MT_KN_IF_FIELDS; f++) {
            if (entry->field_revision[f] > token || items[i].fields[f] == MT_KN_IF_UNKNOWN) { continue; }
            /* Missing data is not evidence that a previously down layer is up. */
            if (items[i].fields[f] != MT_KN_IF_UNSET) { entry->info.fields[f] = items[i].fields[f]; }
        }
        notify_difference(state, &before, entry);
    }
    return MT_OK;
}
