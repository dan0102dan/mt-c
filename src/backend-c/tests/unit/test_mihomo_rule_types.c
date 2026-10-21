/* Independent inventory of every case in MetaCubeX/mihomo rules/parser.go,
 * audited 2026-10-21, blob 474d01ef42a6870fcc349e4bf8f8b5836406e1d4.
 * Do not derive this table from subparse.c: missing recognition must fail a test.
 * Recognition is not execution support; unsupported semantics fail atomically. */
#include "greatest.h"
#include "magitrickle/subparse.h"

#include <stdio.h>
#include <string.h>

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

typedef struct source_case {
    const char *name;
    const char *payload;
    const char *type;
} source_case_t;

static const source_case_t supported[] = {
    {"DOMAIN", "example.com", "domain"},
    {"DOMAIN-SUFFIX", "example.com", "namespace"},
    {"DOMAIN-KEYWORD", "example", "wildcard"},
    {"DOMAIN-REGEX", "^a[0-9]{1,3}\\.example\\.com$", "regex"},
    {"DOMAIN-WILDCARD", "*.example.com", "wildcard"},
    {"IP-CIDR", "192.0.2.0/24", "subnet"},
    {"IP-CIDR6", "2001:db8::/32", "subnet6"},
};

static const source_case_t unsupported[] = {
    {"GEOSITE", "youtube", NULL},
    {"GEOIP", "CN", NULL},
    {"SRC-GEOIP", "CN", NULL},
    {"IP-ASN", "13335", NULL},
    {"SRC-IP-ASN", "9808", NULL},
    {"SRC-IP-CIDR", "192.168.1.201/32", NULL},
    {"IP-SUFFIX", "8.8.8.8/24", NULL},
    {"SRC-IP-SUFFIX", "192.168.1.201/8", NULL},
    {"SRC-PORT", "7777", NULL},
    {"DST-PORT", "80", NULL},
    {"IN-PORT", "7890", NULL},
    {"DSCP", "4", NULL},
    {"PROCESS-NAME", "curl", NULL},
    {"PROCESS-PATH", "/usr/bin/wget", NULL},
    {"PROCESS-NAME-REGEX", "curl$", NULL},
    {"PROCESS-PATH-REGEX", ".*bin/wget", NULL},
    {"PROCESS-NAME-WILDCARD", "*telegram*", NULL},
    {"PROCESS-PATH-WILDCARD", "/usr/*/wget", NULL},
    {"NETWORK", "udp", NULL},
    {"UID", "1001", NULL},
    {"IN-TYPE", "SOCKS/HTTP", NULL},
    {"IN-USER", "mihomo", NULL},
    {"IN-NAME", "ss", NULL},
    {"REMATCH-NAME", "rematch1", NULL},
    {"SUB-RULE", "(NETWORK,tcp),sub-rule", NULL},
    {"AND", "((DOMAIN,example.com),(NETWORK,UDP))", NULL},
    {"OR", "((NETWORK,UDP),(DOMAIN,example.com))", NULL},
    {"NOT", "((DOMAIN,example.com))", NULL},
    {"RULE-SET", "providername", NULL},
    {"MATCH", "DIRECT", NULL},
};

_Static_assert(COUNT(supported) + COUNT(unsupported) == 37,
               "Update the upstream inventory and audit together");

static void variant_name(char *out, const char *name, unsigned variant)
{
    size_t i = 0;
    for (; name[i]; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z' && (variant == 1 || (variant == 2 && i % 2))) {
            c = (char)(c + ('a' - 'A'));
        }
        out[i] = c;
    }
    out[i] = '\0';
}

static bool rejects_atomically(const char *text)
{
    mt_sub_rule_t **rules = NULL;
    size_t n = 123;
    mt_err_t err = mt_sub_parse_rules(text, &rules, &n);
    bool ok = err == MT_ERR_INVAL && rules == NULL && n == 0;
    if (!ok) { fprintf(stderr, "expected atomic rejection: %s\n", text); }
    if (rules) { mt_sub_rules_free(rules, n); }
    return ok;
}

TEST mihomo_supported_inventory(void)
{
    for (size_t i = 0; i < COUNT(supported); i++) {
        for (unsigned variant = 0; variant < 3; variant++) {
            char name[64], text[512], canonical[512];
            variant_name(name, supported[i].name, variant);
            snprintf(text, sizeof(text), " \t%s \t, %s\r\n", name, supported[i].payload);
            snprintf(canonical, sizeof(canonical), "%s,%s", supported[i].name, supported[i].payload);
            mt_sub_rule_t **rules = NULL;
            size_t n = 0;
            ASSERT_EQ(MT_OK, mt_sub_parse_rules(text, &rules, &n));
            ASSERT_EQ(1u, (unsigned)n);
            ASSERT_STR_EQ(supported[i].type, rules[0]->type);
            ASSERT_STR_EQ(canonical, rules[0]->rule);
            mt_sub_rules_free(rules, n);
        }
    }
    PASS();
}

