/* Portable config: explicit per-rule overrides survive without downloading a
 * subscription's entire ruleset into the portable export. */
#include "greatest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "magitrickle/subparse.h"
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

TEST legacy_cache_migrates_only_user_differences(void) {
    const char *fixture =
        "configVersion: 0.8.4.1\n"
        "subscriptions:\n"
        "- id: aabbccdd\n"
        "  url: https://example.org/list\n"
        "  rules:\n"
        "  - {id: aaaa0001, rule: normal.example, type: namespace, enable: true}\n"
        "  - {id: aaaa0002, rule: disabled.example, type: namespace, enable: false}\n"
        "  - {id: aaaa0003, rule: typed.example, type: domain, enable: true}\n"
        "  - {id: aaaa0004, rule: both.example, type: domain, enable: false}\n"
        "  - {id: aaaa0005, rule: 10.0.0.0/8, type: subnet, enable: true}\n";
    mt_config_t cfg = {0}, reloaded = {0};
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
    ASSERT_EQ(MT_OK, mt_config_load_buffer(&cfg, fixture, strlen(fixture)));
    mt_subscription_t *sub = cfg.subscriptions[0];
    ASSERT(sub->overrides_present);
    ASSERT_EQ((size_t)3, sub->n_overrides);
    ASSERT(!mt_subscription_override_find(sub, "normal.example"));
    ASSERT(!mt_subscription_override_find(sub, "10.0.0.0/8"));
    const mt_sub_override_t *disabled = mt_subscription_override_find(sub, "disabled.example");
    ASSERT(disabled); ASSERT(!disabled->type); ASSERT_EQ(0, disabled->enable);
    const mt_sub_override_t *typed = mt_subscription_override_find(sub, "typed.example");
    ASSERT(typed); ASSERT_STR_EQ("domain", typed->type); ASSERT_EQ(-1, typed->enable);
    const mt_sub_override_t *both = mt_subscription_override_find(sub, "both.example");
    ASSERT(both); ASSERT_STR_EQ("domain", both->type); ASSERT_EQ(0, both->enable);
    ASSERT(!sub->rules[1]->enable); ASSERT_STR_EQ("domain", sub->rules[2]->type);
    char *saved = NULL; size_t length = 0;
    ASSERT_EQ(MT_OK, mt_config_save_buffer(&cfg, "0.8.5", &saved, &length));
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&reloaded));
    ASSERT_EQ(MT_OK, mt_config_load_buffer(&reloaded, saved, length));
    ASSERT_EQ((size_t)3, reloaded.subscriptions[0]->n_overrides);
    ASSERT(!reloaded.subscriptions[0]->rules[1]->enable);
    ASSERT_STR_EQ("domain", reloaded.subscriptions[0]->rules[2]->type);
    free(saved); mt_config_clear(&cfg); mt_config_clear(&reloaded);
    PASS();
}

