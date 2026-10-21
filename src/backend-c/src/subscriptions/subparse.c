#define PCRE2_CODE_UNIT_WIDTH 8

#include "magitrickle/subparse.h"
#include "magitrickle/id_pool.h"

#include "magitrickle/lookup.h"
#include "magitrickle/log.h"
#include "magitrickle/rand.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <stdio.h>
#include <pcre2.h>
#include <stdlib.h>
#include <string.h>

/* ---- validators (validate.go) ---- */

static bool all_chars_in(const char *s, const char *extra)
{
    for (const char *p = s; *p != '\0'; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.') {
            continue;
        }
        if (extra != NULL && strchr(extra, c) != NULL) {
            continue;
        }
        return false;
    }
    return true;
}

static bool is_valid_domain(const char *p)
{
    size_t len = strlen(p);
    if (len == 0) {
        return false;
    }
    if (p[0] == '.' || p[len - 1] == '.') {
        return false;
    }
    if (strstr(p, "..") != NULL) {
        return false;
    }
    return all_chars_in(p, NULL);
}

static bool is_valid_wildcard(const char *p)
{
    size_t len = strlen(p);
    if (len == 0) {
        return false;
    }
    if (p[0] == '.' || p[len - 1] == '.') {
        return false;
    }
    if (strstr(p, "..") != NULL || strstr(p, "**") != NULL) {
        return false;
    }
    return all_chars_in(p, "*?");
}

static int parse_dec(const char *s, size_t len)
{
    if (len == 0) {
        return -1;
    }
    int n = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return -1;
        }
        if (n > (999999999 - (s[i] - '0')) / 10) {
            return -1;
        }
        n = n * 10 + (s[i] - '0');
    }
    return n;
}

/* subnetRe: ^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?:\/(\d{1,2}))?$
 * with octets 0..255 and prefix 0..32. */
static bool is_valid_subnet(const char *p)
{
    const char *s = p;
    for (int octet = 0; octet < 4; octet++) {
        const char *start = s;
        while (*s >= '0' && *s <= '9') {
            s++;
        }
        size_t dlen = (size_t)(s - start);
        if (dlen < 1 || dlen > 3) {
            return false;
        }
        int v = parse_dec(start, dlen);
        if (v < 0 || v > 255) {
            return false;
        }
        if (octet < 3) {
            if (*s != '.') {
                return false;
            }
            s++;
        }
    }
    if (*s == '/') {
        s++;
        const char *start = s;
        while (*s >= '0' && *s <= '9') {
            s++;
        }
        size_t dlen = (size_t)(s - start);
        if (dlen < 1 || dlen > 2) {
            return false;
        }
        int v = parse_dec(start, dlen);
        if (v < 0 || v > 32) {
            return false;
        }
    }
    return *s == '\0';
}

static bool is_valid_ipv6_chars(const char *ip)
{
    if (strchr(ip, ':') == NULL) {
        return false;
    }
    for (const char *p = ip; *p != '\0'; p++) {
        char c = *p;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F') || c == ':') {
            continue;
        }
        return false;
    }
    return true;
}

static bool is_valid_subnet6(const char *p)
{
    const char *slash = strchr(p, '/');
    if (slash == NULL) {
        return is_valid_ipv6_chars(p);
    }
    if (strchr(slash + 1, '/') != NULL) {
        return false;
    }
    int prefix = parse_dec(slash + 1, strlen(slash + 1));
    if (prefix < 0 || prefix > 128) {
        return false;
    }
    char head[256];
    size_t head_len = (size_t)(slash - p);
    if (head_len >= sizeof(head)) {
        return false;
    }
    memcpy(head, p, head_len);
    head[head_len] = '\0';
    return is_valid_ipv6_chars(head);
}

/* isValidRegex: Go uses regexp2.Compile(pattern, 0). We use PCRE2 —
 * the 3 known divergence classes are documented in decisions.md D-07. */
static bool is_valid_regex(const char *p)
{
    int errcode = 0;
    PCRE2_SIZE erroff = 0;
    pcre2_code *code =
        pcre2_compile((PCRE2_SPTR)p, PCRE2_ZERO_TERMINATED,
                      PCRE2_UTF | PCRE2_UCP, &errcode, &erroff, NULL);
    if (code == NULL) {
        return false;
    }
    pcre2_code_free(code);
    return true;
}

