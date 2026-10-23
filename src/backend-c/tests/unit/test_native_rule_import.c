/* Typed rule-set import is additive: legacy lists and go-wildcard remain
 * unchanged. Exercise the real parser, match index and subscription models. */
#include "greatest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "magitrickle/match.h"
#include "magitrickle/subparse.h"
#include "magitrickle/sub_runtime.h"

static bool matches(const char *type, const char *pattern, const char *domain)
{
    mt_rule_matcher_t *matcher = mt_rule_matcher_new(type, pattern);
    if (!matcher) { return false; }
    bool result = mt_rule_matcher_match(matcher, domain);
    mt_rule_matcher_free(matcher);
    return result;
}

static bool imported_matches(const char *source, const char *domain)
{
    mt_sub_rule_t **rules = NULL;
    size_t n = 0;
    if (mt_sub_parse_rules(source, &rules, &n) != MT_OK || n != 1) {
        mt_sub_rules_free(rules, n);
        return false;
    }
    char *pattern = mt_sub_rule_runtime_pattern(rules[0]->rule, rules[0]->type);
    bool found = pattern && matches(rules[0]->type, pattern, domain);
    free(pattern);
    mt_sub_rules_free(rules, n);
    return found;
}

TEST imported_keyword_uses_existing_wildcard(void)
{
    ASSERT(imported_matches("DOMAIN-KEYWORD,OpenAI", "api.OPENAI.com"));
    ASSERT(imported_matches("DOMAIN-KEYWORD,labs.google", "x.LABS.GOOGLE.net"));
    ASSERT_FALSE(imported_matches("DOMAIN-KEYWORD,labs.google", "labsXgoogle.net"));
    ASSERT(imported_matches("DOMAIN-KEYWORD,a*b?c", "x.a*b?c.example"));
    ASSERT_FALSE(imported_matches("DOMAIN-KEYWORD,a*b?c", "x.axbyc.example"));
    ASSERT_FALSE(imported_matches("DOMAIN-KEYWORD,longer", "short"));
    ASSERT_FALSE(imported_matches("DOMAIN-KEYWORD,openai", ""));
    mt_sub_rule_t **rules = NULL;
    size_t n = 0;
    ASSERT_EQ(MT_OK, mt_sub_parse_rules("DOMAIN-KEYWORD,", &rules, &n));
    ASSERT(rules == NULL);
    ASSERT_EQ(0u, (unsigned)n);
    PASS();
}

TEST imported_domain_wildcard_uses_existing_type(void)
{
    ASSERT(imported_matches("DOMAIN-WILDCARD,*.EXAMPLE.COM", "a.b.example.com"));
    ASSERT_FALSE(imported_matches("DOMAIN-WILDCARD,*.example.com", "example.com"));
    ASSERT(imported_matches("DOMAIN-WILDCARD,*example*", "example"));
    ASSERT(imported_matches("DOMAIN-WILDCARD,a**b", "ab"));
    ASSERT(imported_matches("DOMAIN-WILDCARD,a?b", "a.b"));
    ASSERT_FALSE(imported_matches("DOMAIN-WILDCARD,a?b", "ab"));
    ASSERT_FALSE(imported_matches("DOMAIN-WILDCARD,a?", "a"));
    ASSERT_FALSE(imported_matches("DOMAIN-WILDCARD,a.b", "aXb"));
    /* Explicitly preserve Go-compatible wildcard quirks for existing rules. */
    ASSERT(matches(MT_RULE_WILDCARD, "a.b", "aXb"));
    ASSERT(matches(MT_RULE_WILDCARD, "a?", "a"));
    ASSERT_FALSE(matches(MT_RULE_WILDCARD, "*openai*", "OPENAI.COM"));
    PASS();
}