TEST authoritative_empty_and_partial_overrides_reset_effective_cache(void) {
    const char *exceptions[] = {
        "  overrides: []\n",
        "  overrides: [{rule: example.org, enable: false}]\n",
        "  overrides: [{rule: example.org, type: domain}]\n",
    };
    for (size_t i = 0; i < sizeof(exceptions) / sizeof(*exceptions); i++) {
        char fixture[1024];
        snprintf(fixture, sizeof(fixture),
            "configVersion: 0.8.5\nsubscriptions:\n- id: aabbccdd\n"
            "  url: https://example.org/list\n%s"
            "  rules: [{id: aaaa0001, rule: example.org, type: domain, enable: false}]\n",
            exceptions[i]);
        mt_config_t cfg = {0}, reloaded = {0};
        ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
        ASSERT_EQ(MT_OK, mt_config_load_buffer(&cfg, fixture, strlen(fixture)));
        mt_subscription_t *sub = cfg.subscriptions[0];
        ASSERT(sub->overrides_present);
        ASSERT_EQ(i == 0 ? (size_t)0 : (size_t)1, sub->n_overrides);
        ASSERT_EQ(i != 1, sub->rules[0]->enable);
        ASSERT_STR_EQ(i == 2 ? "domain" : "namespace", sub->rules[0]->type);
        char *saved = NULL; size_t length = 0;
        ASSERT_EQ(MT_OK, mt_config_save_buffer(&cfg, "0.8.5", &saved, &length));
        if (i == 0) { ASSERT(strstr(saved, "overrides: []") != NULL); }
        ASSERT_EQ(MT_OK, mt_config_init_defaults(&reloaded));
        ASSERT_EQ(MT_OK, mt_config_load_buffer(&reloaded, saved, length));
        ASSERT_EQ(sub->n_overrides, reloaded.subscriptions[0]->n_overrides);
        ASSERT_EQ(sub->rules[0]->enable, reloaded.subscriptions[0]->rules[0]->enable);
        ASSERT_STR_EQ(sub->rules[0]->type, reloaded.subscriptions[0]->rules[0]->type);
        free(saved); mt_config_clear(&cfg); mt_config_clear(&reloaded);
    }
    PASS();
}

TEST legacy_duplicate_patterns_follow_the_first_cached_occurrence(void) {
    const char *fixture =
        "configVersion: 0.8.4.1\nsubscriptions:\n- id: aabbccdd\n"
        "  rules:\n"
        "  - {rule: normal.example, type: namespace, enable: true}\n"
        "  - {rule: normal.example, type: domain, enable: false}\n"
        "  - {rule: disabled.example, type: namespace, enable: false}\n"
        "  - {rule: disabled.example, type: namespace, enable: true}\n";
    mt_config_t cfg = {0};
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
    ASSERT_EQ(MT_OK, mt_config_load_buffer(&cfg, fixture, strlen(fixture)));
    mt_subscription_t *sub = cfg.subscriptions[0];
    ASSERT_EQ((size_t)1, sub->n_overrides);
    ASSERT(!mt_subscription_override_find(sub, "normal.example"));
    const mt_sub_override_t *disabled = mt_subscription_override_find(sub, "disabled.example");
    ASSERT(disabled != NULL);
    ASSERT(disabled->type == NULL);
    ASSERT_EQ(0, disabled->enable);
    mt_sub_rule_t **refreshed = NULL; size_t count = 0;
    ASSERT_EQ(MT_OK, mt_sub_refresh_rules("normal.example\ndisabled.example",
        sub->rules, sub->n_rules, &refreshed, &count));
    ASSERT_EQ((size_t)2, count);
    ASSERT(refreshed[0]->enable); ASSERT_STR_EQ("namespace", refreshed[0]->type);
    ASSERT(!refreshed[1]->enable); ASSERT_STR_EQ("namespace", refreshed[1]->type);
    mt_sub_rules_free(refreshed, count);
    mt_config_clear(&cfg);
    PASS();
}

TEST fresh_source_preserves_ids_without_resurrecting_removed_overrides(void) {
    mt_sub_rule_t **old = NULL, **fresh = NULL; size_t count = 0, refreshed = 0;
    ASSERT_EQ(MT_OK, mt_sub_parse_rules("example.org", &old, &count));
    old[0]->enable = false;
    ASSERT_EQ(MT_OK, mt_strset(&old[0]->type, "domain"));
    mt_subscription_t *sub = mt_subscription_new(); ASSERT(sub);
    sub->overrides_present = true;
    for (size_t attempt = 0; attempt < 2; attempt++) {
        ASSERT_EQ(MT_OK, mt_sub_parse_rules("example.org", &fresh, &refreshed));
        ASSERT_EQ(MT_OK, mt_sub_reconcile_rule_ids(fresh, refreshed, old, count));
        ASSERT_EQ(MT_OK, mt_subscription_apply_overrides(sub, fresh, refreshed));
        ASSERT(mt_id_equal(old[0]->id, fresh[0]->id));
        ASSERT(fresh[0]->enable); ASSERT_STR_EQ("namespace", fresh[0]->type);
        mt_sub_rules_free(old, count); old = fresh; count = refreshed; fresh = NULL;
    }
    mt_sub_rules_free(old, count); mt_subscription_free(sub);
    PASS();
}

