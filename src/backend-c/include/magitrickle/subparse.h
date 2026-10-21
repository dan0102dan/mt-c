/* Subscription list parsing/refresh — port of src/backend/subscriptions
 * (parse.go, validate.go, auto_update.go).
 *
 * Behaviour contract (legacy plain lists):
 * - tokenized on '\n', '\r' and ',', trimmed, empty and '#'-prefixed
 *   entries skipped;
 * - duplicates removed by (detected type + '|' + text);
 * - type auto-detection order: subnet6, subnet, namespace, domain, regex,
 *   wildcard, "" (note: namespace and domain use the same validator, so
 *   plain domains always detect as "namespace" — Go quirk kept);
 * - refresh keeps ID/Enable/Type of entries whose exact text survived,
 *   assigns fresh unique random IDs to the rest;
 * - is_due mirrors subscriptions.IsDue.
 *
 * Typed text rule-sets (Meta/Surge and HOST aliases) are parsed by physical
 * line; canonical TYPE,value records are stable identities. This preserves
 * source types through cache reload and distinguishes equal payloads of
 * different types. Unsupported/invalid records reject the whole refresh.
 * Only explicit DOMAIN-REGEX records use regex validation in typed lists.
 */
#ifndef MAGITRICKLE_SUBPARSE_H
#define MAGITRICKLE_SUBPARSE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#include "magitrickle/err.h"
#include "magitrickle/models.h"

/* Bound expansion of the existing 8 MiB fetch body on memory-limited routers. */
#define MT_SUB_MAX_RULES ((size_t)100000)
#define MT_SUB_MAX_LINE_BYTES ((size_t)4096)

/* Worker-safe: cancel is optional; failure always returns NULL/0 output. */
mt_err_t mt_sub_parse_rules_cancel(const char *list, mt_sub_rule_t ***out_rules,
                                   size_t *out_n, const atomic_bool *cancel);
void mt_sub_rules_free(mt_sub_rule_t **rules, size_t n);
/* Modifies only the caller-owned parsed array, never existing live rules. */
mt_err_t mt_sub_reconcile_rules(mt_sub_rule_t **parsed, size_t n,
                                 mt_sub_rule_t **existing, size_t n_existing);
/* Modern subscriptions keep user settings separately. Preserve only IDs so
 * removed overrides cannot be resurrected from the previous effective cache. */
mt_err_t mt_sub_reconcile_rule_ids(mt_sub_rule_t **parsed, size_t n,
                                    mt_sub_rule_t **existing, size_t n_existing);
mt_err_t mt_sub_rules_fix_ids(mt_sub_rule_t **rules, size_t n);

/* Detected type for a single trimmed pattern; returns one of the
 * MT_RULE_* constants or "" (never NULL). */
const char *mt_sub_detect_type(const char *pattern);

/* Runtime payload of a canonical typed record, or the unchanged legacy
 * pattern. Borrows from `rule`, independent of user-overridden effective type.
 * NULL stays NULL. Do not use the payload as a subscription identity. */
const char *mt_sub_rule_pattern(const char *rule);

/* Parses a raw list into an array of mt_sub_rule_t (caller owns).
 * Every parsed rule: random unique ID, Enable=true. */
mt_err_t mt_sub_parse_rules(const char *list, mt_sub_rule_t ***out_rules,
                            size_t *out_n);

/* RefreshRules port: parse `list`, then preserve ID/Enable/Type from
 * `existing` for rules with identical text. Returns a fresh array. */
mt_err_t mt_sub_refresh_rules(const char *list, mt_sub_rule_t **existing,
                              size_t n_existing, mt_sub_rule_t ***out_rules,
                              size_t *out_n);

/* sameRules port (order-insensitive multiset compare incl. ID/Enable). */
bool mt_sub_same_rules(mt_sub_rule_t **left, size_t n_left,
                       mt_sub_rule_t **right, size_t n_right);

/* IsDue port. */
bool mt_sub_is_due(const mt_subscription_t *sub, int64_t now_unix);

#endif /* MAGITRICKLE_SUBPARSE_H */