/* Keep a canonical typed source record as the subscription identity. This
 * preserves distinct DOMAIN/DOMAIN-KEYWORD records with the same payload,
 * existing pattern-keyed overrides, and type restoration after a restart.
 * Only the runtime projection strips the prefix (mt_sub_rule_pattern). */
typedef struct typed_rule {
    const char *name;
    const char *canonical;
    const char *type;
} typed_rule_t;

static const typed_rule_t typed_rules[] = {
    {"DOMAIN", "DOMAIN", MT_RULE_DOMAIN},
    {"DOMAIN-SUFFIX", "DOMAIN-SUFFIX", MT_RULE_NAMESPACE},
    {"DOMAIN-KEYWORD", "DOMAIN-KEYWORD", MT_RULE_KEYWORD},
    {"DOMAIN-WILDCARD", "DOMAIN-WILDCARD", MT_RULE_DOMAIN_WILDCARD},
    {"DOMAIN-REGEX", "DOMAIN-REGEX", MT_RULE_REGEX},
    {"IP-CIDR", "IP-CIDR", MT_RULE_SUBNET},
    {"IP-CIDR6", "IP-CIDR6", MT_RULE_SUBNET6},
    {"IP6-CIDR", "IP-CIDR6", MT_RULE_SUBNET6},
    {"HOST", "DOMAIN", MT_RULE_DOMAIN},
    {"HOST-SUFFIX", "DOMAIN-SUFFIX", MT_RULE_NAMESPACE},
    {"HOST-KEYWORD", "DOMAIN-KEYWORD", MT_RULE_KEYWORD},
};

static char ascii_lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
}

static bool ascii_equal(const char *s, size_t len, const char *literal)
{
    if (len != strlen(literal)) { return false; }
    for (size_t i = 0; i < len; i++) {
        if (ascii_lower(s[i]) != ascii_lower(literal[i])) { return false; }
    }
    return true;
}

static const typed_rule_t *typed_rule(const char *name, size_t len)
{
    for (size_t i = 0; i < sizeof(typed_rules) / sizeof(typed_rules[0]); i++) {
        if (ascii_equal(name, len, typed_rules[i].name)) { return &typed_rules[i]; }
    }
    return NULL;
}

const char *mt_sub_rule_pattern(const char *rule)
{
    if (!rule) { return NULL; }
    const char *comma = strchr(rule, ',');
    if (comma && typed_rule(rule, (size_t)(comma - rule))) { return comma + 1; }
    return rule;
}

const char *mt_sub_detect_type(const char *pattern)
{
    const char *comma = strchr(pattern, ',');
    const typed_rule_t *source = comma ? typed_rule(pattern, (size_t)(comma - pattern)) : NULL;
    if (source) { return comma[1] ? source->type : ""; }
    /* Go re-trims here (callers already trim, harmless to repeat). */
    if (is_valid_subnet6(pattern)) {
        return MT_RULE_SUBNET6;
    }
    if (is_valid_subnet(pattern)) {
        return MT_RULE_SUBNET;
    }
    if (is_valid_domain(pattern)) {
        /* namespace validator == domain validator, namespace checked
         * first in Go, so plain domains always become "namespace" */
        return MT_RULE_NAMESPACE;
    }
    if (is_valid_regex(pattern)) {
        return MT_RULE_REGEX;
    }
    if (is_valid_wildcard(pattern)) {
        return MT_RULE_WILDCARD;
    }
    return "";
}

/* ---- tokenizer + indexed dedup / ID allocation --------------------------- */

void mt_sub_rules_free(mt_sub_rule_t **rules, size_t n) {
    for (size_t i = 0; i < n; i++) { mt_sub_rule_free(rules[i]); }
    free(rules);
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) { s++; }
    size_t len = strlen(s);
    while (len && isspace((unsigned char)s[len - 1])) { s[--len] = '\0'; }
    return s;
}

static bool canceled(const atomic_bool *cancel) {
    return cancel && atomic_load(cancel);
}

/* Reserve explicit rule records, not comma-separated legacy host lists.
 * Unsupported records must never become e.g. a namespace named PROCESS-NAME.
 * A recognized typed record makes the entire source a strict rule-set. */
