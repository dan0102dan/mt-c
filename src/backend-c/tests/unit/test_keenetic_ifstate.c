#include "greatest.h"
#include "magitrickle/keenetic_ifstate.h"
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned changes;
static void changed(void *ud, const char *name) { (void)ud; if (*name) { changes++; } }
static const char *ready_json =
    "{\"Wireguard0\":{\"system-name\":\"nwg0\",\"state\":\"up\",\"link\":\"up\",\"connected\":\"yes\","
    "\"summary\":{\"layer\":{\"conf\":\"running\",\"link\":\"running\",\"ipv4\":\"running\","
    "\"ipv6\":\"running\",\"ctrl\":\"running\"}}}}";
static mt_err_t snapshot(mt_kn_ifstate_t *s, const char *json, uint64_t token)
{
    mt_kn_ifinfo_t *items = NULL; size_t n = 0;
    mt_err_t err = mt_kn_ifinfo_parse(json, strlen(json), &items, &n);
    if (err == MT_OK) { err = mt_kn_ifstate_snapshot(s, items, n, token); }
    free(items); return err;
}
static mt_err_t event(mt_kn_ifstate_t *s, const char *json, bool *sync)
{
    mt_kn_ifevent_t e;
    mt_err_t err = mt_kn_ifevent_parse(json, strlen(json), &e);
    return err == MT_OK ? mt_kn_ifstate_event(s, &e, sync) : err;
}
static mt_err_t layer(mt_kn_ifstate_t *s, const char *field, const char *value, bool *sync)
{
    char json[256];
    snprintf(json, sizeof(json), "{\"kind\":\"layer\",\"id\":\"Wireguard0\",\"system_name\":\"nwg0\",\"layer\":\"%s\",\"level\":\"%s\"}", field, value);
    return event(s, json, sync);
}
static mt_err_t legacy(mt_kn_ifstate_t *s, const char *up, const char *link, const char *connected, bool *sync)
{
    char json[256];
    snprintf(json, sizeof(json), "{\"kind\":\"state\",\"id\":\"Wireguard0\",\"system_name\":\"nwg0\",\"up\":\"%s\",\"link\":\"%s\",\"connected\":\"%s\"}", up, link, connected);
    return event(s, json, sync);
}

