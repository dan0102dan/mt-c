/* The interactive editor must not acknowledge ambiguous or ignored edits.
 * Keep the legacy loader's compatibility semantics unchanged; validate the
 * document shape here, then use that same loader for model values. */
#include "magitrickle/yamlio.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>

#include "magitrickle/lookup.h"
#include "magitrickle/match.h"
#include "magitrickle/subparse.h"
#include "yaml_scalar.h"

typedef enum editor_shape {
    SH_ROOT, SH_APP, SH_GROUP, SH_RULE, SH_SUBSCRIPTION, SH_SUB_RULE,
    SH_OVERRIDE, SH_PROFILE, SH_SCALAR, SH_STRINGS, SH_GROUPS, SH_RULES,
    SH_SUBSCRIPTIONS, SH_SUB_RULES, SH_OVERRIDES, SH_PROFILES
} editor_shape_t;

typedef struct editor_field {
    const char *name;
    editor_shape_t shape;
} editor_field_t;

static const editor_field_t root_fields[] = {
    {"configVersion", SH_SCALAR}, {"app", SH_APP}, {"groups", SH_GROUPS},
    {"subscriptions", SH_SUBSCRIPTIONS}, {"profiles", SH_PROFILES}, {NULL, 0}
};
static const editor_field_t group_fields[] = {
    {"id", SH_SCALAR}, {"name", SH_SCALAR}, {"color", SH_SCALAR},
    {"interface", SH_SCALAR}, {"profile", SH_SCALAR}, {"enable", SH_SCALAR},
    {"priority", SH_SCALAR}, {"rules", SH_RULES}, {NULL, 0}
};
static const editor_field_t rule_fields[] = {
    {"id", SH_SCALAR}, {"name", SH_SCALAR}, {"type", SH_SCALAR},
    {"rule", SH_SCALAR}, {"enable", SH_SCALAR}, {NULL, 0}
};
static const editor_field_t subscription_fields[] = {
    {"id", SH_SCALAR}, {"name", SH_SCALAR}, {"interface", SH_SCALAR},
    {"profile", SH_SCALAR}, {"enable", SH_SCALAR}, {"priority", SH_SCALAR},
    {"url", SH_SCALAR}, {"interval", SH_SCALAR}, {"last_update", SH_SCALAR},
    {"rules", SH_SUB_RULES}, {"overrides", SH_OVERRIDES}, {NULL, 0}
};
static const editor_field_t sub_rule_fields[] = {
    {"id", SH_SCALAR}, {"type", SH_SCALAR}, {"rule", SH_SCALAR},
    {"enable", SH_SCALAR}, {NULL, 0}
};
static const editor_field_t override_fields[] = {
    {"type", SH_SCALAR}, {"rule", SH_SCALAR}, {"enable", SH_SCALAR}, {NULL, 0}
};
static const editor_field_t profile_fields[] = {
    {"id", SH_SCALAR}, {"name", SH_SCALAR}, {"interfaces", SH_STRINGS},
    {"on_unavailable", SH_SCALAR}, {NULL, 0}
};

static mt_err_t diagnostic(mt_config_diagnostic_t *out, const yaml_mark_t *mark,
                            mt_err_t err, const char *message, const char *key)
{
    if (out) {
        out->line = mark ? mark->line + 1 : 0;
        out->column = mark ? mark->column + 1 : 0;
        snprintf(out->message, sizeof(out->message), "%s%s%.120s", message,
                 key ? ": " : "", key ? key : "");
    }
    return err;
}

static bool node_null(const yaml_node_t *node)
{
    return node->type == YAML_SCALAR_NODE &&
        node->data.scalar.style == YAML_PLAIN_SCALAR_STYLE &&
        mt_yaml_resolve_plain((const char *)node->data.scalar.value).kind == MT_SCALAR_NULL;
}

static const editor_field_t *shape_fields(editor_shape_t shape)
{
    switch (shape) {
    case SH_ROOT: return root_fields;
    case SH_GROUP: return group_fields;
    case SH_RULE: return rule_fields;
    case SH_SUBSCRIPTION: return subscription_fields;
    case SH_SUB_RULE: return sub_rule_fields;
    case SH_OVERRIDE: return override_fields;
    case SH_PROFILE: return profile_fields;
    default: return NULL;
    }
}

