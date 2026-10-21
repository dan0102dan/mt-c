/* Typed rule-set import is additive: legacy lists and go-wildcard remain
 * unchanged. Exercise the real parser, match index and subscription models. */
#include "greatest.h"

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

TEST native_keyword(void)
{
    ASSERT(matches(MT_RULE_KEYWORD, "OpenAI", "api.OPENAI.com"));
    ASSERT(matches(MT_RULE_KEYWORD, "labs.google", "x.LABS.GOOGLE.net"));
    ASSERT_FALSE(matches(MT_RULE_KEYWORD, "labs.google", "labsXgoogle.net"));
    ASSERT(matches(MT_RULE_KEYWORD, "a*b?c", "x.a*b?c.example"));
    ASSERT_FALSE(matches(MT_RULE_KEYWORD, "a*b?c", "x.axbyc.example"));
    ASSERT_FALSE(matches(MT_RULE_KEYWORD, "", "example.com"));
    ASSERT_FALSE(matches(MT_RULE_KEYWORD, "longer", "short"));
    ASSERT_FALSE(matches(MT_RULE_KEYWORD, "openai", ""));
    mt_rule_matcher_t *empty = mt_rule_matcher_new(MT_RULE_KEYWORD, "");
    ASSERT(empty != NULL);
    ASSERT_FALSE(mt_rule_matcher_ok(empty));
    mt_rule_matcher_free(empty);
    PASS();
}

TEST native_wildcard(void)
{
    ASSERT(matches(MT_RULE_DOMAIN_WILDCARD, "*.EXAMPLE.COM", "a.b.example.com"));
    ASSERT_FALSE(matches(MT_RULE_DOMAIN_WILDCARD, "*.example.com", "example.com"));
    ASSERT(matches(MT_RULE_DOMAIN_WILDCARD, "*example*", "example"));
    ASSERT(matches(MT_RULE_DOMAIN_WILDCARD, "a**b", "ab"));
    ASSERT(matches(MT_RULE_DOMAIN_WILDCARD, "a?b", "a.b"));
    ASSERT_FALSE(matches(MT_RULE_DOMAIN_WILDCARD, "a?b", "ab"));
    ASSERT_FALSE(matches(MT_RULE_DOMAIN_WILDCARD, "a?", "a"));
    ASSERT_FALSE(matches(MT_RULE_DOMAIN_WILDCARD, "a.b", "aXb"));
    ASSERT_FALSE(matches(MT_RULE_DOMAIN_WILDCARD, "", ""));
    /* Explicitly retain legacy dot, case and trailing-question-mark quirks. */
    ASSERT(matches(MT_RULE_WILDCARD, "a.b", "aXb"));
    ASSERT(matches(MT_RULE_WILDCARD, "a?", "a"));
    ASSERT_FALSE(matches(MT_RULE_WILDCARD, "*openai*", "OPENAI.COM"));
    PASS();
}

TEST indexed_native_rules(void)
{
    mt_matcher_t *matcher = mt_matcher_new();
    ASSERT(matcher != NULL);
    ASSERT_EQ(MT_OK, mt_matcher_add(matcher, MT_RULE_KEYWORD, "OpenAI"));
    ASSERT_EQ(MT_OK, mt_matcher_add(matcher, MT_RULE_DOMAIN_WILDCARD, "api?.example.com"));
    ASSERT(mt_matcher_match(matcher, "cdn.OPENAI.example"));
    ASSERT(mt_matcher_match(matcher, "API1.EXAMPLE.COM"));
    ASSERT_FALSE(mt_matcher_match(matcher, "api12.example.com"));
    ASSERT_FALSE(mt_matcher_match(matcher, "api1Xexample.com"));
    ASSERT_FALSE(mt_matcher_match(matcher, "unrelated.net"));
    mt_matcher_free(matcher);
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
    const char *types[] = {"domain", "namespace", "keyword", "domain-wildcard",
                           "subnet", "subnet6", "subnet6", "regex"};
    for (size_t i = 0; i < n; i++) {
        ASSERT_STR_EQ(expected[i], rules[i]->rule);
        ASSERT_STR_EQ(types[i], rules[i]->type);
        ASSERT_STR_EQ(types[i], mt_sub_detect_type(rules[i]->rule));
        ASSERT_FALSE(mt_id_is_zero(rules[i]->id));
        ASSERT(rules[i]->enable);
        for (size_t j = 0; j < i; j++) { ASSERT_FALSE(mt_id_equal(rules[i]->id, rules[j]->id)); }
    }
    ASSERT_STR_EQ("^a[0-9]{1,3}\\.example\\.com$", mt_sub_rule_pattern(rules[7]->rule));
    mt_sub_rules_free(rules, n);
    PASS();
}

