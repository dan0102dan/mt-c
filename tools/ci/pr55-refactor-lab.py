#!/usr/bin/env python3
"""One-off, fail-closed source transformation; not part of the proposed PR."""
from pathlib import Path
import subprocess
import sys

HEAD = 'd57336f9d638cb51eee537277d37c65462bc19b8'
BASE = 'f782f76ee40ba21f703617e3689f119cb5e7a3ce'
root = Path(sys.argv[1]).resolve()
def git(*args):
    return subprocess.check_output(['git', '-C', str(root), *args], text=True)
assert git('rev-parse', 'HEAD').strip() == HEAD, 'Source head changed'
assert not git('status', '--porcelain').strip(), 'Working copy is not clean'
restore = [
 'src/backend-c/include/magitrickle/app.h',
 'src/backend-c/include/magitrickle/keenetic_policy.h',
 'src/backend-c/src/api/app.c',
 'src/backend-c/src/api/keenetic_policy_api.c',
 'src/backend-c/src/netfilter/keenetic_policy.c',
 'src/backend-c/tests/unit/test_keenetic_policy.c',
 'src/backend-c/tests/unit/test_keenetic_policy_api.c',
 'src/frontend/dev/keenetic-policy-mock.ts',
 'src/frontend/src/modules/settings/keenetic/PolicyPanel.svelte',
 'src/frontend/src/modules/settings/keenetic/ru.json',
 'src/frontend/src/modules/settings/keenetic/state.svelte.ts',
 'src/frontend/tests/e2e/keenetic-policy.spec.ts',
 'src/frontend/tests/unit/keenetic-policy-mock.test.ts',
 'docs/keenetic-policy.md', 'docs/c-rewrite/decisions.md',
]
for path in restore:
    (root / path).write_text(git('show', f'{BASE}:{path}'), encoding='utf-8')

def replace(path, before, after):
    p = root / path
    s = p.read_text()
    assert s.count(before) == 1, (path, s.count(before), before[:100])
    p.write_text(s.replace(before, after), encoding='utf-8')

h = 'src/backend-c/include/magitrickle/keenetic_policy.h'
replace(h, 'typedef enum { MT_KN_POLICY_INCLUDE, MT_KN_POLICY_EXCLUDE } mt_kn_policy_mode_t;', '''typedef enum { MT_KN_POLICY_INCLUDE, MT_KN_POLICY_EXCLUDE } mt_kn_policy_mode_t;
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
} mt_kn_policy_traffic_t;''')
replace(h, " * 'ready' is separate from mark: zero is not an absence sentinel. */", " * A resolved mark may be zero; resolution, not mark, determines availability. */")
replace(h, '    bool ready;\n', '    mt_kn_policy_resolution_t resolution;\n')
replace(h, 'bool mt_kn_policy_config_valid', '''/* Pure policy decision shared by routing, DNS and downstream guards.
 * Transport failures are handled before publication, never as ABSENT here. */
mt_kn_policy_traffic_t mt_kn_policy_traffic_scope(const mt_kn_policy_scope_t *scope);
bool mt_kn_policy_config_valid''')

nf = 'src/backend-c/src/netfilter/keenetic_policy.c'
replace(nf, 'bool mt_kn_policy_config_valid', '''mt_kn_policy_traffic_t mt_kn_policy_traffic_scope(const mt_kn_policy_scope_t *p)
{
    if (!p || !p->config.enabled) { return MT_KN_TRAFFIC_ALL; }
    switch (p->resolution) {
    case MT_KN_POLICY_RESOLVED:
        return p->config.mode == MT_KN_POLICY_EXCLUDE ? MT_KN_TRAFFIC_EXCLUDE : MT_KN_TRAFFIC_INCLUDE;
    case MT_KN_POLICY_ABSENT:
        return p->config.mode == MT_KN_POLICY_EXCLUDE ? MT_KN_TRAFFIC_ALL : MT_KN_TRAFFIC_NONE;
    default:
        return MT_KN_TRAFFIC_NONE;
    }
}

bool mt_kn_policy_config_valid''')
replace(nf, '''    if (!p->ready) {
        const char *args[] = {"-j", "RETURN"};''', '''    mt_kn_policy_traffic_t traffic = mt_kn_policy_traffic_scope(p);
    if (traffic == MT_KN_TRAFFIC_ALL) { return MT_OK; }
    if (traffic == MT_KN_TRAFFIC_NONE) {
        const char *args[] = {"-j", "RETURN"};''')