static editor_shape_t element_shape(editor_shape_t shape)
{
    switch (shape) {
    case SH_GROUPS: return SH_GROUP;
    case SH_RULES: return SH_RULE;
    case SH_SUBSCRIPTIONS: return SH_SUBSCRIPTION;
    case SH_SUB_RULES: return SH_SUB_RULE;
    case SH_OVERRIDES: return SH_OVERRIDE;
    case SH_PROFILES: return SH_PROFILE;
    default: return SH_SCALAR;
    }
}

static bool known_rule_type(const char *type)
{
    return type && (strcmp(type, MT_RULE_DOMAIN) == 0 || strcmp(type, MT_RULE_NAMESPACE) == 0 ||
        strcmp(type, MT_RULE_WILDCARD) == 0 || strcmp(type, MT_RULE_REGEX) == 0 ||
        strcmp(type, MT_RULE_SUBNET) == 0 || strcmp(type, MT_RULE_SUBNET6) == 0);
}

static mt_err_t check_shape(yaml_document_t *doc, yaml_node_t *node,
                            editor_shape_t shape, const cJSON *app_schema,
                            unsigned depth, mt_config_diagnostic_t *out)
{
    if (!node || depth > 16) {
        return diagnostic(out, node ? &node->start_mark : NULL, MT_ERR_INVAL,
                          "Invalid or recursive YAML structure", NULL);
    }
    if (node->type == YAML_SCALAR_NODE &&
        memchr(node->data.scalar.value, 0, node->data.scalar.length)) {
        return diagnostic(out, &node->start_mark, MT_ERR_INVAL, "NUL is not allowed", NULL);
    }
    if (shape != SH_ROOT && node_null(node)) { return MT_OK; }
    if (shape == SH_SCALAR) {
        return node->type == YAML_SCALAR_NODE ? MT_OK :
            diagnostic(out, &node->start_mark, MT_ERR_INVAL, "Expected a scalar value", NULL);
    }
    if (shape >= SH_STRINGS) {
        if (node->type != YAML_SEQUENCE_NODE) {
            return diagnostic(out, &node->start_mark, MT_ERR_INVAL, "Expected a YAML sequence", NULL);
        }
        for (yaml_node_item_t *i = node->data.sequence.items.start;
             i < node->data.sequence.items.top; i++) {
            mt_err_t err = check_shape(doc, yaml_document_get_node(doc, *i),
                                      element_shape(shape), NULL, depth + 1, out);
            if (err != MT_OK) { return err; }
        }
        return MT_OK;
    }
    if (node->type != YAML_MAPPING_NODE) {
        return diagnostic(out, &node->start_mark, MT_ERR_INVAL, "Expected a YAML mapping", NULL);
    }
    mt_lookup_t keys = {0};
    mt_err_t err = MT_OK;
    for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
         pair < node->data.mapping.pairs.top; pair++) {
        yaml_node_t *key = yaml_document_get_node(doc, pair->key);
        if (!key || key->type != YAML_SCALAR_NODE || !key->data.scalar.length ||
            memchr(key->data.scalar.value, 0, key->data.scalar.length)) {
            err = diagnostic(out, key ? &key->start_mark : NULL, MT_ERR_INVAL,
                             "Configuration keys must be nonempty text", NULL);
            break;
        }
        const char *name = (const char *)key->data.scalar.value;
        bool inserted = false;
        err = mt_lookup_put(&keys, name, key->data.scalar.length, 0, &inserted);
        if (err != MT_OK) { break; }
        if (!inserted) {
            err = diagnostic(out, &key->start_mark, MT_ERR_INVAL, "Duplicate configuration key", name);
            break;
        }
        editor_shape_t child_shape = SH_SCALAR;
        const cJSON *child_schema = NULL;
        bool known = false;
        if (shape == SH_APP) {
            child_schema = cJSON_GetObjectItemCaseSensitive(app_schema, name);
            known = child_schema != NULL;
            if (cJSON_IsObject(child_schema)) { child_shape = SH_APP; }
            else if (cJSON_IsArray(child_schema)) { child_shape = SH_STRINGS; }
        } else {
            for (const editor_field_t *field = shape_fields(shape); field && field->name; field++) {
                if (strcmp(field->name, name) != 0) { continue; }
                known = true; child_shape = field->shape;
                if (child_shape == SH_APP) { child_schema = app_schema; }
                break;
            }
        }
        if (!known) {
            err = diagnostic(out, &key->start_mark, MT_ERR_INVAL,
                             "Unsupported configuration field", name);
            break;
        }
        err = check_shape(doc, yaml_document_get_node(doc, pair->value), child_shape,
                          child_schema, depth + 1, out);
        if (err != MT_OK) { break; }
        if ((shape == SH_RULE || shape == SH_SUB_RULE || shape == SH_OVERRIDE) &&
            strcmp(name, "type") == 0) {
            yaml_node_t *value = yaml_document_get_node(doc, pair->value);
            if (!(shape == SH_OVERRIDE && node_null(value)) &&
                (node_null(value) || !known_rule_type((const char *)value->data.scalar.value))) {
                err = diagnostic(out, &value->start_mark, MT_ERR_INVAL, "Unsupported rule type", NULL);
                break;
            }
        }
    }
    mt_lookup_clear(&keys);
    if (err == MT_OK && (shape == SH_GROUP || shape == SH_RULE ||
                        shape == SH_SUBSCRIPTION || shape == SH_SUB_RULE)) {
        yaml_node_t *id = NULL;
        for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
             pair < node->data.mapping.pairs.top; pair++) {
            yaml_node_t *key = yaml_document_get_node(doc, pair->key);
            if (strcmp((const char *)key->data.scalar.value, "id") == 0) {
                id = yaml_document_get_node(doc, pair->value); break;
            }
        }
        if (!id || id->type != YAML_SCALAR_NODE || node_null(id) || !id->data.scalar.length) {
            err = diagnostic(out, &node->start_mark, MT_ERR_INVAL,
                             "Every group, subscription and rule needs an explicit ID", NULL);
        }
    }
    if (err == MT_OK && (shape == SH_RULE || shape == SH_SUB_RULE)) {
        bool have_type = false, have_rule = false;
        for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
             pair < node->data.mapping.pairs.top; pair++) {
            yaml_node_t *key = yaml_document_get_node(doc, pair->key);
            const char *name = (const char *)key->data.scalar.value;
            if (strcmp(name, "type") == 0) { have_type = true; }
            if (strcmp(name, "rule") == 0) {
                yaml_node_t *value = yaml_document_get_node(doc, pair->value);
                have_rule = value->type == YAML_SCALAR_NODE && !node_null(value) && value->data.scalar.length > 0;
            }
        }
        if (!have_type || !have_rule) {
            err = diagnostic(out, &node->start_mark, MT_ERR_INVAL, "Rule type and pattern are required", NULL);
        }
    }
    return err;
}