TEST indexed_large_overrides_update_copy_and_apply(void) {
    const size_t count = 50000;
    mt_subscription_t *sub = mt_subscription_new(), *copy = mt_subscription_new();
    ASSERT(sub); ASSERT(copy);
    for (size_t i = 0; i < count; i++) {
        char pattern[64]; snprintf(pattern, sizeof(pattern), "site%zu.example", i);
        ASSERT_EQ(MT_OK, mt_subscription_override_set(sub, pattern, i % 2 ? "domain" : NULL,
                                                      i % 2 ? -1 : 0));
        mt_sub_rule_t *rule = mt_sub_rule_new(); ASSERT(rule);
        ASSERT_EQ(MT_OK, mt_strset(&rule->rule, pattern));
        ASSERT_EQ(MT_OK, mt_strset(&rule->type, "namespace")); rule->enable = true;
        ASSERT_EQ(MT_OK, mt_subscription_add_rule(sub, rule));
    }
    ASSERT_EQ(count, sub->override_index.len);
    ASSERT_EQ(MT_OK, mt_subscription_overrides_copy(copy, sub));
    ASSERT_EQ(count, copy->n_overrides);
    ASSERT(copy->overrides[0] != sub->overrides[0]);
    ASSERT_EQ(MT_OK, mt_subscription_override_set(copy, "site0.example", "domain", 1));
    ASSERT_EQ(count, copy->n_overrides);
    ASSERT_EQ(MT_OK, mt_subscription_override_set(copy, "absent.example", NULL, 0));
    ASSERT_EQ(MT_OK, mt_subscription_apply_overrides(copy, sub->rules, sub->n_rules));
    ASSERT_EQ(count, sub->n_rules); /* An absent source pattern is never recreated. */
    for (size_t i = 0; i < count; i++) {
        ASSERT_EQ(i == 0 || i % 2 != 0, sub->rules[i]->enable);
        ASSERT_STR_EQ(i == 0 || i % 2 != 0 ? "domain" : "namespace", sub->rules[i]->type);
    }
    ASSERT_EQ(0, sub->overrides[0]->enable); /* Changing a copy never changes its source. */
    ASSERT(mt_subscription_override_find(copy, "absent.example"));
    mt_subscription_free(copy); mt_subscription_free(sub);
    PASS();
}

TEST duplicate_override_patterns_are_rejected(void) {
    const char *fixture = "configVersion: 0.8.5\nsubscriptions:\n- id: aabbccdd\n"
        "  overrides: [{rule: example.org, enable: false}, {rule: example.org, type: domain}]\n";
    mt_config_t cfg = {0};
    ASSERT_EQ(MT_OK, mt_config_init_defaults(&cfg));
    ASSERT_EQ(MT_ERR_EXIST, mt_config_load_buffer(&cfg, fixture, strlen(fixture)));
    mt_config_clear(&cfg); PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(serialized_overrides_roundtrip_and_apply_after_refresh);
    RUN_TEST(absence_of_overrides_keeps_old_yaml_byte_contract);
    RUN_TEST(legacy_cache_migrates_only_user_differences);
    RUN_TEST(authoritative_empty_and_partial_overrides_reset_effective_cache);
    RUN_TEST(legacy_duplicate_patterns_follow_the_first_cached_occurrence);
    RUN_TEST(fresh_source_preserves_ids_without_resurrecting_removed_overrides);
    RUN_TEST(indexed_large_overrides_update_copy_and_apply);
    RUN_TEST(duplicate_override_patterns_are_rejected);
    GREATEST_MAIN_END();
}
