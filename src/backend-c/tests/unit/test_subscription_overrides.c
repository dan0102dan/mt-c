/* Portable config: explicit per-rule overrides survive without downloading a
 * subscription's entire ruleset into the portable export. */
#include "greatest.h"
#include <stdlib.h>
#include <string.h>

#include "magitrickle/yamlio.h"

TEST serialized_overrides_roundtrip_and_apply_after_refresh(void) {
    const char *fixture =
        "configVersion: 0.8.5\n"
        "subscriptions:\n"
        "  - id: aabbccdd\n"
        "    name: Sub\n"
        "    interface: blackhole\n"
        "    enable: true\n"
        "    priority: 100\n"
        "    url: https://example.org/list\n"
        "    interval: 86400\n"
        "    overrides:\n"
        "      - rule: example.org\n"
        "        enable: false\n"
        "        type: domain\n"
        "    rules:\n"
        "      - id: 11223344\n"
        "        rule: example.org\n"
        "        type: namespace\n"
        "        enable: true\n";
    mt_config_t cfg = {0}, again = {0};
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
    ASSERT_EQ(MT_OK, mt_config_load_buffer(&cfg, fixture, strlen(fixture)));
    ASSERT_EQ((size_t)1, cfg.n_subscriptions);
    mt_subscription_t *sub = cfg.subscriptions[0];
    ASSERT_EQ((size_t)1, sub->n_overrides);
    ASSERT_EQ((size_t)1, sub->n_rules);
    ASSERT(!sub->rules[0]->enable);
    ASSERT_STR_EQ("domain", sub->rules[0]->type);

    char *yaml = NULL;
    size_t len = 0;
    ASSERT_EQ(MT_OK, mt_config_save_buffer(&cfg, "0.8.5", &yaml, &len));
    ASSERT(strstr(yaml, "overrides:") != NULL);
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&again));
    ASSERT_EQ(MT_OK, mt_config_load_buffer(&again, yaml, len));
    free(yaml);
    ASSERT_EQ((size_t)1, again.subscriptions[0]->n_overrides);
    ASSERT(!again.subscriptions[0]->rules[0]->enable);

    mt_sub_rule_t *fresh = mt_sub_rule_new();
    ASSERT(fresh != NULL);
    ASSERT_EQ(MT_OK, mt_strset(&fresh->rule, "example.org"));
    ASSERT_EQ(MT_OK, mt_strset(&fresh->type, "namespace"));
    fresh->enable = true;
    mt_sub_rule_t *list[] = {fresh};
    ASSERT_EQ(MT_OK, mt_subscription_apply_overrides(again.subscriptions[0], list, 1));
    ASSERT(!fresh->enable);
    ASSERT_STR_EQ("domain", fresh->type);
    mt_sub_rule_free(fresh);
    mt_config_clear(&again);
    mt_config_clear(&cfg);
    PASS();
}

TEST absence_of_overrides_keeps_old_yaml_byte_contract(void) {
    mt_config_t cfg = {0};
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
    mt_subscription_t *sub = mt_subscription_new();
    ASSERT(sub != NULL);
    ASSERT_EQ(MT_OK, mt_strset(&sub->name, "Example"));
    ASSERT_EQ(MT_OK, mt_strset(&sub->iface, "blackhole"));
    ASSERT_EQ(MT_OK, mt_strset(&sub->url, "https://example.org/list"));
    ASSERT_EQ(MT_OK, mt_config_add_subscription(&cfg, sub));
    char *yaml = NULL;
    size_t len = 0;
    ASSERT_EQ(MT_OK, mt_config_save_buffer(&cfg, "0.8.5", &yaml, &len));
    ASSERT(strstr(yaml, "overrides:") == NULL);
    free(yaml);
    mt_config_clear(&cfg);
    PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(serialized_overrides_roundtrip_and_apply_after_refresh);
    RUN_TEST(absence_of_overrides_keeps_old_yaml_byte_contract);
    GREATEST_MAIN_END();
}