static mt_err_t check_rule(const char *type, const char *pattern, bool subscription)
{
    if (!known_rule_type(type) || !pattern || !*pattern) { return MT_ERR_INVAL; }
    char *runtime = subscription ? mt_sub_rule_runtime_pattern(pattern, type) : strdup(pattern);
    if (!runtime) { return MT_ERR_NOMEM; }
    mt_err_t err = MT_OK;
    if (strcmp(type, MT_RULE_SUBNET) == 0 || strcmp(type, MT_RULE_SUBNET6) == 0) {
        char address[INET6_ADDRSTRLEN];
        const char *slash = strchr(runtime, '/');
        size_t length = slash ? (size_t)(slash - runtime) : strlen(runtime);
        bool ipv6 = strcmp(type, MT_RULE_SUBNET6) == 0;
        unsigned char parsed[16];
        if (!length || length >= sizeof(address)) { err = MT_ERR_INVAL; }
        else {
            memcpy(address, runtime, length); address[length] = '\0';
            if (inet_pton(ipv6 ? AF_INET6 : AF_INET, address, parsed) != 1) { err = MT_ERR_INVAL; }
        }
        if (slash) {
            unsigned prefix = 0;
            if (!slash[1]) { err = MT_ERR_INVAL; }
            for (const char *p = slash + 1; *p && err == MT_OK; p++) {
                if (*p < '0' || *p > '9' || prefix > 128) { err = MT_ERR_INVAL; break; }
                prefix = prefix * 10 + (unsigned)(*p - '0');
            }
            if (prefix > (ipv6 ? 128u : 32u)) { err = MT_ERR_INVAL; }
        }
    } else if (strcmp(type, MT_RULE_REGEX) == 0 || strcmp(type, MT_RULE_WILDCARD) == 0) {
        mt_rule_matcher_t *matcher = mt_rule_matcher_new(type, runtime);
        if (!matcher) { err = MT_ERR_NOMEM; }
        else if (!mt_rule_matcher_ok(matcher)) { err = MT_ERR_INVAL; }
        mt_rule_matcher_free(matcher);
    }
    free(runtime);
    return err;
}

