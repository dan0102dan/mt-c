/* Keenetic-only traffic scope. No public surface in generic builds. */
#ifndef MAGITRICKLE_KEENETIC_POLICY_H
#define MAGITRICKLE_KEENETIC_POLICY_H
#ifdef MT_ENTWARE_KN
#include <stdbool.h>
#include <stdint.h>
#include "magitrickle/iptables.h"

#define MT_KN_POLICY_ID_SIZE 128
#define MT_KN_POLICY_MASK UINT32_C(0x0fffffff)
typedef enum { MT_KN_POLICY_INCLUDE, MT_KN_POLICY_EXCLUDE } mt_kn_policy_mode_t;
/* Facts from the last validated RCI inventory, independent of processing mode.
 * Zero-initialized/startup state never implies that a policy is absent. */
typedef enum {
    MT_KN_POLICY_UNKNOWN,
    MT_KN_POLICY_ABSENT,
    MT_KN_POLICY_UNMARKED,
    MT_KN_POLICY_RESOLVED,
} mt_kn_policy_resolution_t;
typedef enum {
    MT_KN_TRAFFIC_NONE,
    MT_KN_TRAFFIC_ALL,
    MT_KN_TRAFFIC_INCLUDE,
    MT_KN_TRAFFIC_EXCLUDE,
} mt_kn_policy_traffic_t;
typedef struct {
    bool enabled;
    mt_kn_policy_mode_t mode;
    char policy_id[MT_KN_POLICY_ID_SIZE];
} mt_kn_policy_config_t;
/* Borrowed by rulesets and DNS remap; mutations are serialized by app's nf_mu.
 * A resolved mark may be zero; resolution, not mark, determines availability. */
typedef struct {
    mt_kn_policy_config_t config;
    mt_kn_policy_resolution_t resolution;
    uint32_t mark;
    char gate[64], dispatch[64];
} mt_kn_policy_scope_t;

/* Pure policy decision shared by routing, DNS and downstream guards.
 * Transport failures are handled before publication, never as ABSENT here. */
mt_kn_policy_traffic_t mt_kn_policy_traffic_scope(const mt_kn_policy_scope_t *scope);
bool mt_kn_policy_config_valid(const mt_kn_policy_config_t *config);
bool mt_kn_policy_config_equal(const mt_kn_policy_config_t *a,
                               const mt_kn_policy_config_t *b);
mt_err_t mt_kn_policy_scope_init(mt_kn_policy_scope_t *scope,
                                 const mt_kn_policy_config_t *config, const char *prefix);
/* Called once per fresh engine, before any group is staged. The dispatch is
 * a patch chain so append_ordered retains group/subscription precedence. */
mt_err_t mt_kn_policy_prepare(const mt_kn_policy_scope_t *scope, mt_ipt_t *ipt);
/* Read-only membership guard, before any MARK/CONNMARK mutation. */
mt_err_t mt_kn_policy_guard(const mt_kn_policy_scope_t *scope, mt_ipt_t *ipt,
                            const char *table, const char *chain);
/* Downstream filter/NAT must not act merely because a shared destination
 * ipset matches. Only a connection actually routed by this group qualifies. */
mt_err_t mt_kn_policy_route_guard(const mt_kn_policy_scope_t *scope, mt_ipt_t *ipt,
                                  const char *table, const char *chain, uint32_t route_mark);
#endif
#endif
