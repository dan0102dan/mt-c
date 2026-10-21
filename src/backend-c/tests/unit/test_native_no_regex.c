/* Imported keywords and globs run through the existing wildcard rule type,
 * without compiling or matching PCRE2. */
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <stdlib.h>
#include "greatest.h"
#include "magitrickle/match.h"
#include "magitrickle/subparse.h"

static unsigned compile_calls, match_calls;

pcre2_code *pcre2_compile(PCRE2_SPTR pattern, PCRE2_SIZE length, uint32_t options,
                           int *errorcode, PCRE2_SIZE *erroroffset,
                           pcre2_compile_context *context)
{
    (void)pattern; (void)length; (void)options; (void)context;
    compile_calls++;
    *errorcode = PCRE2_ERROR_INTERNAL;
    *erroroffset = 0;
    return NULL;
}

int pcre2_match(const pcre2_code *code, PCRE2_SPTR subject, PCRE2_SIZE length,
                PCRE2_SIZE offset, uint32_t options, pcre2_match_data *data,
                pcre2_match_context *context)
{
    (void)code; (void)subject; (void)length; (void)offset;
    (void)options; (void)data; (void)context;
    match_calls++;
    return PCRE2_ERROR_NOMATCH;
}

TEST imported_wildcards_never_enter_pcre2(void)
{
    mt_sub_rule_t **rules = NULL;
    size_t n = 0;
    ASSERT_EQ(MT_OK, mt_sub_parse_rules(
        "DOMAIN-KEYWORD,labs.google\nDOMAIN-WILDCARD,api?.example.com\n",
        &rules, &n));
    ASSERT_EQ(2u, (unsigned)n);
    mt_matcher_t *matcher = mt_matcher_new();
    ASSERT(matcher != NULL);
    for (size_t i = 0; i < n; i++) {
        ASSERT_STR_EQ("wildcard", rules[i]->type);
        char *pattern = mt_sub_rule_runtime_pattern(rules[i]->rule, rules[i]->type);
        ASSERT(pattern != NULL);
        ASSERT_EQ(MT_OK, mt_matcher_add(matcher, rules[i]->type, pattern));
        free(pattern);
    }
    ASSERT(mt_matcher_match(matcher, "X.LABS.GOOGLE.NET"));
    ASSERT(mt_matcher_match(matcher, "API1.EXAMPLE.COM"));
    ASSERT_FALSE(mt_matcher_match(matcher, "labsXgoogle.net"));
    ASSERT_EQ(0u, compile_calls);
    ASSERT_EQ(0u, match_calls);
    mt_matcher_free(matcher);
    mt_sub_rules_free(rules, n);
    PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(imported_wildcards_never_enter_pcre2);
    GREATEST_MAIN_END();
}
