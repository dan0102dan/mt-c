/* Regression coverage for typed/legacy format detection and empty imports. */
#include "greatest.h"

#include <stdio.h>
#include <string.h>

#include "magitrickle/subparse.h"

TEST legacy_hyphenated_entries_keep_plain_list_parsing(void)
{
    const char *names[] = {"MY-SITE", "MY-SITE-2", "DOMAIN-NAME", "HOST-01", "My-Site", "my-site"};
    const char *prefixes[] = {"", " ", "# comment\r\n", "example.com,"};
    const char *suffixes[] = {",example.com", " , example.com \r\n", ",example.com\r\n", ""};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        for (size_t j = 0; j < sizeof(prefixes) / sizeof(prefixes[0]); j++) {
            char source[128];
            int len = snprintf(source, sizeof(source), "%s%s%s", prefixes[j], names[i], suffixes[j]);
            ASSERT(len > 0 && (size_t)len < sizeof(source));
            mt_sub_rule_t **rules = NULL;
            size_t n = 0;
            ASSERT_EQ(MT_OK, mt_sub_parse_rules(source, &rules, &n));
            ASSERT_EQ(2u, (unsigned)n);
            size_t name_index = j == 3 ? 1 : 0;
            ASSERT_STR_EQ(names[i], rules[name_index]->rule);
            ASSERT_STR_EQ("example.com", rules[1 - name_index]->rule);
            ASSERT_STR_EQ(MT_RULE_NAMESPACE, rules[0]->type);
            ASSERT_STR_EQ(MT_RULE_NAMESPACE, rules[1]->type);
            ASSERT(rules[0]->enable && rules[1]->enable);
            mt_sub_rules_free(rules, n);
        }
    }
    PASS();
}

TEST legacy_hyphenated_refresh_preserves_user_state(void)
{
    mt_sub_rule_t **old = NULL, **fresh = NULL;
    size_t n_old = 0, n_fresh = 0;
    ASSERT_EQ(MT_OK, mt_sub_parse_rules("MY-SITE\nexample.com\n", &old, &n_old));
    ASSERT_EQ(2u, (unsigned)n_old);
    old[0]->enable = false;
    ASSERT_EQ(MT_OK, mt_strset(&old[0]->type, MT_RULE_DOMAIN));
    ASSERT_EQ(MT_OK, mt_sub_refresh_rules(" MY-SITE , example.com \r\n",
                                          old, n_old, &fresh, &n_fresh));
    ASSERT_EQ(n_old, n_fresh);
    for (size_t i = 0; i < n_old; i++) {
        ASSERT(mt_id_equal(old[i]->id, fresh[i]->id));
        ASSERT_STR_EQ(old[i]->rule, fresh[i]->rule);
        ASSERT_STR_EQ(old[i]->type, fresh[i]->type);
        ASSERT_EQ(old[i]->enable, fresh[i]->enable);
    }
    ASSERT_FALSE(fresh[0]->enable);
    ASSERT_STR_EQ(MT_RULE_DOMAIN, fresh[0]->type);
    mt_sub_rules_free(fresh, n_fresh);
    mt_sub_rules_free(old, n_old);
    PASS();
}

TEST explicit_typed_names_are_skipped_whole(void)
{
    const char *unsupported[] = {
        "GEOIP", "GEOSITE", "AND", "OR", "NOT", "MATCH", "FINAL",
        "NETWORK", "UID", "DSCP", "INCLUDE", "SUB-RULE",
        "PROCESS-NAME", "PROCESS-PATH", "PROCESS-NAME-REGEX", "PROCESS-PATH-REGEX",
        "IP-ASN", "SRC-IP-CIDR", "SRC-IP-SUFFIX", "SRC-PORT", "DST-PORT", "RULE-SET",
        "process-name", "GeoIp",
    };
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); i++) {
        char source[128];
        int len = snprintf(source, sizeof(source), "%s,example.com\n", unsupported[i]);
        ASSERT(len > 0 && (size_t)len < sizeof(source));
        mt_sub_rule_t **rules = NULL;
        size_t n = 99;
        ASSERT_EQ(MT_OK, mt_sub_parse_rules(source, &rules, &n));
        ASSERT(rules == NULL);
        ASSERT_EQ(0u, (unsigned)n);
    }
    /* Unknown typed lines cannot leak their values into the legacy parser. */
    const char *mixed[] = {"DOMAIN,example.com\nMY-SITE,other.example\n",
                           "MY-SITE,other.example\nDOMAIN,example.com\n"};
    for (size_t i = 0; i < sizeof(mixed) / sizeof(mixed[0]); i++) {
        mt_sub_rule_t **rules = NULL;
        size_t n = 99;
        ASSERT_EQ(MT_OK, mt_sub_parse_rules(mixed[i], &rules, &n));
        ASSERT_EQ(1u, (unsigned)n);
        ASSERT_STR_EQ("DOMAIN,example.com", rules[0]->rule);
        mt_sub_rules_free(rules, n);
    }
    PASS();
}