replace(nf, '    if (p->config.mode == MT_KN_POLICY_INCLUDE) {', '    if (traffic == MT_KN_TRAFFIC_INCLUDE) {')
replace(nf, '    if (!p->ready) { return mt_kn_policy_guard(p, ipt, table, chain); }', '''    if (mt_kn_policy_traffic_scope(p) == MT_KN_TRAFFIC_NONE) {
        return mt_kn_policy_guard(p, ipt, table, chain);
    }
    /* Even an unrestricted device scope must not NAT/accept a connection
     * that this group did not route (local/router-originated/other-policy). */''')
replace(nf, '    if (p->ready) {', '''    if (mt_kn_policy_traffic_scope(p) != MT_KN_TRAFFIC_NONE) {''')
replace(nf, '''    /* Until RCI resolves the policy, RETURN alone closes the scope. Do not
     * make startup depend on address enumeration for an inactive gate. */''', '''    /* Membership is independent of local/reply protection above. Unknown
     * scope stays closed and does not require address enumeration at startup. */''')

apph = 'src/backend-c/include/magitrickle/app.h'
replace(apph, '                        bool *ready, bool *applied, mt_err_t *apply_error);', '                        mt_kn_policy_resolution_t *resolution, bool *applied, mt_err_t *apply_error);')
replace(apph, '                            bool ready, uint32_t mark);', '                            mt_kn_policy_resolution_t resolution, uint32_t mark);')
replace(apph, '''/* Writes the netfilter tables whole: drops every chain and jump of ours
 * from the kernel first, then stages the port remap and every enabled
 * group and writes the result in one commit per family. Returns''', '''/* Prepares the complete replacement in memory, then reconciles owned
 * chains without a separate destructive cleanup. Applied status is invalid
 * during a pass and after any failure, even at the same generation. Returns''')

app = 'src/backend-c/src/api/app.c'
replace(app, 'app->cfg->app.keenetic_policy, false, 0)', 'app->cfg->app.keenetic_policy, MT_KN_POLICY_UNKNOWN, 0)')
replace(app, 'old_desired.config, old_desired.ready, old_desired.mark)', 'old_desired.config, old_desired.resolution, old_desired.mark)')
replace(app, '''    mt_err_t err = rebuild_netfilter_locked(app, cancel);
#ifdef MT_ENTWARE_KN''', '''#ifdef MT_ENTWARE_KN
    /* A firmware rewrite may have removed rules without changing settings.
     * A previous successful generation is not proof this pass is applied. */
    atomic_store(&app->policy_applied, 0);
    atomic_store(&app->policy_apply_error, MT_OK);
#endif
    mt_err_t err = rebuild_netfilter_locked(app, cancel);
#ifdef MT_ENTWARE_KN''')
replace(app, '''                        bool *ready, bool *applied, mt_err_t *apply_error)
{
    if (config) { *config = app->cfg->app.keenetic_policy; }
    if (ready) { *ready = app->policy_desired.ready; }
    if (applied) { *applied = atomic_load(&app->policy_applied) == atomic_load(&app->policy_generation); }
    if (apply_error) { *apply_error = (mt_err_t)atomic_load(&app->policy_apply_error); }
}''', '''                        mt_kn_policy_resolution_t *resolution, bool *applied, mt_err_t *apply_error)
{
    if (config) { *config = app->cfg->app.keenetic_policy; }
    if (resolution) { *resolution = app->policy_desired.resolution; }
    mt_err_t err = (mt_err_t)atomic_load(&app->policy_apply_error);
    if (applied) {
        *applied = err == MT_OK && atomic_load(&app->policy_applied) == atomic_load(&app->policy_generation);
    }
    if (apply_error) { *apply_error = err; }
}''')
replace(app, '''mt_err_t mt_app_policy_set(mt_app_t *app, const mt_kn_policy_config_t *config, bool ready, uint32_t mark)''', '''mt_err_t mt_app_policy_set(mt_app_t *app, const mt_kn_policy_config_t *config,
                            mt_kn_policy_resolution_t resolution, uint32_t mark)''')