TEST mihomo_unsupported_inventory_standalone(void)
{
    for (size_t i = 0; i < COUNT(unsupported); i++) {
        for (unsigned variant = 0; variant < 3; variant++) {
            char name[64], text[512];
            variant_name(name, unsupported[i].name, variant);
            /* No supported record may help detection here: this is the bug. */
            snprintf(text, sizeof(text), "%s,%s", name, unsupported[i].payload);
            ASSERT(rejects_atomically(text));
            snprintf(text, sizeof(text), "\xef\xbb\xbf# source\r\n; comment\r\n// comment\r\n"
                     " \t%s \t, %s\r\n", name, unsupported[i].payload);
            ASSERT(rejects_atomically(text));
        }
    }
    PASS();
}

TEST mihomo_unsupported_refresh_keeps_previous_rules(void)
{
    mt_sub_rule_t **old = NULL;
    size_t n_old = 0;
    ASSERT_EQ(MT_OK, mt_sub_parse_rules("keep.example.com", &old, &n_old));
    old[0]->enable = false;
    ASSERT_EQ(MT_OK, mt_strset(&old[0]->type, MT_RULE_DOMAIN));
    mt_id_t id = old[0]->id;
    bool ok = true;
    for (size_t i = 0; i < COUNT(unsupported) && ok; i++) {
        for (unsigned position = 0; position < 3 && ok; position++) {
            char text[512];
            snprintf(text, sizeof(text), "%s%s,%s%s",
                     position == 0 ? "" : "DOMAIN,before.example.com\n",
                     unsupported[i].name, unsupported[i].payload,
                     position == 2 ? "" : "\nIP-CIDR,192.0.2.0/24\n");
            mt_sub_rule_t **fresh = NULL;
            size_t n_fresh = 123;
            mt_err_t err = mt_sub_refresh_rules(text, old, n_old, &fresh, &n_fresh);
            ok = err == MT_ERR_INVAL && fresh == NULL && n_fresh == 0 &&
                 n_old == 1 && mt_id_equal(id, old[0]->id) && !old[0]->enable &&
                 strcmp(old[0]->rule, "keep.example.com") == 0 &&
                 strcmp(old[0]->type, MT_RULE_DOMAIN) == 0;
            if (!ok) { fprintf(stderr, "refresh mutated state for %s\n", unsupported[i].name); }
            if (fresh) { mt_sub_rules_free(fresh, n_fresh); }
        }
    }
    mt_sub_rules_free(old, n_old);
    ASSERT(ok);
    PASS();
}

TEST mihomo_names_do_not_reserve_entire_prefixes(void)
{
    const char *names[] = {
        "MY-SITE", "IN-HOUSE", "IN-PORT-HOST", "SRC-IP-LOCAL",
        "PROCESS-NAME-HOST", "IP-SUFFIX-HOST", "MY-RULE-SET",
    };
    for (size_t i = 0; i < COUNT(names); i++) {
        char text[128];
        snprintf(text, sizeof(text), "%s,example.com", names[i]);
        mt_sub_rule_t **rules = NULL;
        size_t n = 0;
        ASSERT_EQ(MT_OK, mt_sub_parse_rules(text, &rules, &n));
        ASSERT_EQ(2u, (unsigned)n);
        ASSERT_STR_EQ(names[i], rules[0]->rule);
        ASSERT_STR_EQ(MT_RULE_NAMESPACE, rules[0]->type);
        ASSERT_STR_EQ("example.com", rules[1]->rule);
        mt_sub_rules_free(rules, n);
    }
    ASSERT(rejects_atomically("DOMAIN,example.com\nUNKNOWN-TYPE,8.8.8.8/24"));
    PASS();
}

TEST other_text_dialect_aliases_retained(void)
{
    mt_sub_rule_t **rules = NULL;
    size_t n = 0;
    ASSERT_EQ(MT_OK, mt_sub_parse_rules(
        "HOST,example.com\nHOST-SUFFIX,example.com\nHOST-KEYWORD,example\n"
        "IP6-CIDR,2001:db8::/32\n", &rules, &n));
    ASSERT_EQ(4u, (unsigned)n);
    const char *types[] = {MT_RULE_DOMAIN, MT_RULE_NAMESPACE, MT_RULE_WILDCARD, MT_RULE_SUBNET6};
    for (size_t i = 0; i < n; i++) { ASSERT_STR_EQ(types[i], rules[i]->type); }
    mt_sub_rules_free(rules, n);
    ASSERT(rejects_atomically("FINAL,DIRECT"));
    ASSERT(rejects_atomically("INCLUDE,example.list"));
    PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(mihomo_supported_inventory);
    RUN_TEST(mihomo_unsupported_inventory_standalone);
    RUN_TEST(mihomo_unsupported_refresh_keeps_previous_rules);
    RUN_TEST(mihomo_names_do_not_reserve_entire_prefixes);
    RUN_TEST(other_text_dialect_aliases_retained);
    GREATEST_MAIN_END();
}