static mt_err_t check_model_rules(const mt_config_t *cfg)
{
    for (size_t i = 0; i < cfg->n_groups; i++) {
        const mt_group_t *g = cfg->groups[i];
        for (size_t j = 0; j < g->n_rules; j++) {
            mt_err_t err = check_rule(g->rules[j]->type, g->rules[j]->rule, false);
            if (err != MT_OK) { return err; }
        }
    }
    for (size_t i = 0; i < cfg->n_subscriptions; i++) {
        const mt_subscription_t *s = cfg->subscriptions[i];
        mt_lookup_t ids = {0};
        mt_err_t err = MT_OK;
        for (size_t j = 0; j < s->n_rules; j++) {
            const mt_sub_rule_t *rule = s->rules[j];
            bool inserted = false;
            err = mt_lookup_put(&ids, rule->id.b, sizeof(rule->id.b), j, &inserted);
            if (err == MT_OK && !inserted) { err = MT_ERR_EXIST; }
            if (err == MT_OK) { err = check_rule(rule->type, rule->rule, true); }
            if (err != MT_OK) { break; }
        }
        mt_lookup_clear(&ids);
        if (err != MT_OK) { return err; }
        for (size_t j = 0; j < s->n_overrides; j++) {
            const mt_sub_override_t *item = s->overrides[j];
            if (item->type && (err = check_rule(item->type, item->rule, true)) != MT_OK) { return err; }
        }
    }
    return MT_OK;
}

mt_err_t mt_config_load_editor_buffer(mt_config_t *cfg, const char *buf, size_t len,
                                      mt_config_diagnostic_t *out)
{
    if (out) { memset(out, 0, sizeof(*out)); }
    if (!cfg || !buf || !len || memchr(buf, 0, len)) {
        return diagnostic(out, NULL, MT_ERR_INVAL, "Expected nonempty UTF-8 YAML", NULL);
    }
    yaml_parser_t parser;
    yaml_document_t doc;
    if (!yaml_parser_initialize(&parser)) { return MT_ERR_NOMEM; }
    yaml_parser_set_input_string(&parser, (const unsigned char *)buf, len);
    if (!yaml_parser_load(&parser, &doc)) {
        mt_err_t err = diagnostic(out, &parser.problem_mark, MT_ERR_PROTO,
                                  "YAML syntax error", parser.problem);
        yaml_parser_delete(&parser);
        return err;
    }
    cJSON *schema = NULL;
    mt_err_t err = mt_config_save_json(cfg, "0.0.0", &schema);
    if (err == MT_OK) {
        err = check_shape(&doc, yaml_document_get_root_node(&doc), SH_ROOT,
                          cJSON_GetObjectItemCaseSensitive(schema, "app"), 0, out);
    }
    cJSON_Delete(schema);
    yaml_document_delete(&doc);
    if (err == MT_OK) {
        if (!yaml_parser_load(&parser, &doc)) {
            err = diagnostic(out, &parser.problem_mark, MT_ERR_PROTO,
                             "YAML syntax error", parser.problem);
        } else {
            yaml_node_t *extra = yaml_document_get_root_node(&doc);
            if (extra) {
                err = diagnostic(out, &extra->start_mark, MT_ERR_INVAL,
                                 "Only one YAML document is allowed", NULL);
            }
            yaml_document_delete(&doc);
        }
    }
    yaml_parser_delete(&parser);
    if (err != MT_OK) { return err; }
    err = mt_config_load_buffer(cfg, buf, len);
    if (err == MT_OK) { err = check_model_rules(cfg); }
    if (err != MT_OK) {
        diagnostic(out, NULL, err, err == MT_ERR_STATE ?
            "Unsupported configVersion; expected 0.x" :
            "Invalid configuration values, rule patterns, IDs, priorities or profile references", NULL);
    }
    return err;
}