static bool looks_typed(const char *s, size_t len)
{
    if (typed_rule(s, len)) { return true; }
    static const char *const unsupported[] = {
        "GEOIP", "GEOSITE", "AND", "OR", "NOT", "MATCH", "FINAL",
        "NETWORK", "UID", "DSCP", "INCLUDE", "SUB-RULE",
        "PROCESS-NAME", "PROCESS-PATH", "PROCESS-NAME-REGEX", "PROCESS-PATH-REGEX",
        "IP-ASN", "SRC-IP-CIDR", "SRC-IP-SUFFIX", "SRC-PORT", "DST-PORT", "RULE-SET",
    };
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); i++) {
        if (ascii_equal(s, len, unsupported[i])) { return true; }
    }
    bool hyphen = false;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '-') { hyphen = true; }
        else if (!(s[i] >= 'A' && s[i] <= 'Z') && !(s[i] >= '0' && s[i] <= '9')) {
            return false;
        }
    }
    return hyphen;
}

static bool line_comment(const char *line)
{
    return *line == '#' || *line == ';' || (line[0] == '/' && line[1] == '/');
}

static bool is_typed_list(const char *list)
{
    const char *line = list;
    while (*line) {
        size_t len = strcspn(line, "\r\n");
        const char *end = line + len;
        while (line < end && isspace((unsigned char)*line)) { line++; }
        if (line < end && !line_comment(line)) {
            const char *comma = memchr(line, ',', (size_t)(end - line));
            if (comma) {
                const char *name_end = comma;
                while (name_end > line && isspace((unsigned char)name_end[-1])) { name_end--; }
                if (looks_typed(line, (size_t)(name_end - line))) { return true; }
            }
        }
        line = *end ? end + 1 : end;
    }
    return false;
}

static bool valid_literal_keyword(const char *value)
{
    if (!*value) { return false; }
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p < 0x21 || *p > 0x7e) { return false; }
    }
    return true;
}

static bool valid_typed_domain(const char *value)
{
    size_t len = strlen(value);
    return len && len <= 253 && value[0] != '.' && value[len - 1] != '.' &&
           !strstr(value, "..") && all_chars_in(value, "_");
}

static bool typed_ip_family(const char *value, bool *ipv6)
{
    const char *slash = strchr(value, '/');
    size_t len = slash ? (size_t)(slash - value) : strlen(value);
    char address[INET6_ADDRSTRLEN];
    if (!len || len >= sizeof(address)) { return false; }
    memcpy(address, value, len);
    address[len] = '\0';
    struct in6_addr storage;
    if (inet_pton(AF_INET, address, &storage) == 1) { *ipv6 = false; }
    else if (inet_pton(AF_INET6, address, &storage) == 1) { *ipv6 = true; }
    else { return false; }
    if (slash) {
        int prefix = parse_dec(slash + 1, strlen(slash + 1));
        if (prefix < 0 || prefix > (*ipv6 ? 128 : 32)) { return false; }
    }
    return true;
}

/* Input is one mutable physical line. Regex payloads are intentionally kept
 * whole, including commas (quantifiers, classes and literal commas): the
 * supported regex form is TYPE,expression, not an inline policy rule. */
