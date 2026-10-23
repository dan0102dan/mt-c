#include "greatest.h"
#include "magitrickle/yamlio.h"
#include <yaml.h>
#include <stdlib.h>
#include <string.h>

/* Independent oracle for the old API representation, used only in tests. */
static cJSON *from_yaml(yaml_document_t *doc, yaml_node_t *n) {
    if (!n) { return NULL; }
    if (n->type == YAML_SEQUENCE_NODE) {
        cJSON *a = cJSON_CreateArray();
        for (yaml_node_item_t *i = n->data.sequence.items.start; a && i < n->data.sequence.items.top; i++) {
            cJSON *v = from_yaml(doc, yaml_document_get_node(doc, *i));
            if (!v || !cJSON_AddItemToArray(a, v)) { cJSON_Delete(v); cJSON_Delete(a); return NULL; }
        }
        return a;
    }
    if (n->type == YAML_MAPPING_NODE) {
        cJSON *o = cJSON_CreateObject();
        for (yaml_node_pair_t *i = n->data.mapping.pairs.start; o && i < n->data.mapping.pairs.top; i++) {
            yaml_node_t *key = yaml_document_get_node(doc, i->key);
            cJSON *v = from_yaml(doc, yaml_document_get_node(doc, i->value));
            if (!key || key->type != YAML_SCALAR_NODE || !v ||
                !cJSON_AddItemToObject(o, (char *)key->data.scalar.value, v)) {
                cJSON_Delete(v); cJSON_Delete(o); return NULL;
            }
        }
        return o;
    }
    const char *v = (char *)n->data.scalar.value;
    if (n->data.scalar.style == YAML_PLAIN_SCALAR_STYLE) {
        if (!strcmp(v, "true")) { return cJSON_CreateBool(true); }
        if (!strcmp(v, "false")) { return cJSON_CreateBool(false); }
        if (!strcmp(v, "null")) { return cJSON_CreateNull(); }
        if (*v >= '0' && *v <= '9') {
            const char *p = v;
            while (*p >= '0' && *p <= '9') { p++; }
            if (!*p && (size_t)(p-v) < 16) { return cJSON_CreateNumber(strtod(v, NULL)); }
        }
    }
    return cJSON_CreateString(v);
}

TEST direct_json_has_the_same_schema_and_scalar_types_as_canonical_yaml(void) {
    const char *fixtures[] = {
        "configVersion: 0.8\n",
        "configVersion: 0.8\napp: {logLevel: debug, showAllInterfaces: true, link: [br0, br1]}\n"
        "profiles: [{id: primary, name: 'Профиль', interfaces: [tun0, tun1]}]\n"
        "groups: [{id: '12345678', name: 'true', color: '#aabbcc', profile: primary, priority: 90, "
        "rules: [{id: '11223344', type: domain, name: '01', rule: example.org}]}]\n"
        "subscriptions: [{id: aabbccdd, name: 'null', url: https://example.org/list, interval: 10, "
        "profile: primary, last_update: 1234567890, overrides: [{rule: example.net, enable: false}], "
        "rules: [{id: abcdef01, rule: example.net, type: domain, enable: false}]}]\n",
        "configVersion: 0.8\napp: {dnsProxy: {timeout: 1m30s, maxConcurrent: 65535}, "
        "netfilter: {ipset: {additionalTTL: 2h}}}\n"
        "groups: [{id: abcdef12, name: \"line 1\\nline 2\", enable: false, rules: []}]\n"
    };
    for (size_t i = 0; i < sizeof(fixtures)/sizeof(fixtures[0]); i++) {
        mt_config_t cfg = {0}; ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
        ASSERT_EQ(MT_OK, mt_config_load_buffer(&cfg, fixtures[i], strlen(fixtures[i])));
        char *text = NULL; size_t len = 0;
        ASSERT_EQ(MT_OK, mt_config_save_buffer(&cfg, "0.8.6", &text, &len));
        yaml_parser_t parser; yaml_document_t doc;
        ASSERT(yaml_parser_initialize(&parser));
        yaml_parser_set_input_string(&parser, (unsigned char *)text, len);
        ASSERT(yaml_parser_load(&parser, &doc)); yaml_parser_delete(&parser);
        cJSON *old = from_yaml(&doc, yaml_document_get_root_node(&doc)); ASSERT(old);
        yaml_document_delete(&doc);
        cJSON *json = NULL; ASSERT_EQ(MT_OK, mt_config_save_json(&cfg, "0.8.6", &json));
        ASSERT(cJSON_Compare(old, json, true));
        char *wire = cJSON_PrintUnformatted(json); ASSERT(wire);
        mt_config_t reloaded = {0}; ASSERT_EQ(MT_OK, mt_config_init_defaults(&reloaded));
        ASSERT_EQ(MT_OK, mt_config_load_buffer(&reloaded, wire, strlen(wire)));
        char *again = NULL; size_t again_len = 0;
        ASSERT_EQ(MT_OK, mt_config_save_buffer(&reloaded, "0.8.6", &again, &again_len));
        ASSERT_EQ(len, again_len); ASSERT_STR_EQ(text, again);
        mt_config_clear(&reloaded); mt_config_clear(&cfg);
        free(text); free(again); free(wire); cJSON_Delete(json); cJSON_Delete(old);
    }
    PASS();
}
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(direct_json_has_the_same_schema_and_scalar_types_as_canonical_yaml);
    GREATEST_MAIN_END();
}