TEST empty_wildcards_are_skipped_during_refresh(void)
{
    const char *empty[] = {
        "DOMAIN-WILDCARD,", "DOMAIN-WILDCARD,,",
        "DOMAIN-WILDCARD, \t\r\n", "domain-wildcard, ,proxy\n",
        "DOMAIN-WILDCARD,,proxy\n", "DOMAIN-WILDCARD,\nDOMAIN,example.com\n",
        "DOMAIN,example.com\nDOMAIN-WILDCARD,\n",
        "DOMAIN,example.com\nDOMAIN-WILDCARD,,\n",
        "DOMAIN,example.com\nDOMAIN-WILDCARD, \t,proxy\r\n",
    };
    mt_sub_rule_t **old = NULL;
    size_t n_old = 0;
    ASSERT_EQ(MT_OK, mt_sub_parse_rules("DOMAIN-WILDCARD,*.example.com\n", &old, &n_old));
    ASSERT_EQ(1u, (unsigned)n_old);
    old[0]->enable = false;
    ASSERT_EQ(MT_OK, mt_strset(&old[0]->type, MT_RULE_DOMAIN));
    mt_id_t old_id = old[0]->id;
    for (size_t i = 0; i < sizeof(empty) / sizeof(empty[0]); i++) {
        const size_t expected = strstr(empty[i], "DOMAIN,example.com") ? 1u : 0u;
        mt_sub_rule_t **parsed = NULL;
        size_t n = 99;
        ASSERT_EQ(MT_OK, mt_sub_parse_rules(empty[i], &parsed, &n));
        ASSERT_EQ(expected, n);
        if (n) { ASSERT_STR_EQ("DOMAIN,example.com", parsed[0]->rule); }
        else { ASSERT(parsed == NULL); }
        mt_sub_rules_free(parsed, n);
        parsed = NULL;
        n = 99;
        ASSERT_EQ(MT_OK, mt_sub_refresh_rules(empty[i], old, n_old, &parsed, &n));
        ASSERT_EQ(expected, n);
        if (n) { ASSERT_STR_EQ("DOMAIN,example.com", parsed[0]->rule); }
        else { ASSERT(parsed == NULL); }
        mt_sub_rules_free(parsed, n);
        ASSERT(mt_id_equal(old_id, old[0]->id));
        ASSERT_FALSE(old[0]->enable);
        ASSERT_STR_EQ(MT_RULE_DOMAIN, old[0]->type);
        ASSERT_STR_EQ("DOMAIN-WILDCARD,*.example.com", old[0]->rule);
    }
    mt_sub_rules_free(old, n_old);
    PASS();
}

TEST nonempty_wildcards_keep_existing_rule_type(void)
{
    const char *source = "DOMAIN-WILDCARD,*\nDOMAIN-WILDCARD,?\n"
                         "DOMAIN-WILDCARD,*.example.com\nDOMAIN-WILDCARD,**\n";
    mt_sub_rule_t **rules = NULL;
    size_t n = 0;
    ASSERT_EQ(MT_OK, mt_sub_parse_rules(source, &rules, &n));
    ASSERT_EQ(4u, (unsigned)n);
    for (size_t i = 0; i < n; i++) {
        ASSERT_STR_EQ(MT_RULE_WILDCARD, rules[i]->type);
        ASSERT(mt_sub_rule_pattern(rules[i]->rule)[0] != '\0');
    }
    mt_sub_rules_free(rules, n);
    PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(legacy_hyphenated_entries_keep_plain_list_parsing);
    RUN_TEST(legacy_hyphenated_refresh_preserves_user_state);
    RUN_TEST(explicit_typed_names_are_skipped_whole);
    RUN_TEST(empty_wildcards_are_skipped_during_refresh);
    RUN_TEST(nonempty_wildcards_keep_existing_rule_type);
    GREATEST_MAIN_END();
}