static mt_err_t canonical_typed_record(char *line, char *out, size_t out_size,
                                       const char **type, const char **reason)
{
    *reason = "invalid typed rule";
    char *comma = strchr(line, ',');
    if (!comma) { *reason = "expected TYPE,value in rule-set"; return MT_ERR_INVAL; }
    *comma = '\0';
    const char *name = trim(line);
    const typed_rule_t *spec = typed_rule(name, strlen(name));
    if (!spec) { *reason = "unsupported rule type in rule-set"; return MT_ERR_INVAL; }
    char *value = trim(comma + 1);
    bool ip = strcmp(spec->type, MT_RULE_SUBNET) == 0 || strcmp(spec->type, MT_RULE_SUBNET6) == 0;
    bool regex = strcmp(spec->type, MT_RULE_REGEX) == 0;
    if (!regex) {
        /* Policy labels belong to the upstream client, not mt-c routing.
         * Accept at most one label and an optional IP-only no-resolve flag.
         * In particular, never import an IP rule carrying the `src` modifier. */
        char *extra = strchr(value, ',');
        if (extra) { *extra++ = '\0'; }
        value = trim(value);
        bool policy_seen = false, no_resolve_seen = false;
        while (extra) {
            char *next = strchr(extra, ',');
            if (next) { *next++ = '\0'; }
            char *field = trim(extra);
            size_t n = strlen(field);
            if (!n || ascii_equal(field, n, "src")) {
                *reason = "empty or unsupported rule modifier"; return MT_ERR_INVAL;
            }
            if (ascii_equal(field, n, "no-resolve")) {
                if (!ip || no_resolve_seen || next) {
                    *reason = "invalid no-resolve modifier"; return MT_ERR_INVAL;
                }
                no_resolve_seen = true;
            } else if (policy_seen || no_resolve_seen) {
                *reason = "unsupported rule modifiers"; return MT_ERR_INVAL;
            } else {
                policy_seen = true;
            }
            extra = next;
        }
    }
    if (!*value) { *reason = "empty rule value"; return MT_ERR_INVAL; }
    const char *canonical = spec->canonical;
    *type = spec->type;
    if (ip) {
        bool ipv6;
        if (!typed_ip_family(value, &ipv6)) { *reason = "invalid IP/CIDR"; return MT_ERR_INVAL; }
        canonical = ipv6 ? "IP-CIDR6" : "IP-CIDR";
        *type = ipv6 ? MT_RULE_SUBNET6 : MT_RULE_SUBNET;
    } else if (strcmp(*type, MT_RULE_DOMAIN) == 0 || strcmp(*type, MT_RULE_NAMESPACE) == 0) {
        size_t n = strlen(value);
        if (value[n - 1] == '.') { value[n - 1] = '\0'; }
        if (!valid_typed_domain(value)) { *reason = "invalid domain"; return MT_ERR_INVAL; }
    } else if (strcmp(*type, MT_RULE_KEYWORD) == 0) {
        if (!valid_literal_keyword(value)) { *reason = "invalid keyword"; return MT_ERR_INVAL; }
    } else if (strcmp(*type, MT_RULE_DOMAIN_WILDCARD) == 0) {
        if (!all_chars_in(value, "_*?")) {
            *reason = "wildcard supports only literal DNS characters, * and ?"; return MT_ERR_INVAL;
        }
    } else if (!is_valid_regex(value)) {
        *reason = "invalid DOMAIN-REGEX expression"; return MT_ERR_INVAL;
    }
    size_t name_len = strlen(canonical), value_len = strlen(value);
    if (name_len + 1 + value_len >= out_size) { return MT_ERR_LIMIT; }
    memcpy(out, canonical, name_len);
    out[name_len] = ',';
    memcpy(out + name_len + 1, value, value_len + 1);
    if (!regex) {
        for (size_t i = name_len + 1; i < name_len + 1 + value_len; i++) {
            out[i] = ascii_lower(out[i]);
        }
    }
    return MT_OK;
}

typedef struct parse_state {
    mt_sub_rule_t **rules;
    size_t n, cap;
    mt_lookup_t seen, used;
    mt_id_pool_t pool;
} parse_state_t;

static mt_err_t append_parsed_rule(parse_state_t *state, const char *text, const char *type)
{
    size_t len = strlen(text);
    if (len > MT_SUB_MAX_LINE_BYTES) { return MT_ERR_LIMIT; }
    if (mt_lookup_get(&state->seen, text, len, NULL)) { return MT_OK; }
    if (state->n == MT_SUB_MAX_RULES) { return MT_ERR_LIMIT; }
    mt_err_t err = mt_lookup_put(&state->seen, text, len, state->n, NULL);
    if (err != MT_OK) { return err; }
    mt_sub_rule_t *rule = mt_sub_rule_new();
    if (!rule) { return MT_ERR_NOMEM; }
    rule->enable = true;
    err = mt_id_pool_unique(&state->used, &state->pool, &rule->id);
    if (err == MT_OK) { err = mt_strset(&rule->rule, text); }
    if (err == MT_OK) { err = mt_strset(&rule->type, type ? type : mt_sub_detect_type(text)); }
    if (err != MT_OK) { mt_sub_rule_free(rule); return err; }
    if (state->n == state->cap) {
        size_t next = state->cap ? state->cap * 2 : 16;
        mt_sub_rule_t **grown = realloc(state->rules, next * sizeof(*grown));
        if (!grown) { mt_sub_rule_free(rule); return MT_ERR_NOMEM; }
        state->rules = grown;
        state->cap = next;
    }
    state->rules[state->n++] = rule;
    return MT_OK;
}