replace(app, '''    if (err != MT_OK || (ready && mark > MT_KN_POLICY_MASK)) { return MT_ERR_INVAL; }
    desired.ready = config->enabled && ready;
    desired.mark = desired.ready ? mark : 0;''', '''    if (err != MT_OK || (unsigned)resolution > MT_KN_POLICY_RESOLVED ||
        (resolution == MT_KN_POLICY_RESOLVED && mark > MT_KN_POLICY_MASK)) { return MT_ERR_INVAL; }
    desired.resolution = config->enabled ? resolution : MT_KN_POLICY_UNKNOWN;
    desired.mark = desired.resolution == MT_KN_POLICY_RESOLVED ? mark : 0;''')
replace(app, 'app->policy_desired.ready != desired.ready', 'app->policy_desired.resolution != desired.resolution')

api = 'src/backend-c/src/api/keenetic_policy_api.c'
replace(api, '    bool ready, applied;\n', '    mt_kn_policy_resolution_t resolution;\n    bool applied;\n')
replace(api, 'mt_app_policy_get(s->app, &c, &ready, &applied, &apply_error);', 'mt_app_policy_get(s->app, &c, &resolution, &applied, &apply_error);')
replace(api, '!c.enabled ? "disabled" : ready ? "ready" : "unavailable";', '!c.enabled ? "disabled" : resolution == MT_KN_POLICY_RESOLVED ? "ready" : "unavailable";')
replace(api, '''s->last_error == MT_ERR_NOENT ? "POLICY_NOT_FOUND" :
            s->last_error == MT_ERR_AGAIN ? "POLICY_UNAVAILABLE" : "RCI_UNAVAILABLE"''', '''resolution == MT_KN_POLICY_ABSENT ? "POLICY_NOT_FOUND" :
            resolution == MT_KN_POLICY_UNMARKED ? "POLICY_UNAVAILABLE" : "RCI_UNAVAILABLE"''')
replace(api, 'static void request_fetch(mt_kn_policy_service_t *s)', '''/* Called only for a successfully fetched and validated inventory. */
static mt_kn_policy_resolution_t policy_resolution(const mt_kn_policy_entry_t *entry)
{
    return !entry ? MT_KN_POLICY_ABSENT :
        entry->available ? MT_KN_POLICY_RESOLVED : MT_KN_POLICY_UNMARKED;
}
static void request_fetch(mt_kn_policy_service_t *s)''')
replace(api, 'mt_app_policy_set(s->app, c, entry && entry->available, entry ? entry->mark : 0)', 'mt_app_policy_set(s->app, c, policy_resolution(entry), entry ? entry->mark : 0)')
replace(api, '    bool was_ready;\n    mt_app_policy_get(s->app, &current, &was_ready, NULL, NULL);', '    mt_app_policy_get(s->app, &current, NULL, NULL, NULL);')
replace(api, '''            /* A failed/invalid inventory says nothing about membership or
             * the selected mark. Keep the CURRENT desired scope exactly as
             * it is, including an unresolved scope on startup/config change.
             * Do not publish ready=false: that installs unconditional RETURN
             * in both the routing and DNS gates and bypasses MT for everybody.
             * No separate cache is kept, so a removed/changed/disabled policy
             * can never resurrect an older successful mark on a later error. */''', '''            /* Transport failure is not a policy fact. Retain the exact current
             * resolution and mark (including UNKNOWN on startup), without
             * reapplying rules or resurrecting a previous policy selection. */''')