TEST indexed_imported_rules(void)
{
    mt_sub_rule_t **rules = NULL;
    size_t n = 0;
    ASSERT_EQ(MT_OK, mt_sub_parse_rules(
        "DOMAIN-KEYWORD,openai\nDOMAIN-WILDCARD,api?.example.com\n",
        &rules, &n));
    mt_matcher_t *matcher = mt_matcher_new();
    ASSERT(matcher != NULL);
    for (size_t i = 0; i < n; i++) {
        ASSERT_STR_EQ("wildcard", rules[i]->type);
        char *pattern = mt_sub_rule_runtime_pattern(rules[i]->rule, rules[i]->type);
        ASSERT(pattern != NULL);
        ASSERT_EQ(MT_OK, mt_matcher_add(matcher, rules[i]->type, pattern));
        free(pattern);
    }
    ASSERT(mt_matcher_match(matcher, "cdn.OPENAI.example"));
    ASSERT(mt_matcher_match(matcher, "API1.EXAMPLE.COM"));
    ASSERT_FALSE(mt_matcher_match(matcher, "api12.example.com"));
    ASSERT_FALSE(mt_matcher_match(matcher, "api1Xexample.com"));
    ASSERT_FALSE(mt_matcher_match(matcher, "unrelated.net"));
    mt_matcher_free(matcher);
    mt_sub_rules_free(rules, n);
    PASS();
}

TEST typed_import_canonical_identity(void)
{
    const char *text =
        "\xef\xbb\xbf# comment\r\n; comment\n// comment\n"
        " domain , EXAMPLE.COM. , proxy \r\n"
        "DOMAIN-SUFFIX,example.com\n"
        "DOMAIN-KEYWORD,example.com\n"
        "HOST-KEYWORD,EXAMPLE.COM,other-policy\n"
        "HOST,example.com\n"
        "DOMAIN-WILDCARD,*.EXAMPLE.COM\n"
        "IP-CIDR,192.0.2.0/24,no-resolve\n"
        "IP6-CIDR,2001:DB8::/32,proxy,no-resolve\n"
        "IP-CIDR,2001:db8:1::/48\n"
        "DOMAIN-REGEX,^a[0-9]{1,3}\\.example\\.com$\n";
    mt_sub_rule_t **rules = NULL;
    size_t n = 0;
    ASSERT_EQ(MT_OK, mt_sub_parse_rules(text, &rules, &n));
    ASSERT_EQ(8u, (unsigned)n);
    const char *expected[] = {
        "DOMAIN,example.com", "DOMAIN-SUFFIX,example.com", "DOMAIN-KEYWORD,example.com",
        "DOMAIN-WILDCARD,*.example.com", "IP-CIDR,192.0.2.0/24",
        "IP-CIDR6,2001:db8::/32", "IP-CIDR6,2001:db8:1::/48",
        "DOMAIN-REGEX,^a[0-9]{1,3}\\.example\\.com$",
    };
    const char *types[] = {"domain", "namespace", "wildcard", "wildcard",
                           "subnet", "subnet6", "subnet6", "regex"};
    for (size_t i = 0; i < n; i++) {
        ASSERT_STR_EQ(expected[i], rules[i]->rule);
        ASSERT_STR_EQ(types[i], mt_sub_detect_type(rules[i]->rule));
        ASSERT_STR_EQ(types[i], rules[i]->type);
        ASSERT_FALSE(mt_id_is_zero(rules[i]->id));
        ASSERT(rules[i]->enable);
        for (size_t j = 0; j < i; j++) { ASSERT_FALSE(mt_id_equal(rules[i]->id, rules[j]->id)); }
    }
    ASSERT_STR_EQ("^a[0-9]{1,3}\\.example\\.com$", mt_sub_rule_pattern(rules[7]->rule));
    mt_sub_rules_free(rules, n);
    PASS();
}