TEST user_wireguard_trace_is_not_ready_until_final_ctrl_event(void)
{
    mt_kn_ifstate_t *s = mt_kn_ifstate_new(changed, NULL); ASSERT(s);
    ASSERT_EQ(MT_OK, snapshot(s, ready_json, 0));
    changes = 0;
    /* 2026-10-13 trace supplied by the user. Each modern callback was followed
     * by a legacy callback; neither link=up nor the first running layer is ready. */
    const char *fields[] = {"ctrl","ipv6","ipv4","link","conf","conf","link","ipv4","ipv6","ctrl","link","ipv4","ipv6","ctrl"};
    const char *values[] = {"disabled","disabled","disabled","disabled","disabled","running","pending","pending","pending","pending","running","running","running","running"};
    bool sync = false;
    for (size_t i = 0; i < 14; i++) {
        ASSERT_EQ(MT_OK, layer(s, fields[i], values[i], &sync));
        ASSERT_FALSE(sync); /* warm baseline: no RCI after an ordinary event */
        ASSERT_EQ(i != 13, mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
        ASSERT_EQ(i != 13, mt_kn_ifstate_blocks(s, "nwg0", AF_INET6));
        ASSERT_EQ(MT_OK, legacy(s, i == 4 ? "down" : "up", i < 3 || i >= 10 ? "up" : "down", i == 13 ? "yes" : "no", &sync));
        ASSERT_FALSE(sync);
        ASSERT_EQ(i != 13, mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
        ASSERT_EQ(i != 13, mt_kn_ifstate_blocks(s, "nwg0", AF_INET6));
    }
    ASSERT_EQ(2u, changes); /* down once, recovered once */
    ASSERT_EQ(MT_OK, legacy(s, "up", "up", "no", &sync));
    ASSERT_FALSE(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    ASSERT_EQ(2u, changes);
    mt_kn_ifstate_free(s); PASS();
}
TEST families_and_nonapplicable_layers_are_independent(void)
{
    mt_kn_ifstate_t *s = mt_kn_ifstate_new(changed, NULL); ASSERT(s);
    ASSERT_EQ(MT_OK, snapshot(s, ready_json, 0));
    bool sync;
    ASSERT_EQ(MT_OK, layer(s, "ipv6", "disabled", &sync));
    ASSERT_FALSE(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    ASSERT(mt_kn_ifstate_blocks(s, "nwg0", AF_INET6));
    ASSERT_EQ(MT_OK, layer(s, "ipv4", "pending", &sync));
    ASSERT(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    ASSERT_EQ(MT_OK, layer(s, "ipv4", "running", &sync));
    ASSERT_FALSE(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    ASSERT_EQ(MT_OK, snapshot(s, "{\"Bridge0\":{\"system-name\":\"br0\",\"summary\":{\"layer\":{\"conf\":\"running\",\"link\":\"running\"}}}}", mt_kn_ifstate_revision(s)));
    ASSERT_FALSE(mt_kn_ifstate_blocks(s, "br0", AF_INET));
    ASSERT_FALSE(mt_kn_ifstate_blocks(s, "br0", AF_INET6));
    ASSERT_FALSE(mt_kn_ifstate_blocks(NULL, "nwg0", AF_INET));
    ASSERT_FALSE(mt_kn_ifstate_blocks(s, "unknown0", AF_INET));
    mt_kn_ifstate_free(s); PASS();
}
TEST legacy_connected_and_unknown_values_preserve_negative_knowledge(void)
{
    mt_kn_ifstate_t *s = mt_kn_ifstate_new(NULL, NULL); ASSERT(s);
    bool sync;
    ASSERT_EQ(MT_OK, legacy(s, "up", "up", "no", &sync));
    ASSERT(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    ASSERT_EQ(MT_OK, legacy(s, "up", "up", "future", &sync));
    ASSERT(sync); ASSERT(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    ASSERT_EQ(MT_OK, layer(s, "link", "running", &sync));
    ASSERT(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    ASSERT_EQ(MT_OK, snapshot(s, ready_json, mt_kn_ifstate_revision(s)));
    ASSERT_FALSE(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    ASSERT_EQ(MT_OK, layer(s, "ctrl", "disabled", &sync));
    ASSERT_EQ(MT_OK, layer(s, "ctrl", "unknown-new-level", &sync));
    ASSERT(sync); ASSERT(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    mt_kn_ifstate_free(s); PASS();
}
TEST legacy_only_interface_can_fail_over_and_recover(void)
{
    mt_kn_ifstate_t *s = mt_kn_ifstate_new(NULL, NULL); ASSERT(s);
    ASSERT_EQ(MT_OK, snapshot(s, "{\"Wireguard0\":{\"system-name\":\"nwg0\",\"state\":\"up\",\"link\":\"up\",\"connected\":\"yes\"}}", 0));
    bool sync;
    ASSERT_EQ(MT_OK, legacy(s, "up", "up", "no", &sync));
    ASSERT_FALSE(sync); ASSERT(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    ASSERT_EQ(MT_OK, legacy(s, "up", "up", "yes", &sync));
    ASSERT_FALSE(sync); ASSERT_FALSE(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    mt_kn_ifstate_free(s); PASS();
}
TEST stale_snapshots_do_not_overwrite_newer_fields_or_lifecycle(void)
{
    mt_kn_ifstate_t *s = mt_kn_ifstate_new(NULL, NULL); ASSERT(s);
    ASSERT_EQ(MT_OK, snapshot(s, ready_json, 0));
    bool sync;
    uint64_t token = mt_kn_ifstate_revision(s);
    ASSERT_EQ(MT_OK, layer(s, "ctrl", "disabled", &sync));
    ASSERT_EQ(MT_OK, snapshot(s, ready_json, token));
    ASSERT(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    ASSERT_EQ(MT_OK, layer(s, "ctrl", "running", &sync));
    token = mt_kn_ifstate_revision(s);
    const char *destroy = "{\"kind\":\"destroyed\",\"id\":\"Wireguard0\",\"system_name\":\"nwg0\"}";
    ASSERT_EQ(MT_OK, event(s, destroy, &sync));
    ASSERT_EQ(MT_OK, snapshot(s, ready_json, token));
    ASSERT(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    ASSERT_EQ(MT_OK, event(s, "{\"kind\":\"created\",\"id\":\"Wireguard0\",\"system_name\":\"nwg0\"}", &sync));
    ASSERT(sync);
    ASSERT_EQ(MT_OK, layer(s, "ctrl", "pending", &sync));
    ASSERT_EQ(MT_OK, snapshot(s, ready_json, token));
    ASSERT(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    ASSERT_EQ(MT_OK, snapshot(s, ready_json, mt_kn_ifstate_revision(s)));
    ASSERT_FALSE(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    mt_kn_ifstate_free(s); PASS();
}
TEST old_snapshot_cannot_steal_reused_system_name(void)
{
    /* Neither alphabetical order nor NDM hierarchy depth decides which
     * unrelated object owns a genuinely reassigned kernel name. */
    const char *ids[] = {"Replacement", "ZReplacement", "ZReplacement/Sub"};
    for (size_t i = 0; i < sizeof(ids) / sizeof(*ids); i++) {
        mt_kn_ifstate_t *s = mt_kn_ifstate_new(NULL, NULL); ASSERT(s);
        ASSERT_EQ(MT_OK, snapshot(s, ready_json, 0));
        uint64_t token = mt_kn_ifstate_revision(s);
        mt_kn_ifevent_t e = {.kind = MT_KN_IF_LAYER, .info = {.name = "nwg0"}};
        strcpy(e.info.id, ids[i]);
        e.info.fields[MT_KN_IF_CTRL] = MT_KN_IF_BLOCKED;
        bool sync;
        ASSERT_EQ(MT_OK, mt_kn_ifstate_event(s, &e, &sync));
        ASSERT(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
        ASSERT(mt_kn_ifstate_blocks(s, "nwg0", AF_INET6));
        ASSERT_EQ(MT_OK, snapshot(s, ready_json, token));
        ASSERT(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
        ASSERT(mt_kn_ifstate_blocks(s, "nwg0", AF_INET6));
        e.info.fields[MT_KN_IF_CTRL] = MT_KN_IF_RUNNING;
        ASSERT_EQ(MT_OK, mt_kn_ifstate_event(s, &e, &sync));
        ASSERT_FALSE(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
        ASSERT_FALSE(mt_kn_ifstate_blocks(s, "nwg0", AF_INET6));
        mt_kn_ifstate_free(s);
    }
    PASS();
}
TEST old_firmware_mapping_comes_from_hook_not_description(void)
{
    mt_kn_ifstate_t *s = mt_kn_ifstate_new(NULL, NULL); ASSERT(s);
    ASSERT_EQ(MT_OK, snapshot(s, "{\"Wireguard0\":{\"description\":\"nwg0\",\"interface-name\":\"Home VPN\",\"connected\":\"no\"}}", 0));
    ASSERT_FALSE(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    bool sync;
    ASSERT_EQ(MT_OK, legacy(s, "up", "up", "no", &sync));
    ASSERT(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    ASSERT_FALSE(sync);
    mt_kn_ifstate_free(s); PASS();
}
TEST a_layered_rcI_snapshot_does_not_disable_legacy_hooks_on_old_firmware(void)
{
    mt_kn_ifstate_t *s = mt_kn_ifstate_new(NULL, NULL); ASSERT(s);
    ASSERT_EQ(MT_OK, snapshot(s, ready_json, 0));
    bool sync;
    ASSERT_EQ(MT_OK, legacy(s, "up", "up", "no", &sync));
    ASSERT_FALSE(sync); ASSERT(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    uint64_t token = mt_kn_ifstate_revision(s);
    ASSERT_EQ(MT_OK, legacy(s, "down", "down", "no", &sync));
    ASSERT_EQ(MT_OK, snapshot(s, ready_json, token));
    ASSERT(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    ASSERT_EQ(MT_OK, legacy(s, "up", "up", "yes", &sync));
    ASSERT_FALSE(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    ASSERT_EQ(MT_OK, legacy(s, "up", "up", "no", &sync));
    ASSERT_EQ(MT_OK, layer(s, "link", "running", &sync));
    ASSERT(sync); ASSERT(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    ASSERT_EQ(MT_OK, snapshot(s, ready_json, mt_kn_ifstate_revision(s)));
    ASSERT_FALSE(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    ASSERT_EQ(MT_OK, legacy(s, "up", "up", "no", &sync));
    ASSERT_FALSE(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    mt_kn_ifstate_free(s); PASS();
}

TEST ambiguous_nested_fields_and_escaped_nul_are_rejected(void)
{
    const char *bad[] = {
        "{\"X\":{\"state\":\"up\",\"state\":\"down\"}}",
        "{\"X\":{\"summary\":{\"layer\":{\"ctrl\":\"running\",\"ctrl\":\"disabled\"}}}}",
        "{\"X\\u0000Y\":{}}",
        "{\"X\":{\"system-name\":\"nwg0\\u0000evil\"}}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        mt_kn_ifinfo_t *items = NULL; size_t count;
        ASSERT(mt_kn_ifinfo_parse(bad[i], strlen(bad[i]), &items, &count) != MT_OK);
        ASSERT(items == NULL);
    }
    mt_kn_ifevent_t e;
    const char *json = "{\"kind\":\"created\",\"id\":\"P\",\"system_name\":\"nwg0\\u0000evil\"}";
    ASSERT(mt_kn_ifevent_parse(json, strlen(json), &e) != MT_OK);
    PASS();
}

TEST malformed_messages_do_not_turn_into_empty_successes(void)
{
    mt_kn_ifinfo_t *items = NULL; size_t count = 0;
    const char *bad[] = {"{", "[]", "{}junk", "{\"X\":{},\"X\":{}}", "{\"X\":42}",
        "{\"X\":{\"id\":42}}", "{\"X\":{\"id\":\"bad id\"}}", "{\"X\":{\"id\":\"\"}}", "{\"X\":{\"system-name\":\"bad name\"}}", "{\"X\":{\"summary\":[]}}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        ASSERT(mt_kn_ifinfo_parse(bad[i], strlen(bad[i]), &items, &count) != MT_OK);
        ASSERT(items == NULL); ASSERT_EQ(0u, count);
    }
    ASSERT_EQ(MT_ERR_UPSTREAM, mt_kn_ifinfo_parse("{\"status\":[]}", 13, &items, &count));
    ASSERT_EQ(MT_ERR_LIMIT, mt_kn_ifinfo_parse("{}", MT_KN_IFSTATE_BODY_LIMIT + 1, &items, &count));
    mt_kn_ifevent_t e;
    ASSERT(mt_kn_ifevent_parse("{}", 2, &e) != MT_OK);
    const char *injection = "{\"kind\":\"created\",\"id\":\"P\",\"system_name\":\"x\\ncmd\"}";
    ASSERT(mt_kn_ifevent_parse(injection, strlen(injection), &e) != MT_OK);
    ASSERT_EQ(MT_ERR_LIMIT, mt_kn_ifevent_parse("{}", 1025, &e));
    ASSERT_EQ(MT_OK, mt_kn_ifinfo_parse("{}\n", 3, &items, &count));
    ASSERT(items == NULL); ASSERT_EQ(0u, count);
    PASS();
}

TEST numeric_port_keys_use_canonical_ids_and_detect_duplicates(void)
{
    mt_kn_ifinfo_t *items = NULL; size_t count = 0;
    const char *json = "{\"0\":{\"id\":\"GigabitEthernet1/0\",\"system-name\":\"eth3\"},"
                       "\"GigabitEthernet1\":{\"system-name\":\"eth3\",\"connected\":\"no\"},"
                       "\"alias\":{\"id\":\"Wireguard0\",\"system-name\":\"nwg0\",\"connected\":\"no\"}}";
    ASSERT_EQ(MT_OK, mt_kn_ifinfo_parse(json, strlen(json), &items, &count));
    ASSERT_EQ(3u, count);
    ASSERT_STR_EQ("GigabitEthernet1/0", items[0].id);
    ASSERT_STR_EQ("GigabitEthernet1", items[1].id);
    ASSERT_STR_EQ("Wireguard0", items[2].id);
    mt_kn_ifstate_t *s = mt_kn_ifstate_new(NULL, NULL); ASSERT(s);
    ASSERT_EQ(MT_OK, mt_kn_ifstate_snapshot(s, items, count, 0));
    ASSERT(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    ASSERT(mt_kn_ifstate_blocks(s, "nwg0", AF_INET6));
    mt_kn_ifstate_free(s);
    free(items);
    const char *duplicates[] = {
        "{\"0\":{\"id\":\"GigabitEthernet1/0\"},\"1\":{\"id\":\"GigabitEthernet1/0\"}}",
        "{\"0\":{\"id\":\"GigabitEthernet1/0\"},\"GigabitEthernet1/0\":{}}",
        "{\"GigabitEthernet1/0\":{},\"0\":{\"id\":\"GigabitEthernet1/0\"}}",
    };
    for (size_t i = 0; i < sizeof(duplicates) / sizeof(*duplicates); i++) {
        ASSERT_EQ(MT_ERR_PROTO, mt_kn_ifinfo_parse(duplicates[i], strlen(duplicates[i]), &items, &count));
        ASSERT(items == NULL); ASSERT_EQ(0u, count);
    }
    PASS();
}
TEST shared_kernel_name_uses_parent_not_last_child(void)
{
    mt_kn_ifstate_t *s = mt_kn_ifstate_new(NULL, NULL); ASSERT(s);
    bool sync;
    const char *parent = "{\"kind\":\"state\",\"id\":\"GigabitEthernet1\","
                         "\"system_name\":\"eth3\",\"up\":\"up\",\"link\":\"up\",\"connected\":\"no\"}";
    const char *child = "{\"kind\":\"state\",\"id\":\"GigabitEthernet1/0\","
                        "\"system_name\":\"eth3\",\"up\":\"up\",\"link\":\"up\",\"connected\":\"yes\"}";
    ASSERT_EQ(MT_OK, event(s, parent, &sync));
    ASSERT_EQ(MT_OK, event(s, child, &sync));
    ASSERT(mt_kn_ifstate_blocks(s, "eth3", AF_INET));
    ASSERT_EQ(MT_OK, event(s, parent, &sync));
    ASSERT_EQ(MT_OK, event(s, child, &sync));
    ASSERT(mt_kn_ifstate_blocks(s, "eth3", AF_INET));
    mt_kn_ifstate_free(s); PASS();
}
TEST stale_snapshot_cannot_restore_discarded_layer_baseline(void)
{
    mt_kn_ifstate_t *s = mt_kn_ifstate_new(NULL, NULL); ASSERT(s);
    ASSERT_EQ(MT_OK, snapshot(s, ready_json, 0));
    bool sync;
    ASSERT_EQ(MT_OK, legacy(s, "up", "up", "no", &sync));
    uint64_t token = mt_kn_ifstate_revision(s);
    ASSERT_EQ(MT_OK, layer(s, "link", "running", &sync));
    ASSERT(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    ASSERT_EQ(MT_OK, snapshot(s, ready_json, token));
    ASSERT(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    ASSERT_EQ(MT_OK, snapshot(s, ready_json, mt_kn_ifstate_revision(s)));
    ASSERT_FALSE(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    mt_kn_ifstate_free(s); PASS();
}
TEST shared_kernel_name_is_snapshot_order_independent(void)
{
    const unsigned orders[][3] = {{0,1,2}, {0,2,1}, {1,0,2}, {1,2,0}, {2,0,1}, {2,1,0}};
    const char *ids[] = {"GigabitEthernet1", "GigabitEthernet1/0", "GigabitEthernet1/1"};
    for (unsigned down = 0; down < 2; down++) {
        for (size_t order = 0; order < sizeof(orders) / sizeof(*orders); order++) {
            mt_kn_ifstate_t *s = mt_kn_ifstate_new(NULL, NULL); ASSERT(s);
            mt_kn_ifinfo_t inventory[3] = {0};
            for (size_t i = 0; i < 3; i++) {
                unsigned member = orders[order][i];
                strcpy(inventory[i].id, ids[member]);
                strcpy(inventory[i].name, "eth3");
                /* Inverse child state: neither an idle port nor a connected
                 * port overrides the parent's routing availability. */
                bool unavailable = member == 0 ? down != 0 : down == 0;
                inventory[i].fields[MT_KN_IF_CONNECTED] = unavailable ? MT_KN_IF_BLOCKED : MT_KN_IF_RUNNING;
            }
            ASSERT_EQ(MT_OK, mt_kn_ifstate_snapshot(s, inventory, 3, 0));
            ASSERT_EQ(down != 0, mt_kn_ifstate_blocks(s, "eth3", AF_INET));
            ASSERT_EQ(down != 0, mt_kn_ifstate_blocks(s, "eth3", AF_INET6));
            for (size_t i = 0; i < 3; i++) {
                mt_kn_ifevent_t e = {.kind = MT_KN_IF_LEGACY, .info = inventory[i]};
                bool sync;
                ASSERT_EQ(MT_OK, mt_kn_ifstate_event(s, &e, &sync));
                ASSERT_FALSE(sync);
                ASSERT_EQ(down != 0, mt_kn_ifstate_blocks(s, "eth3", AF_INET));
                ASSERT_EQ(down != 0, mt_kn_ifstate_blocks(s, "eth3", AF_INET6));
            }
            mt_kn_ifstate_free(s);
        }
    }
    PASS();
}
TEST first_modern_event_freezes_legacy_snapshot_until_family_recovery(void)
{
    mt_kn_ifstate_t *s = mt_kn_ifstate_new(NULL, NULL); ASSERT(s);
    const char *old = "{\"Wireguard0\":{\"system-name\":\"nwg0\",\"state\":\"up\",\"link\":\"up\",\"connected\":\"no\"}}";
    ASSERT_EQ(MT_OK, snapshot(s, old, 0));
    uint64_t token = mt_kn_ifstate_revision(s);
    bool sync;
    ASSERT_EQ(MT_OK, layer(s, "link", "running", &sync));
    ASSERT(sync);
    /* Neither a companion legacy event nor an old snapshot may release the
     * negative fallback before the relevant modern layers are complete. */
    ASSERT_EQ(MT_OK, legacy(s, "up", "up", "yes", &sync));
    ASSERT(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    ASSERT(mt_kn_ifstate_blocks(s, "nwg0", AF_INET6));
    ASSERT_EQ(MT_OK, snapshot(s, ready_json, token));
    ASSERT(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    ASSERT(mt_kn_ifstate_blocks(s, "nwg0", AF_INET6));
    ASSERT_EQ(MT_OK, layer(s, "conf", "running", &sync));
    ASSERT_EQ(MT_OK, layer(s, "ipv4", "running", &sync));
    ASSERT_EQ(MT_OK, layer(s, "ctrl", "running", &sync));
    ASSERT_FALSE(mt_kn_ifstate_blocks(s, "nwg0", AF_INET));
    ASSERT(mt_kn_ifstate_blocks(s, "nwg0", AF_INET6));
    ASSERT_EQ(MT_OK, layer(s, "ipv6", "running", &sync));
    ASSERT_FALSE(mt_kn_ifstate_blocks(s, "nwg0", AF_INET6));
    mt_kn_ifstate_free(s); PASS();
}
GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(shared_kernel_name_is_snapshot_order_independent);
    RUN_TEST(first_modern_event_freezes_legacy_snapshot_until_family_recovery);
    RUN_TEST(numeric_port_keys_use_canonical_ids_and_detect_duplicates);
    RUN_TEST(shared_kernel_name_uses_parent_not_last_child);
    RUN_TEST(stale_snapshot_cannot_restore_discarded_layer_baseline);
    RUN_TEST(user_wireguard_trace_is_not_ready_until_final_ctrl_event);
    RUN_TEST(families_and_nonapplicable_layers_are_independent);
    RUN_TEST(legacy_connected_and_unknown_values_preserve_negative_knowledge);
    RUN_TEST(legacy_only_interface_can_fail_over_and_recover);
    RUN_TEST(stale_snapshots_do_not_overwrite_newer_fields_or_lifecycle);
    RUN_TEST(old_snapshot_cannot_steal_reused_system_name);
    RUN_TEST(old_firmware_mapping_comes_from_hook_not_description);
    RUN_TEST(malformed_messages_do_not_turn_into_empty_successes);
    RUN_TEST(a_layered_rcI_snapshot_does_not_disable_legacy_hooks_on_old_firmware);
    RUN_TEST(ambiguous_nested_fields_and_escaped_nul_are_rejected);
    GREATEST_MAIN_END();
}