replace(api, '''                    was_ready ? "keeping last known policy mark and traffic scope" :
                                "policy remains unresolved; MT processing stays closed",''', '''                    "keeping current policy resolution and traffic scope",''')
replace(api, '''            bool ready = entry && entry->available;
            mt_err_t next_error = !entry ? MT_ERR_NOENT : !ready ? MT_ERR_AGAIN : MT_OK;''', '''            mt_kn_policy_resolution_t resolution = policy_resolution(entry);
            mt_err_t next_error = resolution == MT_KN_POLICY_ABSENT ? MT_ERR_NOENT :
                resolution == MT_KN_POLICY_UNMARKED ? MT_ERR_AGAIN : MT_OK;''')
replace(api, '''"new traffic will bypass MT processing",
                        current.policy_id,''', '''"retaining the configured processing mode",
                        current.policy_id,''')
replace(api, 's->last_error != MT_OK && ready', 's->last_error != MT_OK && resolution == MT_KN_POLICY_RESOLVED')
replace(api, '(void)mt_app_policy_set(s->app, &current, ready, ready ? entry->mark : 0);', '(void)mt_app_policy_set(s->app, &current, resolution, entry ? entry->mark : 0);')

for name in ['test_nfrebuild.c', 'test_nfrebuild_recovery.c', 'test_keenetic_policy_addresses.c']:
    path = root / 'src/backend-c/tests/unit' / name
    text = path.read_text()
    import re
    text, n = re.subn(r'(mt_app_policy_set\([^\n]+, )(true|false)(, )',
        lambda m: m[1] + ('MT_KN_POLICY_RESOLVED' if m[2] == 'true' else 'MT_KN_POLICY_UNKNOWN') + m[3], text)
    assert n > 0, name
    if name == 'test_keenetic_policy_addresses.c':
        assert text.count('scope.ready = true;') == 1
        text = text.replace('scope.ready = true;', 'scope.resolution = MT_KN_POLICY_RESOLVED;')
    path.write_text(text)

testp = 'src/backend-c/tests/unit/test_keenetic_policy.c'
replace(testp, 'TEST dns_guards_and_ordered_dispatch_compile_for_both_families(void)', '''TEST traffic_scope_is_derived_from_policy_facts_and_mode(void)
{
    const struct {
        mt_kn_policy_resolution_t resolution;
        mt_kn_policy_traffic_t include, exclude;
    } cases[] = {
        {MT_KN_POLICY_UNKNOWN, MT_KN_TRAFFIC_NONE, MT_KN_TRAFFIC_NONE},
        {MT_KN_POLICY_ABSENT, MT_KN_TRAFFIC_NONE, MT_KN_TRAFFIC_ALL},
        {MT_KN_POLICY_UNMARKED, MT_KN_TRAFFIC_NONE, MT_KN_TRAFFIC_NONE},
        {MT_KN_POLICY_RESOLVED, MT_KN_TRAFFIC_INCLUDE, MT_KN_TRAFFIC_EXCLUDE},
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(*cases); i++) {
        mt_kn_policy_scope_t scope = {.config = {.enabled = true}, .resolution = cases[i].resolution};
        ASSERT_EQ(cases[i].include, mt_kn_policy_traffic_scope(&scope));
        scope.config.mode = MT_KN_POLICY_EXCLUDE;
        ASSERT_EQ(cases[i].exclude, mt_kn_policy_traffic_scope(&scope));
        scope.config.enabled = false;
        ASSERT_EQ(MT_KN_TRAFFIC_ALL, mt_kn_policy_traffic_scope(&scope));
    }
    PASS();
}
TEST dns_guards_and_ordered_dispatch_compile_for_both_families(void)''')
replace(testp, 'for (unsigned mode = 0; mode < 3; mode++)', 'for (unsigned mode = 0; mode < 4; mode++)')
replace(testp, 'config.mode = mode == 1 ?', 'config.mode = mode == 1 || mode == 3 ?')
replace(testp, 'scope.ready = mode != 2; scope.mark = 0xffffd00;', '''scope.resolution = mode < 2 ? MT_KN_POLICY_RESOLVED :
                mode == 2 ? MT_KN_POLICY_UNKNOWN : MT_KN_POLICY_ABSENT;
            scope.mark = 0xffffd00;''')