TEST typed_import_skips_invalid_records(void)
{
    const char *bad[] = {
        "process-name,app.exe\n", "GEOIP,CN\n", "RULE-SET,another.list\n",
        "DOMAIN-KEYWORD,\n", "DOMAIN-WILDCARD,[ab].example.com\n",
        "DOMAIN,example..com\n", "DOMAIN-REGEX,[invalid\n",
        "IP-CIDR,999.1.2.3/32\n", "IP-CIDR6,2001:db8::/129\n",
        "IP-CIDR,192.0.2.0/24,src\n",
        "IP-CIDR,192.0.2.0/24,proxy,no-resolve,src\n",
        "DOMAIN,example.com,no-resolve\n", "DOMAIN,example.com,,\n",
        "DOMAIN,example.com,proxy,unknown-flag\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        mt_sub_rule_t **rules = NULL;
        size_t n = 99;
        ASSERT_EQ(MT_OK, mt_sub_parse_rules(bad[i], &rules, &n));
        ASSERT(rules == NULL);
        ASSERT_EQ(0u, (unsigned)n);
        char source[512];
        snprintf(source, sizeof(source), "DOMAIN-KEYWORD,ok\n%sDOMAIN,after.example\n", bad[i]);
        ASSERT_EQ(MT_OK, mt_sub_parse_rules(source, &rules, &n));
        ASSERT_EQ(2u, (unsigned)n);
        ASSERT_STR_EQ("DOMAIN-KEYWORD,ok", rules[0]->rule);
        ASSERT_STR_EQ("DOMAIN,after.example", rules[1]->rule);
        mt_sub_rules_free(rules, n);
    }
    PASS();
}

TEST typed_import_keeps_operational_errors_atomic(void)
{
    atomic_bool cancel = ATOMIC_VAR_INIT(true);
    mt_sub_rule_t **rules = NULL;
    size_t n = 99;
    ASSERT_EQ(MT_ERR_CANCELED,
              mt_sub_parse_rules_cancel("DOMAIN-KEYWORD,openai", &rules, &n, &cancel));
    ASSERT(rules == NULL);
    ASSERT_EQ(0u, (unsigned)n);
    char *large = malloc(MT_SUB_MAX_LINE_BYTES + 128);
    ASSERT(large != NULL);
    const char *prefix = "DOMAIN,kept.example\nGEOIP,CN\nDOMAIN-KEYWORD,";
    size_t offset = strlen(prefix);
    memcpy(large, prefix, offset);
    memset(large + offset, 'x', MT_SUB_MAX_LINE_BYTES);
    large[offset + MT_SUB_MAX_LINE_BYTES] = '\0';
    ASSERT_EQ(MT_ERR_LIMIT, mt_sub_parse_rules(large, &rules, &n));
    ASSERT(rules == NULL);
    ASSERT_EQ(0u, (unsigned)n);
    free(large);
    ASSERT_EQ(MT_ERR_INVAL, mt_sub_parse_rules(NULL, &rules, &n));
    ASSERT(rules == NULL);
    ASSERT_EQ(0u, (unsigned)n);
    PASS();
}

TEST typed_cache_restore_and_runtime(void)
{
    mt_subscription_t *sub = mt_subscription_new();
    ASSERT(sub != NULL);
    ASSERT_EQ(MT_OK, mt_strset(&sub->iface, "wg0"));
    sub->enable = true;
    sub->overrides_present = true;
    ASSERT_EQ(MT_OK, mt_sub_parse_rules(
        "DOMAIN,openai\nDOMAIN-KEYWORD,openai\nDOMAIN-WILDCARD,*.example.com\n"
        "IP-CIDR,192.0.2.0/24\n", &sub->rules, &sub->n_rules));
    /* Emulate saved effective values: reload must recover SOURCE defaults
     * before applying the current override list, including its removals. */
    ASSERT_EQ(MT_OK, mt_strset(&sub->rules[1]->type, "domain"));
    sub->rules[1]->enable = false;
    ASSERT_EQ(MT_OK, mt_subscription_restore_cached_rules(sub));
    ASSERT_STR_EQ("wildcard", sub->rules[1]->type);
    ASSERT(sub->rules[1]->enable);
    ASSERT_EQ(MT_OK, mt_subscription_override_set(sub, "DOMAIN,openai", NULL, 0));
    ASSERT_EQ(MT_OK, mt_subscription_override_set(sub, "DOMAIN-KEYWORD,openai", "namespace", -1));
    ASSERT_EQ(MT_OK, mt_subscription_restore_cached_rules(sub));
    ASSERT_FALSE(sub->rules[0]->enable);
    ASSERT(sub->rules[1]->enable);
    ASSERT_STR_EQ("namespace", sub->rules[1]->type);
    mt_group_t *group = mt_sub_runtime_group(sub);
    ASSERT(group != NULL);
    ASSERT(group->enable);
    ASSERT_STR_EQ("openai", group->rules[0]->rule);
    ASSERT_STR_EQ("openai", group->rules[1]->rule);
    ASSERT_STR_EQ("namespace", group->rules[1]->type);
    ASSERT_STR_EQ("@mtc:*.example.com", group->rules[2]->rule);
    ASSERT_STR_EQ("wildcard", group->rules[2]->type);
    ASSERT_STR_EQ("192.0.2.0/24", group->rules[3]->rule);
    ASSERT_STR_EQ("DOMAIN-KEYWORD,openai", sub->rules[1]->rule);
    mt_group_free(group);
    mt_subscription_free(sub);
    PASS();
}

