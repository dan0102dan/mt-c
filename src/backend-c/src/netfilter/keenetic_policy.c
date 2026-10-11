#include "magitrickle/keenetic_policy.h"
#include <stdio.h>
#include <limits.h>
#include <string.h>

bool mt_kn_policy_config_valid(const mt_kn_policy_config_t *c)
{
    if (!c || (c->mode != MT_KN_POLICY_INCLUDE && c->mode != MT_KN_POLICY_EXCLUDE)) { return false; }
    size_t n = strnlen(c->policy_id, sizeof(c->policy_id));
    if (n == sizeof(c->policy_id) || (c->enabled && !n)) { return false; }
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)c->policy_id[i] < 32u || c->policy_id[i] == 127) { return false; }
    }
    return true;
}
bool mt_kn_policy_config_equal(const mt_kn_policy_config_t *a, const mt_kn_policy_config_t *b)
{
    return a->enabled == b->enabled && a->mode == b->mode && strcmp(a->policy_id, b->policy_id) == 0;
}
mt_err_t mt_kn_policy_scope_init(mt_kn_policy_scope_t *p, const mt_kn_policy_config_t *c, const char *prefix)
{
    if (!p || !prefix || !mt_kn_policy_config_valid(c)) { return MT_ERR_INVAL; }
    memset(p, 0, sizeof(*p));
    p->config = *c;
    int a = snprintf(p->gate, sizeof(p->gate), "%sPOLICY", prefix);
    int b = snprintf(p->dispatch, sizeof(p->dispatch), "%sPOLICY_RULES", prefix);
    /* Kernel user-chain names are limited to 28 characters. Disabled legacy
     * configurations need not satisfy a constraint for unused chains. */
    return c->enabled && (a < 0 || b < 0 || a > 28 || b > 28) ? MT_ERR_INVAL : MT_OK;
}
mt_err_t mt_kn_policy_guard(const mt_kn_policy_scope_t *p, mt_ipt_t *ipt,
                            const char *table, const char *chain)
{
    if (!ipt || !p || !p->config.enabled) { return MT_OK; }
    if (!p->ready) {
        const char *args[] = {"-j", "RETURN"};
        return mt_ipt_append(ipt, table, chain, args, 2);
    }
    char mark[32];
    snprintf(mark, sizeof(mark), "0x%x/0x0fffffff", p->mark);
    if (p->config.mode == MT_KN_POLICY_INCLUDE) {
        const char *args[] = {"-m", "mark", "!", "--mark", mark, "-j", "RETURN"};
        return mt_ipt_append(ipt, table, chain, args, 7);
    }
    const char *args[] = {"-m", "mark", "--mark", mark, "-j", "RETURN"};
    return mt_ipt_append(ipt, table, chain, args, 6);
}
mt_err_t mt_kn_policy_route_guard(const mt_kn_policy_scope_t *p, mt_ipt_t *ipt,
                                  const char *table, const char *chain, uint32_t route_mark)
{
    if (!ipt || !p || !p->config.enabled) { return MT_OK; }
    if (!p->ready) { return mt_kn_policy_guard(p, ipt, table, chain); }
    char mark[32];
    snprintf(mark, sizeof(mark), "0x%x/0x0fffffff", route_mark & MT_KN_POLICY_MASK);
    const char *args[] = {"-m", "connmark", "!", "--mark", mark, "-j", "RETURN"};
    return mt_ipt_append(ipt, table, chain, args, 7);
}
mt_err_t mt_kn_policy_prepare(const mt_kn_policy_scope_t *p, mt_ipt_t *ipt)
{
    if (!ipt || !p || !p->config.enabled) { return MT_OK; }
    mt_err_t err = mt_ipt_register_chain_patch(ipt, "mangle", p->dispatch);
    if (err != MT_OK) { return err; }
    /* A patch chain with no commands is not created by the engine. Keep an
     * ordered terminal entry even with zero enabled groups. INT_MAX also makes
     * later group additions sort before this entry, unlike an ordinary RETURN. */
    const char *terminal[] = {"-j", "RETURN"};
    err = mt_ipt_append_ordered(ipt, "mangle", p->dispatch, INT_MAX, terminal, 2);
    if (err != MT_OK) { return err; }
    err = mt_ipt_register_chain_override(ipt, "mangle", p->gate);
    if (err != MT_OK) { return err; }
    const char *reply[] = {"-m", "conntrack", "--ctdir", "REPLY", "-j", "RETURN"};
    err = mt_ipt_append(ipt, "mangle", p->gate, reply, 6);
    if (err != MT_OK) { return err; }
    /* Router-local DNS must retain its Keenetic mark until nat/PREROUTING,
     * where the same membership guard precedes MT's DNS redirection. */
    const char *local[] = {"-m", "addrtype", "--dst-type", "LOCAL", "-j", "RETURN"};
    err = mt_ipt_append(ipt, "mangle", p->gate, local, 6);
    if (err != MT_OK) { return err; }
    err = mt_kn_policy_guard(p, ipt, "mangle", p->gate);
    if (err != MT_OK) { return err; }
    const char *dispatch[] = {"-j", p->dispatch};
    err = mt_ipt_append(ipt, "mangle", p->gate, dispatch, 2);
    if (err != MT_OK) { return err; }
    const char *jump[] = {"-j", p->gate};
    return mt_ipt_append(ipt, "mangle", "PREROUTING", jump, 2);
}