replace(testp, '''            ASSERT_EQ(3u, count);
            ASSERT_STR_EQ("RETURN", rules[0]->parts[rules[0]->n_parts - 1]);
            ASSERT_EQ(mode == 2 ? 2u : mode == 1 ? 6u : 7u, rules[0]->n_parts);
            if (mode != 2) { ASSERT_STR_EQ("0xffffd00/0x0fffffff", rules[0]->parts[mode == 1 ? 3 : 4]); }''', '''            ASSERT_EQ(mode == 3 ? 2u : 3u, count);
            if (mode == 3) {
                ASSERT_STR_EQ("-p", rules[0]->parts[0]); /* DNAT, no membership guard. */
            } else {
                ASSERT_STR_EQ("RETURN", rules[0]->parts[rules[0]->n_parts - 1]);
                ASSERT_EQ(mode == 2 ? 2u : mode == 1 ? 6u : 7u, rules[0]->n_parts);
                if (mode != 2) { ASSERT_STR_EQ("0xffffd00/0x0fffffff", rules[0]->parts[mode == 1 ? 3 : 4]); }
            }
            /* Downstream rules always require the route's connmark, even
             * with an empty exclusion set; destination ipset alone is insufficient. */
            ASSERT_EQ(MT_OK, mt_ipt_register_chain_override(ipt, "filter", "MT_TEST_ROUTE"));
            ASSERT_EQ(MT_OK, mt_kn_policy_route_guard(&scope, ipt, "filter", "MT_TEST_ROUTE", 101));
            ASSERT_EQ(MT_OK, mt_ipt_commit(ipt));
            ASSERT(mt_fake_ipt_get_rules(f, "filter", "MT_TEST_ROUTE", &rules, &count));
            ASSERT_EQ(1u, count);
            if (mode == 2) { ASSERT_EQ(2u, rules[0]->n_parts); }
            else {
                ASSERT_EQ(7u, rules[0]->n_parts);
                ASSERT_STR_EQ("connmark", rules[0]->parts[1]);
                ASSERT_STR_EQ("0x65/0x0fffffff", rules[0]->parts[4]);
            }''')
replace(testp, '    RUN_TEST(dns_guards_and_ordered_dispatch_compile_for_both_families);', '''    RUN_TEST(traffic_scope_is_derived_from_policy_facts_and_mode);
    RUN_TEST(dns_guards_and_ordered_dispatch_compile_for_both_families);''')

priority = 'src/backend-c/tests/unit/test_rule_priority.c'
replace(priority, 'for (unsigned mode = 0; mode < 3; mode++)', 'for (unsigned mode = 0; mode < 4; mode++)')
replace(priority, 'scope.config.mode = mode == 1 ?', 'scope.config.mode = mode == 1 || mode == 3 ?')
replace(priority, '        scope.ready = mode != 2;', '''        scope.resolution = mode < 2 ? MT_KN_POLICY_RESOLVED :
            mode == 2 ? MT_KN_POLICY_UNKNOWN : MT_KN_POLICY_ABSENT;''')
replace(priority, 'bool applies = mode != 2 && (mode == 1 ? !member : member);', 'bool applies = mode == 3 || (mode != 2 && (mode == 1 ? !member : member));')

