/* Local CPU microbenchmark; no network, router or wall-clock CI threshold.
 * Build from src/backend-c: make bench_subscription_overrides
 * Run: src/backend-c/build/host/bench-subscription-overrides 50000
 *
 * Construct the same cache/exception arrays as the PR #19 review benchmark.
 * Setup is outside the timers. Application includes building an index for a
 * directly constructed model; copying measures indexed insertion as well.
 * Keep these measured operations unchanged when comparing revisions. */
#include "magitrickle/models.h"
#include "magitrickle/subparse.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double seconds(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    unsigned long count = 50000;
    if (argc > 1) {
        char *end; errno = 0; count = strtoul(argv[1], &end, 10);
        if (errno || *end || !count || count > MT_SUB_MAX_RULES) {
            fprintf(stderr, "count must be 1..%zu\n", MT_SUB_MAX_RULES); return 2;
        }
    }
    size_t n = (size_t)count;
    mt_subscription_t *sub = mt_subscription_new(), *copy = mt_subscription_new();
    if (!sub || !copy) { mt_subscription_free(sub); mt_subscription_free(copy); return 2; }
    sub->rules = calloc(n, sizeof(*sub->rules));
    sub->overrides = calloc(n, sizeof(*sub->overrides));
    if (!sub->rules || !sub->overrides) { goto allocation_failed; }
    for (size_t i = 0; i < n; i++) {
        char pattern[64]; snprintf(pattern, sizeof(pattern), "site%05zu.example.org", i);
        mt_sub_rule_t *rule = mt_sub_rule_new();
        mt_sub_override_t *override = calloc(1, sizeof(*override));
        if (!rule || !override) { mt_sub_rule_free(rule); free(override); goto allocation_failed; }
        sub->rules[sub->n_rules++] = rule;
        sub->overrides[sub->n_overrides++] = override;
        if (mt_strset(&rule->rule, pattern) != MT_OK ||
            mt_strset(&rule->type, "namespace") != MT_OK ||
            mt_strset(&override->rule, pattern) != MT_OK) { goto allocation_failed; }
        rule->enable = true;
        override->enable = 0;
    }
    double start = seconds();
    mt_err_t err = mt_subscription_apply_overrides(sub, sub->rules, n);
    double apply = seconds() - start;
    start = seconds();
    mt_err_t copy_err = mt_subscription_overrides_copy(copy, sub);
    double copying = seconds() - start;
    printf("rules=%zu overrides=%zu apply_s=%.6f apply_err=%d copy_s=%.6f copy_err=%d\n",
           n, n, apply, (int)err, copying, (int)copy_err);
    bool valid = err == MT_OK && copy_err == MT_OK && copy->n_overrides == n;
    for (size_t i = 0; valid && i < n; i++) {
        valid = !sub->rules[i]->enable && copy->overrides[i]->enable == 0;
    }
    mt_subscription_free(copy); mt_subscription_free(sub);
    return valid ? 0 : 1;

allocation_failed:
    mt_subscription_free(copy); mt_subscription_free(sub);
    fprintf(stderr, "failed to allocate benchmark input\n");
    return 2;
}