TEST typed_refresh_stable_ids_with_skipped_records(void)
{
    mt_sub_rule_t **old = NULL, **fresh = NULL;
    size_t n_old = 0, n_fresh = 0;
    ASSERT_EQ(MT_OK, mt_sub_parse_rules("HOST-KEYWORD,OpenAI,policy1\n", &old, &n_old));
    old[0]->enable = false;
    ASSERT_EQ(MT_OK, mt_sub_refresh_rules("DOMAIN-KEYWORD,openai,policy2\n",
                                          old, n_old, &fresh, &n_fresh));
    ASSERT_EQ(1u, (unsigned)n_fresh);
    ASSERT(mt_id_equal(old[0]->id, fresh[0]->id));
    ASSERT_FALSE(fresh[0]->enable);
    ASSERT_STR_EQ("wildcard", fresh[0]->type);
    mt_sub_rules_free(fresh, n_fresh);
    fresh = NULL;
    ASSERT_EQ(MT_OK, mt_sub_refresh_rules("DOMAIN-KEYWORD,openai\nGEOIP,CN",
                                          old, n_old, &fresh, &n_fresh));
    ASSERT_EQ(1u, (unsigned)n_fresh);
    ASSERT(mt_id_equal(old[0]->id, fresh[0]->id));
    ASSERT_FALSE(fresh[0]->enable);
    ASSERT_STR_EQ("wildcard", fresh[0]->type);
    ASSERT_FALSE(old[0]->enable);
    ASSERT_STR_EQ("DOMAIN-KEYWORD,openai", old[0]->rule);
    mt_sub_rules_free(fresh, n_fresh);
    fresh = NULL;
    ASSERT_EQ(MT_OK, mt_sub_refresh_rules("GEOIP,CN\nDOMAIN-WILDCARD,",
                                          old, n_old, &fresh, &n_fresh));
    ASSERT(fresh == NULL);
    ASSERT_EQ(0u, (unsigned)n_fresh);
    mt_sub_rules_free(old, n_old);
    PASS();
}

TEST legacy_list_unchanged(void)
{
    mt_sub_rule_t **rules = NULL;
    size_t n = 0;
    ASSERT_EQ(MT_OK, mt_sub_parse_rules("# comment\nexample.com,a.com\n*.wild.net\n"
                                       "^a\\.net$\nexample.com\n", &rules, &n));
    ASSERT_EQ(4u, (unsigned)n);
    ASSERT_STR_EQ("namespace", rules[0]->type);
    ASSERT_STR_EQ("wildcard", rules[2]->type);
    ASSERT_STR_EQ("regex", rules[3]->type);
    ASSERT_STR_EQ("example.com", mt_sub_rule_pattern(rules[0]->rule));
    ASSERT_STR_EQ("UNKNOWN,foo", mt_sub_rule_pattern("UNKNOWN,foo"));
    ASSERT(mt_sub_rule_pattern(NULL) == NULL);
    mt_sub_rules_free(rules, n);
    PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(imported_keyword_uses_existing_wildcard);
    RUN_TEST(imported_domain_wildcard_uses_existing_type);
    RUN_TEST(indexed_imported_rules);
    RUN_TEST(typed_import_canonical_identity);
    RUN_TEST(typed_import_skips_invalid_records);
    RUN_TEST(typed_import_keeps_operational_errors_atomic);
    RUN_TEST(typed_cache_restore_and_runtime);
    RUN_TEST(typed_refresh_stable_ids_with_skipped_records);
    RUN_TEST(legacy_list_unchanged);
    GREATEST_MAIN_END();
}