testapi = 'src/backend-c/tests/unit/test_keenetic_policy_api.c'
replace(testapi, 'TEST both_modes_persist_and_missing_policy_never_falls_back_to_all(void)', 'TEST both_modes_persist_and_missing_include_stays_closed(void)')
replace(testapi, 'RUN_TEST(both_modes_persist_and_missing_policy_never_falls_back_to_all);', 'RUN_TEST(both_modes_persist_and_missing_include_stays_closed);')
replace(testapi, 'static int assert_closed_gates(void)', '''static int assert_open_gates(void) {
    cJSON *out = NULL;
    ASSERT_EQ(200, do_request("GET", "/test/netfilter", NULL, &out)); ASSERT(out);
    for (unsigned family = 0; family < 2; family++) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(out, family ? "ipv6" : "ipv4");
        ASSERT(cJSON_IsString(value));
        ASSERT(strstr(value->valuestring, "-A MT_POLICY -j MT_POLICY_RULES"));
        ASSERT(strstr(value->valuestring, "-A MT_POLICY -m conntrack --ctdir REPLY -j RETURN"));
        ASSERT(strstr(value->valuestring, family ? "-d ::1/128 -j RETURN" : "-d 127.0.0.1/32 -j RETURN"));
        ASSERT(strstr(value->valuestring, "-A MT_DNSOR -p tcp"));
        ASSERT(strstr(value->valuestring, "-A MT_DNSOR -p udp"));
        ASSERT_FALSE(strstr(value->valuestring, "-A MT_POLICY -j RETURN"));
        ASSERT_FALSE(strstr(value->valuestring, "-A MT_DNSOR -j RETURN"));
    }
    cJSON_Delete(out); PASS();
}
static int assert_closed_gates(void)''')
replace(testapi, '            cJSON_Delete(out); CHECK_CALL(assert_closed_gates());', '''            cJSON_Delete(out);
            if (mode == 1 && missing[i] == 2) { CHECK_CALL(assert_open_gates()); }
            else { CHECK_CALL(assert_closed_gates()); }''')
regression = r'''TEST absent_exclude_preserves_facts_across_errors_and_policy_transitions(void) {
    harness_t *h = harness_start_seeded(false,
        "configVersion: 0.8\napp: {keeneticPolicy: {enabled: true, policyId: mt, mode: exclude}}\n", 1);
    ASSERT(h);
    ASSERT_EQ(502, do_request("GET", "/api/v1/keenetic/policies", NULL, NULL));
    CHECK_CALL(assert_closed_gates());
    atomic_store(&fetch_mode, 2);
    ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policies", NULL, NULL));
    CHECK_CALL(assert_open_gates());
    char *before = snapshot_text(); ASSERT(before);
    const int inventories[] = {2, 1, 5, 6, 7, 2};
    for (size_t i = 0; i < sizeof(inventories)/sizeof(*inventories); i++) {
        atomic_store(&fetch_mode, inventories[i]);
        ASSERT_EQ(inventories[i] == 2 ? 200 : 502,
            do_request("GET", "/api/v1/keenetic/policies", NULL, NULL));
        cJSON *out = NULL;
        ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policy", NULL, &out));
        ASSERT_STR_EQ("unavailable", state(out));
        const cJSON *runtime = cJSON_GetObjectItemCaseSensitive(out, "runtime");
        ASSERT_STR_EQ("POLICY_NOT_FOUND", cJSON_GetObjectItemCaseSensitive(runtime, "reason")->valuestring);
        ASSERT_FALSE(cJSON_GetObjectItemCaseSensitive(runtime, "processing_all"));
        cJSON_Delete(out);
        CHECK_CALL(assert_snapshot_unchanged(before));
    }
    free(before);
    atomic_store(&fetch_mode, 3);
    ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policies", NULL, NULL));
    CHECK_CALL(assert_active_mark("0xffffd00/0x0fffffff", true));
    atomic_store(&fetch_mode, 2);
    ASSERT_EQ(200, do_request("GET", "/api/v1/keenetic/policies", NULL, NULL));
    CHECK_CALL(assert_open_gates());
    ASSERT_EQ(200, do_request("PUT", "/api/v1/keenetic/policy",
        "{\"enabled\":true,\"policy_id\":\"mt\",\"mode\":\"include\"}", NULL));
    CHECK_CALL(assert_closed_gates());
    atomic_store(&fetch_mode, 0);
    ASSERT_EQ(200, do_request("PUT", "/api/v1/keenetic/policy",
        "{\"enabled\":true,\"policy_id\":\"Waiting\",\"mode\":\"exclude\"}", NULL));
    CHECK_CALL(assert_closed_gates());
    harness_stop(h); PASS();
}
'''
replace(testapi, 'TEST validated_absence_or_mark_loss_invalidates_scope_and_errors_cannot_resurrect_it(void)', regression + 'TEST validated_absence_or_mark_loss_invalidates_scope_and_errors_cannot_resurrect_it(void)')
replace(testapi, '    RUN_TEST(failed_refresh_preserves_both_families_dns_and_exact_mark_without_writes);', '''    RUN_TEST(failed_refresh_preserves_both_families_dns_and_exact_mark_without_writes);
    RUN_TEST(absent_exclude_preserves_facts_across_errors_and_policy_transitions);''')