mt_err_t mt_sub_parse_rules_cancel(const char *list, mt_sub_rule_t ***out_rules,
                                   size_t *out_n, const atomic_bool *cancel) {
    *out_rules = NULL; *out_n = 0;
    if (!list) { return MT_ERR_INVAL; }
    if (canceled(cancel)) { return MT_ERR_CANCELED; }
    if (strncmp(list, "\xef\xbb\xbf", 3) == 0) { list += 3; }
    bool typed = is_typed_list(list);
    char *copy = strdup(list);
    if (!copy) { return MT_ERR_NOMEM; }
    parse_state_t state = {0};
    mt_err_t err = MT_OK;
    if (!typed) {
        /* Preserve legacy tokenization and detection verbatim. */
        char *save = NULL;
        for (char *token = strtok_r(copy, "\n\r,", &save); token;
             token = strtok_r(NULL, "\n\r,", &save)) {
            if (canceled(cancel)) { err = MT_ERR_CANCELED; break; }
            char *text = trim(token);
            if (!*text || *text == '#') { continue; }
            err = append_parsed_rule(&state, text, NULL);
            if (err != MT_OK) { break; }
        }
    } else {
        char canonical[MT_SUB_MAX_LINE_BYTES + 1];
        size_t line_number = 0;
        for (char *line = copy; line && *line;) {
            if (canceled(cancel)) { err = MT_ERR_CANCELED; break; }
            char *end = strpbrk(line, "\r\n"), *next = NULL;
            if (end) {
                next = end + 1;
                if (*end == '\r' && *next == '\n') { next++; }
                *end = '\0';
            }
            line_number++;
            char *text = trim(line);
            if (*text && !line_comment(text)) {
                if (strlen(text) > MT_SUB_MAX_LINE_BYTES) { err = MT_ERR_LIMIT; break; }
                const char *type = NULL, *reason = NULL;
                err = canonical_typed_record(text, canonical, sizeof(canonical), &type, &reason);
                if (err == MT_OK) { err = append_parsed_rule(&state, canonical, type); }
                if (err != MT_OK) {
                    MT_WARN("subscription rule-set line %zu: %s", line_number,
                            reason ? reason : "unable to import rule");
                    break;
                }
            }
            line = next;
        }
    }
    if (err == MT_OK && canceled(cancel)) { err = MT_ERR_CANCELED; }
    mt_lookup_clear(&state.seen); mt_lookup_clear(&state.used); free(copy);
    if (err != MT_OK) { mt_sub_rules_free(state.rules, state.n); return err; }
    *out_rules = state.rules; *out_n = state.n;
    return MT_OK;
}

mt_err_t mt_sub_parse_rules(const char *list, mt_sub_rule_t ***out_rules, size_t *out_n) {
    return mt_sub_parse_rules_cancel(list, out_rules, out_n, NULL);
}

mt_err_t mt_sub_rules_fix_ids(mt_sub_rule_t **rules, size_t n) {
    mt_lookup_t used = {0}; mt_id_pool_t pool = {0};
    mt_err_t err = MT_OK;
    for (size_t i = 0; i < n; i++) {
        if (!rules[i]) { continue; }
        mt_id_t *id = &rules[i]->id;
        if (mt_id_is_zero(*id) || mt_lookup_get(&used, id->b, sizeof(id->b), NULL)) {
            err = mt_id_pool_unique(&used, &pool, id);
        } else { err = mt_lookup_put(&used, id->b, sizeof(id->b), i, NULL); }
        if (err != MT_OK) { break; }
    }
    mt_lookup_clear(&used);
    return err;
}

static mt_err_t reconcile_rules(mt_sub_rule_t **parsed, size_t n,
                                mt_sub_rule_t **existing, size_t n_existing,
                                bool preserve_effective) {
    mt_lookup_t by_text = {0};
    mt_err_t err = MT_OK;
    for (size_t i = 0; i < n_existing; i++) {
        const mt_sub_rule_t *r = existing[i];
        if (!r || !r->rule || !*r->rule) { continue; }
        err = mt_lookup_put(&by_text, r->rule, strlen(r->rule), i, NULL);
        if (err != MT_OK) { break; }
    }
    for (size_t i = 0; i < n && err == MT_OK; i++) {
        mt_sub_rule_t *r = parsed[i]; size_t j;
        if (!r || !r->rule || !mt_lookup_get(&by_text, r->rule, strlen(r->rule), &j)) { continue; }
        const mt_sub_rule_t *old = existing[j];
        r->id = old->id;
        if (preserve_effective) {
            r->enable = old->enable;
            if (old->type && *old->type) { err = mt_strset(&r->type, old->type); }
        }
    }
    mt_lookup_clear(&by_text);
    if (err == MT_OK) { err = mt_sub_rules_fix_ids(parsed, n); }
    return err;
}