TEST typed_import_rejects_atomically(void)
{
    const char *bad[] = {
        "DOMAIN-KEYWORD,ok\nPROCESS-NAME,app.exe\n",
        "process-name,app.exe\n", "GEOIP,CN\n", "RULE-SET,another.list\n",
        "DOMAIN-KEYWORD,\n", "DOMAIN-WILDCARD,[ab].example.com\n",
        "DOMAIN,example..com\n", "DOMAIN-REGEX,[invalid\n",
        "IP-CIDR,999.1.2.3/32\n", "IP-CIDR6,2001:db8::/129\n",
        "IP-CIDR,192.0.2.0/24,src\n",
        "IP-CIDR,192.0.2.0/24,proxy,no-resolve,src\n",
        "DOMAIN,example.com,no-resolve\n", "DOMAIN,example.com,,\n",
        "DOMAIN,example.com,proxy,unknown-flag\n",
        "DOMAIN-KEYWORD,ok\nlegacy.example.com\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        mt_sub_rule_t **rules = NULL;
        size_t n = 99;
        ASSERT_EQ(MT_ERR_INVAL, mt_sub_parse_rules(bad[i], &rules, &n));
        ASSERT(rules == NULL);
        ASSERT_EQ(0u, (unsigned)n);
    }
    atomic_bool cancel = ATOMIC_VAR_INIT(true);
    mt_sub_rule_t **rules = NULL;
    size_t n = 99;
    ASSERT_EQ(MT_ERR_CANCELED,
              mt_sub_parse_rules_cancel("DOMAIN-KEYWORD,openai", &rules, &n, &cancel));
    ASSERT(rules == NULL);
    ASSERT_EQ(0u, (unsigned)n);
    char *large = malloc(MT_SUB_MAX_LINE_BYTES + 32);
    ASSERT(large != NULL);
    strcpy(large, "DOMAIN-KEYWORD,");
    memset(large + 15, 'x', MT_SUB_MAX_LINE_BYTES);
    large[15 + MT_SUB_MAX_LINE_BYTES] = '\0';
    ASSERT_EQ(MT_ERR_LIMIT, mt_sub_parse_rules(large, &rules, &n));
    ASSERT(rules == NULL);
    ASSERT_EQ(0u, (unsigned)n);
    free(large);
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
    ASSERT_STR_EQ("keyword", sub->rules[1]->type);
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
    ASSERT_STR_EQ("*.example.com", group->rules[2]->rule);
    ASSERT_STR_EQ("domain-wildcard", group->rules[2]->type);
    ASSERT_STR_EQ("192.0.2.0/24", group->rules[3]->rule);
    ASSERT_STR_EQ("DOMAIN-KEYWORD,openai", sub->rules[1]->rule);
    mt_group_free(group);
    mt_subscription_free(sub);
    PASS();
}

TEST typed_refresh_stable_ids_and_failure(void)
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
    ASSERT_STR_EQ("keyword", fresh[0]->type);
    mt_sub_rules_free(fresh, n_fresh);
    fresh = NULL;
    ASSERT_EQ(MT_ERR_INVAL, mt_sub_refresh_rules("DOMAIN-KEYWORD,openai\nGEOIP,CN",
                                                 old, n_old, &fresh, &n_fresh));
    ASSERT(fresh == NULL);
    ASSERT_EQ(0u, (unsigned)n_fresh);
    ASSERT_FALSE(old[0]->enable);
    ASSERT_STR_EQ("DOMAIN-KEYWORD,openai", old[0]->rule);
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
    RUN_TEST(native_keyword);
    RUN_TEST(native_wildcard);
    RUN_TEST(indexed_native_rules);
    RUN_TEST(typed_import_canonical_identity);
    RUN_TEST(typed_import_rejects_atomically);
    RUN_TEST(typed_cache_restore_and_runtime);
    RUN_TEST(typed_refresh_stable_ids_and_failure);
    RUN_TEST(legacy_list_unchanged);
    GREATEST_MAIN_END();
}