recovery = 'src/backend-c/tests/unit/test_nfrebuild_recovery.c'
regression = '''#ifdef MT_ENTWARE_KN
TEST same_generation_repair_failure_invalidates_applied_status(void) {
    setup(3u, "MT_");
    mt_kn_policy_config_t config = {.enabled = true, .policy_id = "mt", .mode = MT_KN_POLICY_EXCLUDE};
    ASSERT_EQ(MT_OK, mt_app_policy_set(fx.app, &config, MT_KN_POLICY_ABSENT, 0));
    bool applied;
    mt_err_t error;
    mt_kn_policy_resolution_t resolution;
    mt_app_policy_get(fx.app, NULL, &resolution, &applied, &error);
    ASSERT(applied); ASSERT_EQ(MT_OK, error); ASSERT_EQ(MT_KN_POLICY_ABSENT, resolution);
    fx.g[0]->fail_save = fx.g[0]->saves + 1;
    fx.g[0]->failure = MT_ERR_IO;
    ASSERT_EQ(MT_ERR_IO, mt_app_rebuild_netfilter(fx.app, NULL));
    mt_app_policy_get(fx.app, NULL, &resolution, &applied, &error);
    ASSERT_FALSE(applied); ASSERT_EQ(MT_ERR_IO, error); ASSERT_EQ(MT_KN_POLICY_ABSENT, resolution);
    ASSERT_EQ(MT_OK, mt_app_policy_set(fx.app, &config, MT_KN_POLICY_ABSENT, 0));
    mt_app_policy_get(fx.app, NULL, NULL, &applied, &error);
    ASSERT(applied); ASSERT_EQ(MT_OK, error);
    mt_cancel_t *cancel = mt_cancel_new(); ASSERT(cancel); mt_cancel_raise(cancel);
    ASSERT_EQ(MT_ERR_CANCELED, mt_app_rebuild_netfilter(fx.app, cancel));
    mt_app_policy_get(fx.app, NULL, NULL, &applied, &error);
    ASSERT_FALSE(applied); ASSERT_EQ(MT_ERR_CANCELED, error);
    mt_cancel_free(cancel);
    ASSERT_EQ(MT_OK, mt_app_rebuild_netfilter(fx.app, NULL));
    mt_app_policy_get(fx.app, NULL, NULL, &applied, &error);
    ASSERT(applied); ASSERT_EQ(MT_OK, error);
    PASS();
}
#endif
'''
replace(recovery, 'GREATEST_MAIN_DEFS();', regression + '\nGREATEST_MAIN_DEFS();')
replace(recovery, '    SET_TEARDOWN(teardown, NULL);', '''    SET_TEARDOWN(teardown, NULL);
#ifdef MT_ENTWARE_KN
    RUN_TEST(same_generation_repair_failure_invalidates_applied_status);
#endif''')