mt_err_t mt_sub_reconcile_rules(mt_sub_rule_t **parsed, size_t n,
                                 mt_sub_rule_t **existing, size_t n_existing) {
    return reconcile_rules(parsed, n, existing, n_existing, true);
}

mt_err_t mt_sub_reconcile_rule_ids(mt_sub_rule_t **parsed, size_t n,
                                    mt_sub_rule_t **existing, size_t n_existing) {
    return reconcile_rules(parsed, n, existing, n_existing, false);
}

mt_err_t mt_sub_refresh_rules(const char *list, mt_sub_rule_t **existing,
                              size_t n_existing, mt_sub_rule_t ***out_rules, size_t *out_n) {
    *out_rules = NULL; *out_n = 0;
    mt_sub_rule_t **parsed = NULL; size_t n = 0;
    mt_err_t err = mt_sub_parse_rules(list, &parsed, &n);
    if (err == MT_OK) { err = mt_sub_reconcile_rules(parsed, n, existing, n_existing); }
    if (err != MT_OK) { mt_sub_rules_free(parsed, n); return err; }
    *out_rules = parsed; *out_n = n;
    return MT_OK;
}

/* ---- order-insensitive multiset compare; preserve legacy duplicate rules --- */
typedef struct rule_state { mt_id_t id; bool enable; size_t count; } rule_state_t;

static char *same_rules_key(const mt_sub_rule_t *rule) {
    const char *text = rule->rule ? rule->rule : "";
    const char *type = rule->type && *rule->type ? rule->type : mt_sub_detect_type(text);
    size_t len = strlen(type) + 1 + strlen(text) + 1;
    char *key = malloc(len);
    if (key) { snprintf(key, len, "%s|%s", type, text); }
    return key;
}

bool mt_sub_same_rules(mt_sub_rule_t **left, size_t n_left,
                       mt_sub_rule_t **right, size_t n_right) {
    if (n_left != n_right) { return false; }
    if (!n_left) { return true; }
    rule_state_t *states = calloc(n_left, sizeof(*states));
    if (!states) { return false; }
    mt_lookup_t map = {0};
    size_t left_nil = 0, right_nil = 0;
    bool result = true;
    for (size_t i = 0; i < n_left && result; i++) {
        if (!left[i]) { left_nil++; continue; }
        char *key = same_rules_key(left[i]);
        if (!key) { result = false; break; }
        size_t j = i;
        if (!mt_lookup_get(&map, key, strlen(key), &j)) {
            if (mt_lookup_put(&map, key, strlen(key), i, NULL) != MT_OK) { result = false; }
        }
        free(key);
        rule_state_t *st = &states[j];
        if (st->count && (!mt_id_equal(st->id, left[i]->id) || st->enable != left[i]->enable)) {
            result = false; break;
        }
        st->id = left[i]->id; st->enable = left[i]->enable; st->count++;
    }
    for (size_t i = 0; i < n_right && result; i++) {
        if (!right[i]) { right_nil++; continue; }
        char *key = same_rules_key(right[i]); size_t j;
        if (!key) { result = false; break; }
        bool found = mt_lookup_get(&map, key, strlen(key), &j);
        free(key);
        if (!found || !states[j].count || !mt_id_equal(states[j].id, right[i]->id) ||
            states[j].enable != right[i]->enable) { result = false; break; }
        states[j].count--;
    }
    if (left_nil != right_nil) { result = false; }
    for (size_t i = 0; i < n_left && result; i++) { if (states[i].count) { result = false; } }
    mt_lookup_clear(&map); free(states);
    return result;
}

bool mt_sub_is_due(const mt_subscription_t *sub, int64_t now_unix)
{
    if (sub == NULL || !sub->enable || sub->url == NULL ||
        sub->url[0] == '\0' || sub->interval == 0) {
        return false;
    }
    uint32_t last_check = sub->last_check;
    if (last_check == 0) {
        last_check = sub->last_update;
    }
    if (last_check > 0 &&
        (uint64_t)now_unix < (uint64_t)last_check + (uint64_t)sub->interval) {
        return false;
    }
    return true;
}