doc = 'docs/keenetic-policy.md'
replace(doc, '''До появления политики с пригодной меткой runtime остаётся `unavailable`;
область MT закрыта для нового трафика и не расширяется на всех клиентов.''', '''До появления политики с пригодной меткой runtime остаётся `unavailable`.
Если валидный ответ RCI подтверждает отсутствие политики, `exclude` не
исключает никого и MT обрабатывает все устройства; `include` не обрабатывает
никого. Существующая политика без метки и отсутствие достоверного ответа RCI
не означают пустую политику: в обоих режимах область MT остаётся закрытой.''')
replace(doc, 'настройкой область MT закрыта до первого успешного разрешения метки.', 'настройкой область MT закрыта до первого успешного ответа RCI, разрешающего\nметку либо подтверждающего отсутствие политики для режима `exclude`.')
replace(doc, '''меткой переводит её в `unavailable`. Последующий сбой RCI не возвращает
старую метку.''', '''меткой переводит её в `unavailable`. При подтверждённом отсутствии политика
имеет пустое множество участников: `exclude` обрабатывает всех, `include` —
никого. При отсутствии метки у существующей политики оба режима закрыты.
Последующий сбой RCI не возвращает старую метку и не меняет последнее
подтверждённое состояние.''')
replace(doc, '''До разрешения метки политики gate содержит безусловный `RETURN` и не требует
получения адресов.''', '''При закрытой области gate содержит безусловный `RETURN` и не требует
получения адресов. Во всех открытых областях, в том числе при отсутствии
исключаемой политики, сохраняются проверки локальных назначений, ответных
пакетов и connmark маршрута для downstream filter/NAT.''')
replace(doc, '''Это не транзакция сразу через IPv4/IPv6: ошибка отдельного семейства
показывается как `error`, существующий committer повторяет применение.''', '''Это не транзакция сразу через IPv4/IPv6: ошибка отдельного семейства
показывается как `error`, существующий committer повторяет применение.
Признак успешного применения сбрасывается в начале каждого прохода, даже
если конфигурация не менялась: старое совпадение поколений не подтверждает
наличие правил после их внешнего удаления или ошибки восстановления.''')
d = root / 'docs/c-rewrite/decisions.md'
d.write_text(d.read_text() + '''
## D-94 — Derive Keenetic traffic scope from one policy resolution (2026-11-12)

**Status: accepted.** An absent excluded policy has no members to exclude.
A validated RCI inventory resolves the selected policy to ABSENT, UNMARKED,
or RESOLVED; before the first valid response it is UNKNOWN. These facts are
independent of include/exclude. RCI errors retain the exact last resolution
and mark, never invent an empty inventory or reuse a former policy's mark.

One pure function derives NONE, ALL, INCLUDE or EXCLUDE traffic scope. ABSENT
opens exclude only. UNKNOWN and UNMARKED close both modes. RESOLVED preserves
mark-based membership. A resolved mark of zero remains valid. Membership
scope never disables router-local/reply protection or the route connmark
checks used by downstream filter/NAT. No extra flag or wording is required
in the WebUI; YAML and the pre-PR HTTP representation stay unchanged.

Every rebuild invalidates its applied-generation acknowledgement before
writing. Success acknowledges the current generation; errors/cancellation
cannot claim that an earlier same-generation application is still valid.
Tests cover IPv4/IPv6 client and router-local packets, connmarks, DNS guards,
RCI transitions/errors, and identical-generation failure and retry. Real
kernel legacy/nft recovery tests remain part of CI; none of these substitute
for validation of hardware-specific Keenetic behavior.
''')

for path in ['src/backend-c/include/magitrickle/keenetic_policy.h', app, api, apph, nf]:
    text = (root/path).read_text()
    for old in ['confirmed_absent', 'processing_all', 'mt_app_policy_excludes_none', 'mt_app_policy_set_resolved', '->ready', '.ready']:
        assert old not in text, (path, old)
for path in restore:
    if path.startswith('src/frontend/'):
        assert (root/path).read_text() == git('show', f'{BASE}:{path}')
subprocess.run(['git', '-C', str(root), 'diff', '--check'], check=True)
print(git('diff', '--stat'))
