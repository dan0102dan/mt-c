import { strict as assert } from "node:assert";

import {
  applyRuleOverrides,
  assertPlanCurrent,
  copy,
  diffSnapshots,
  exportSnapshot,
  fingerprint,
  planImport,
  type ImportOptions,
  type Snapshot,
} from "../../src/modules/config-transfer/model";

const snapshot = (): Snapshot => ({
  groups: [
    {
      id: "11223344",
      name: "Video",
      color: "#ffffff",
      interface: "wg0",
      profile: "vpn",
      enable: true,
      priority: 300,
      rules: [
        {
          id: "aabbccdd",
          rule: "example.org",
          name: "Example",
          type: "namespace",
          enable: true,
        },
      ],
    },
  ],
  subscriptions: [
    {
      id: "55667788",
      name: "Clouds",
      url: "https://example.org/list?token=secret",
      interface: "wg0",
      profile: "vpn",
      enable: true,
      priority: 100,
      interval: 86400,
      overrides: [{ rule: "disabled.org", enable: false }],
    },
  ],
  profiles: [
    { id: "vpn", name: "VPN", interfaces: ["wg0", "wg1"], on_unavailable: "blackhole" },
    { id: "unused", name: "Spare", interfaces: ["tun0"], on_unavailable: "blackhole" },
  ],
  settings: {
    keeneticPolicy: { enabled: true, policyId: "Policy0", mode: "include" },
    dns: { port: 5353, upstream: "127.0.0.1" },
  },
});
const opts = (extra: Partial<ImportOptions> = {}): ImportOptions => {
  const ruleIds = new Map<string, string>();
  return {
    mode: "merge",
    selected: new Set(["groups", "subscriptions", "profiles", "settings"]),
    newId: (section, id) => {
      if (section === "profiles") return `copy_${id}`;
      if (!id.startsWith("rule:")) return "abcdef01";
      if (!ruleIds.has(id)) ruleIds.set(id, (0x10000000 + ruleIds.size).toString(16));
      return ruleIds.get(id)!;
    },
    ...extra,
  };
};

Deno.test("export preserves unfinished drafts and all sections", () => {
  const state = snapshot();
  state.profiles[0].name = "  Unsaved profile  ";
  state.groups[0].name = "";
  state.subscriptions[0].url = " incomplete draft URL ";
  const before = fingerprint(state);
  const result = exportSnapshot(state, { application: "mt-c" }, "2026-10-15T00:00:00Z");
  assert.equal(result.profiles?.length, 2);
  assert.equal(result.profiles?.[0].name, "  Unsaved profile  ");
  assert.equal(result.groups?.[0].name, "");
  assert.equal(result.subscriptions?.[0].url, " incomplete draft URL ");
  assert.deepEqual(result.settings, { keeneticPolicy: state.settings.keeneticPolicy });
  assert.equal(fingerprint(state), before);
});
Deno.test("export strips subscription caches, timestamps and profile usage", () => {
  const state = snapshot();
  Object.assign(state.subscriptions[0], {
    rules: Array.from({ length: 1000 }, (_, i) => ({ rule: `${i}.org` })),
    lastUpdate: 123,
    lastCheck: 456,
    sync_pending: true,
  });
  Object.assign(state.profiles[0], { usage: { groups: 1, subscriptions: 1 }, active: "wg1" });
  const result = exportSnapshot(state, { application: "mt-c" });
  assert(!("rules" in result.subscriptions![0]));
  assert(!("lastUpdate" in result.subscriptions![0]));
  assert(!("sync_pending" in result.subscriptions![0]));
  assert(!("usage" in result.profiles![0]));
  assert(!("active" in result.profiles![0]));
  assert.deepEqual(result.subscriptions![0].overrides, [{ rule: "disabled.org", enable: false }]);
  assert.equal(result.groups![0].rules.length, 1);
  assert.equal(result.profiles![0].interfaces[0], "wg0");
});
Deno.test("export detaches all nested arrays and settings", () => {
  const state = snapshot();
  const result = exportSnapshot(state, { application: "mt-c" });
  state.groups[0].rules[0].enable = false;
  state.profiles[0].interfaces.reverse();
  state.subscriptions[0].overrides![0].enable = true;
  (state.settings.dns as { port: number }).port = 9999;
  assert.equal(result.groups![0].rules[0].enable, true);
  assert.deepEqual(result.profiles![0].interfaces, ["wg0", "wg1"]);
  assert.equal(result.subscriptions![0].overrides![0].enable, false);
  assert.equal(result.settings!.dns, undefined);
});
Deno.test("replace distinguishes absent sections from explicitly empty groups", () => {
  const state = snapshot();
  const result = planImport(state, { groups: [] }, opts({ mode: "replace" }));
  assert.deepEqual(result.next.groups, []);
  assert.deepEqual(result.next.subscriptions, state.subscriptions);
  assert.deepEqual(result.next.profiles, state.profiles);
  assert.deepEqual(result.next.settings, state.settings);
  assert.equal(result.changes.filter((c) => c.kind === "remove").length, 1);
});
Deno.test("an unselected empty section never deletes anything", () => {
  const state = snapshot();
  const options = opts({ mode: "replace", selected: new Set() });
  const result = planImport(state, { groups: [] }, options);
  assert.deepEqual(result.next, state);
});
Deno.test("import permits subscription/settings-only snapshots", () => {
  const state = snapshot();
  const added = { ...state.subscriptions[0], id: "deadbeef" };
  const input = { subscriptions: [added], settings: { log: "debug" } };
  const result = planImport(state, input, opts());
  assert.equal(result.next.groups.length, 1);
  assert.equal(result.next.subscriptions.length, 2);
  assert.equal(result.next.settings.log, "debug");
});
Deno.test("repeat merge is idempotent and retains routing IDs", () => {
  const state = snapshot();
  const input = exportSnapshot(state, { application: "mt-c" });
  const first = planImport(state, input, opts());
  const again = planImport(first.next, input, opts());
  assert.deepEqual(first.next, state);
  assert.deepEqual(again.next, state);
  assert(again.changes.every((c) => c.kind === "unchanged"));
});
Deno.test("a shared URL or name is not subscription identity", () => {
  const state = snapshot();
  const input = { subscriptions: [{ ...state.subscriptions[0], id: "deadbeef" }] };
  const result = planImport(state, input, opts());
  assert.equal(result.next.subscriptions.length, 2);
});
Deno.test("conflicts keep current values unless replacement is selected", () => {
  const state = snapshot();
  const input = { groups: [{ ...state.groups[0], name: "Different" }] };
  const kept = planImport(state, input, opts());
  assert.equal(kept.next.groups[0].name, "Video");
  assert.deepEqual(kept.conflicts, [{ section: "groups", id: "11223344", name: "Different" }]);
  const options = opts({ choices: new Map([["groups:11223344", "replace"]]) });
  const replaced = planImport(state, input, options);
  assert.equal(replaced.next.groups[0].name, "Different");
  assert.equal(replaced.changes[0].fields[0].field, "name");
});
Deno.test("copying a group gives its rules distinct stable editor identities", () => {
  const state = snapshot();
  const input = { groups: [{ ...copy(state.groups[0]), name: "Copy" }] };
  const before = fingerprint(input);
  const options = opts({ choices: new Map([["groups:11223344", "copy"]]) });
  const result = planImport(state, input, options);
  assert.deepEqual(result.problems, []);
  assert.deepEqual(result.next.groups[0], state.groups[0]);
  const added = result.next.groups[1];
  assert.equal(added.id, "abcdef01");
  assert.notEqual(added.rules[0].id, state.groups[0].rules[0].id);
  assert.notEqual(added.rules[0].id, added.id);
  assert.deepEqual(
    { ...added.rules[0], id: state.groups[0].rules[0].id },
    state.groups[0].rules[0],
  );
  assert.deepEqual(planImport(state, input, options).next, result.next);
  assert.equal(fingerprint(input), before);
});
Deno.test("new groups remap colliding rules without changing unique or saved identities", () => {
  const state = snapshot();
  const rule = state.groups[0].rules[0];
  const input = {
    groups: [
      {
        ...copy(state.groups[0]),
        id: "deadbeef",
        rules: [
          { ...rule },
          { ...rule, id: state.groups[0].id, rule: "root.org" },
          { ...rule, id: "facefeed", rule: "unique.org" },
        ],
      },
      {
        ...copy(state.groups[0]),
        id: "deadc0de",
        rules: [
          { ...rule, id: "facefeed", rule: "other.org" },
          { ...rule, id: "facefeed", rule: "duplicate-id.org" },
          { ...rule, id: "deadc0de", rule: "own-root.org" },
        ],
      },
    ],
  };
  const before = fingerprint(input);
  const options = opts();
  const result = planImport(state, input, options);
  assert.deepEqual(result.problems, []);
  assert.deepEqual(result.next.groups[0], state.groups[0]);
  assert.equal(result.next.groups[1].rules[2].id, "facefeed");
  const ids = result.next.groups.flatMap((group) => [group.id, ...group.rules.map((r) => r.id)]);
  assert.equal(new Set(ids).size, ids.length);
  assert.deepEqual(planImport(state, input, options).next, result.next);
  assert.equal(fingerprint(input), before);
});
Deno.test("replacement preserves its own rules and remaps collisions with another group", () => {
  const state = snapshot();
  state.groups.push({
    ...copy(state.groups[0]),
    id: "deadbeef",
    rules: [{ ...state.groups[0].rules[0], id: "facefeed" }],
  });
  const input = { groups: [copy(state.groups[0])] };
  input.groups[0].rules[0].enable = false;
  input.groups[0].rules.push({ ...state.groups[1].rules[0], rule: "new.org" });
  const result = planImport(
    state,
    input,
    opts({ choices: new Map([["groups:11223344", "replace"]]) }),
  );
  assert.deepEqual(result.problems, []);
  assert.equal(result.next.groups[0].rules[0].id, state.groups[0].rules[0].id);
  assert.equal(result.next.groups[0].rules[0].enable, false);
  assert.notEqual(result.next.groups[0].rules[1].id, "facefeed");
  assert.deepEqual(result.next.groups[1], state.groups[1]);
});
Deno.test("allocated rule IDs cannot collide with existing or later imported objects", () => {
  const state = snapshot();
  const input = {
    groups: [
      {
        ...copy(state.groups[0]),
        id: "deadbeef",
        rules: [
          { ...state.groups[0].rules[0] },
          { ...state.groups[0].rules[0], id: "facefeed", rule: "later.org" },
        ],
      },
    ],
  };
  for (const id of ["11223344", "aabbccdd", "deadbeef", "facefeed"]) {
    const result = planImport(state, input, opts({ newId: () => id }));
    assert(result.problems.includes(`Allocated rule ID already exists: ${id}`));
    assert.throws(() => assertPlanCurrent(result, state), /Allocated rule ID already exists/);
  }
});
Deno.test("an imported root cannot reuse a retained rule ID or rewrite its saved owner", () => {
  const state = snapshot();
  const input = { groups: [{ ...copy(state.groups[0]), id: "aabbccdd", rules: [] }] };
  const result = planImport(state, input, opts());
  assert(result.problems.includes("Group/rule ID collision: aabbccdd"));
  assert.throws(() => assertPlanCurrent(result, state), /Group\/rule ID collision/);
  assert.deepEqual(result.next.groups[0], state.groups[0]);
});
Deno.test("copying a profile remaps groups and subscriptions together", () => {
  const state = snapshot();
  const input = {
    groups: [{ ...state.groups[0], id: "deadbeef" }],
    subscriptions: [{ ...state.subscriptions[0], id: "01020304" }],
    profiles: [{ ...state.profiles[0], interfaces: ["tun0"] }],
  };
  const options = opts({ choices: new Map([["profiles:vpn", "copy"]]) });
  const result = planImport(state, input, options);
  assert.deepEqual(result.problems, []);
  assert.equal(result.next.groups[1].profile, "copy_vpn");
  assert.equal(result.next.subscriptions[1].profile, "copy_vpn");
  assert.equal(result.next.groups[1].interface, "tun0");
  assert.equal(result.next.groups[0].profile, "vpn");
  assert.equal(result.next.profiles.length, 3);
});
Deno.test("selected groups bring their profile definitions as dependencies", () => {
  const state = snapshot();
  const input = {
    groups: [{ ...state.groups[0], id: "deadbeef", profile: "new_profile" }],
    profiles: [{ ...state.profiles[0], id: "new_profile", interfaces: ["tun0"] }],
  };
  const result = planImport(state, input, opts({ selected: new Set(["groups"]) }));
  assert.deepEqual(result.includedDependencies, ["new_profile"]);
  assert.equal(result.next.profiles.length, 3);
  assert.deepEqual(result.problems, []);
});
Deno.test("an unselected unfinished profile does not block importing valid groups", () => {
  const state = snapshot();
  const added = { ...state.groups[0], id: "deadbeef", profile: undefined };
  const input = {
    groups: [added],
    profiles: [{ id: "unfinished", name: "", interfaces: [] }],
  };
  const result = planImport(state, input, opts({ selected: new Set(["groups"]) }));
  assertPlanCurrent(result, state);
  assert.equal(result.next.groups.at(-1)?.id, added.id);
  assert.deepEqual(result.next.profiles, state.profiles);
});
Deno.test("invalid accepted profile definitions block application with their identity", () => {
  const state = snapshot();
  for (const profile of [
    { id: "unfinished", name: "", interfaces: ["wg0"] },
    { id: "unfinished", name: "Unfinished", interfaces: [] },
    { id: "unfinished", name: "Unfinished", interfaces: ["not an interface"] },
    { id: "unfinished", name: "Unfinished", interfaces: ["wg0", "wg0"] },
    { id: "unfinished", name: "Unfinished", interfaces: ["wg0"], on_unavailable: "accept" },
  ]) {
    const result = planImport(
      state,
      { profiles: [profile] },
      opts({ selected: new Set(["profiles"]) }),
    );
    assert(
      result.problems.some((problem) => problem.startsWith("Invalid routing profile: unfinished:")),
    );
    assert.throws(() => assertPlanCurrent(result, state), /Invalid routing profile: unfinished:/);
  }
});
Deno.test("unfinished required profile blocks its selected consumers", () => {
  const state = snapshot();
  const input = {
    groups: [{ ...state.groups[0], id: "deadbeef", profile: "unfinished" }],
    profiles: [{ id: "unfinished", name: "Unfinished", interfaces: [] }],
  };
  const result = planImport(state, input, opts({ selected: new Set(["groups"]) }));
  assert.deepEqual(result.includedDependencies, ["unfinished"]);
  assert.deepEqual(result.problems, [
    "Invalid routing profile: unfinished: Add at least one interface",
  ]);
  assert.throws(() => assertPlanCurrent(result, state), /Add at least one interface/);
});
Deno.test(
  "keeping a valid current profile does not import its unfinished conflicting draft",
  () => {
    const state = snapshot();
    const input = {
      groups: [{ ...state.groups[0], id: "deadbeef" }],
      profiles: [{ ...state.profiles[0], name: "", interfaces: [] }],
    };
    const kept = planImport(state, input, opts());
    assertPlanCurrent(kept, state);
    assert.deepEqual(kept.next.profiles, state.profiles);
    assert.equal(kept.next.groups.at(-1)?.interface, "wg0");
    const replaced = planImport(
      state,
      input,
      opts({ choices: new Map([["profiles:vpn", "replace"]]) }),
    );
    assert.throws(() => assertPlanCurrent(replaced, state), /Invalid routing profile: vpn:/);
  },
);
Deno.test("profile changes expose effects on retained consumers", () => {
  const state = snapshot();
  const input = {
    profiles: [{ ...state.profiles[0], interfaces: ["wg1", "wg0"] }, state.profiles[1]],
  };
  const options = opts({ selected: new Set(["profiles"]), mode: "replace" });
  const result = planImport(state, input, options);
  assert.equal(result.next.groups[0].interface, "wg1");
  assert.equal(result.next.subscriptions[0].interface, "wg1");
  const changed = result.changes.filter((c) => c.kind === "update").map((c) => c.section);
  assert.deepEqual(changed, ["groups", "subscriptions", "profiles"]);
});
Deno.test("removing a referenced profile prevents application", () => {
  const state = snapshot();
  const options = opts({ selected: new Set(["profiles"]), mode: "replace" });
  const plan = planImport(state, { profiles: [] }, options);
  assert(plan.problems.some((p) => p.includes("Missing routing profile")));
  assert.throws(() => assertPlanCurrent(plan, state), /Missing routing profile/);
});
Deno.test("interface mapping preserves order and rejects duplicate results", () => {
  const state = snapshot();
  const input = { profiles: state.profiles };
  const options = opts({
    mode: "replace",
    interfaceMap: new Map([
      ["wg0", "nwg0"],
      ["wg1", "nwg1"],
    ]),
  });
  const mapped = planImport(state, input, options);
  assert.deepEqual(mapped.next.profiles[0].interfaces, ["nwg0", "nwg1"]);
  options.interfaceMap = new Map([
    ["wg0", "nwg0"],
    ["wg1", "nwg0"],
  ]);
  const invalid = planImport(state, input, options);
  assert(invalid.problems.some((p) => p.includes("duplicate interfaces")));
});
Deno.test("duplicate IDs and cross-section route collisions are reported", () => {
  const state = snapshot();
  const duplicateInput = { groups: [state.groups[0], state.groups[0]] };
  const duplicate = planImport(state, duplicateInput, opts());
  assert(duplicate.problems.some((p) => p.includes("Duplicate groups ID")));
  const collisionInput = {
    subscriptions: [{ ...state.subscriptions[0], id: state.groups[0].id }],
  };
  const collision = planImport(state, collisionInput, opts());
  assert(collision.problems.some((p) => p.includes("ID collision")));
});
Deno.test("allocated copy IDs cannot collide with existing definitions", () => {
  const state = snapshot();
  const input = { profiles: [{ ...state.profiles[0], name: "New" }] };
  const options = opts({
    choices: new Map([["profiles:vpn", "copy"]]),
    newId: () => "unused",
  });
  const result = planImport(state, input, options);
  assert(result.problems.some((p) => p.includes("already exists")));
});
Deno.test("nested settings survive and policy mapping is explicit", () => {
  const state = snapshot();
  const input = {
    settings: {
      dns: { port: 5354 },
      keeneticPolicy: { enabled: true, policyId: "Foreign0", mode: "exclude" },
    },
  };
  const result = planImport(state, input, opts({ policyId: "Local0" }));
  assert.deepEqual(result.next.settings.dns, { port: 5354, upstream: "127.0.0.1" });
  assert.deepEqual(result.next.settings.keeneticPolicy, {
    enabled: true,
    policyId: "Local0",
    mode: "exclude",
  });
});
Deno.test("settings overlays reject prototype mutation keys", () => {
  const input = { settings: JSON.parse('{"__proto__":{"polluted":true}}') };
  assert.throws(() => planImport(snapshot(), input, opts()), /Invalid settings key/);
  assert.equal(({} as { polluted?: boolean }).polluted, undefined);
});
Deno.test("preview is bound to draft values, not object property order", () => {
  const state = snapshot();
  const plan = planImport(state, { groups: [] }, opts());
  assertPlanCurrent(plan, copy(state));
  state.groups[0].name = "Edited while preview open";
  assert.throws(() => assertPlanCurrent(plan, state), /refresh the import preview/);
  assert.equal(fingerprint({ a: 1, b: 2 }), fingerprint({ b: 2, a: 1 }));
  assert.notEqual(fingerprint(["wg0", "wg1"]), fingerprint(["wg1", "wg0"]));
});
Deno.test("planning never mutates either input", () => {
  const state = snapshot();
  const input = exportSnapshot(state, { application: "mt-c" });
  const a = fingerprint(state);
  const b = fingerprint(input);
  const options = opts({ mode: "replace", interfaceMap: new Map([["wg0", "nwg0"]]) });
  planImport(state, input, options);
  assert.equal(fingerprint(state), a);
  assert.equal(fingerprint(input), b);
});
Deno.test("overrides use patterns and do not recreate removed source rules", () => {
  const fresh = [{ id: "new-id", rule: "present.org", enable: true, type: "namespace" }];
  const overrides = [
    { rule: "present.org", enable: false, type: "domain" },
    { rule: "removed.org", enable: false },
  ];
  const result = applyRuleOverrides(fresh, overrides);
  assert.deepEqual(result.rules, [
    { id: "new-id", rule: "present.org", enable: false, type: "domain" },
  ]);
  assert.deepEqual(result.unmatched, [{ rule: "removed.org", enable: false }]);
  assert.equal(fresh[0].enable, true);
  assert.throws(
    () => applyRuleOverrides(fresh, [overrides[0], overrides[0]]),
    /Duplicate override/,
  );
});
Deno.test("type-only overrides do not change enabled state", () => {
  const fresh = [{ rule: "example.org", type: "namespace", enable: false }];
  const result = applyRuleOverrides(fresh, [{ rule: "example.org", type: "domain" }]);
  assert.equal(result.rules[0].enable, false);
});
Deno.test("diff reports deletions and ordered interface changes", () => {
  const before = snapshot();
  const after = copy(before);
  after.groups = [];
  after.profiles[0].interfaces.reverse();
  const changes = diffSnapshots(before, after);
  assert(changes.some((c) => c.section === "groups" && c.kind === "remove"));
  const profileChanges = changes.filter((c) => c.section === "profiles");
  assert(profileChanges.some((c) => c.fields.some((f) => f.field === "interfaces")));
});
Deno.test("export works with frozen editor data without mutations", () => {
  const state = snapshot();
  Object.freeze(state.groups[0].rules[0]);
  Object.freeze(state.groups[0].rules);
  Object.freeze(state.groups[0]);
  Object.freeze(state.groups);
  const result = exportSnapshot(state, { application: "mt-c" });
  assert.equal(result.groups![0].rules[0].rule, "example.org");
});
Deno.test("no-op imports do not normalize unrelated unfinished route drafts", () => {
  const state = snapshot();
  state.groups[0].interface = "unfinished0";
  const result = planImport(state, {}, opts({ selected: new Set() }));
  assert.deepEqual(result.next, state);
  assert(result.changes.every((change) => change.kind === "unchanged"));
});
Deno.test("missing, false, zero, empty string and null are distinct settings", () => {
  const state = snapshot();
  const input = { settings: { disabled: false, zero: 0, text: "", reset: null } };
  const result = planImport(state, input, opts());
  assert.equal(result.next.settings.disabled, false);
  assert.equal(result.next.settings.zero, 0);
  assert.equal(result.next.settings.text, "");
  assert.equal(result.next.settings.reset, null);
  assert.equal(result.changes.filter((change) => change.section === "settings").length, 4);
});
Deno.test("settings diff identifies changed leaf fields", () => {
  const state = snapshot();
  const result = planImport(state, { settings: { dns: { port: 5354 } } }, opts());
  const changes = result.changes.filter((change) => change.section === "settings");
  assert.equal(changes.length, 1);
  assert.deepEqual(changes[0].fields, [{ field: "dns.port", before: 5353, after: 5354 }]);
});
Deno.test("prototype keys are rejected in new settings objects and arrays", () => {
  for (const json of [
    '{"new":{"__proto__":{"polluted":true}}}',
    '{"list":[{"constructor":{"prototype":{"polluted":true}}}]}',
  ]) {
    const input = { settings: JSON.parse(json) };
    assert.throws(() => planImport(snapshot(), input, opts()), /Invalid settings key/);
  }
});
Deno.test("subscription rule cache size never changes portable export", () => {
  const state = snapshot();
  const date = "2026-10-15T00:00:00Z";
  const small = JSON.stringify(exportSnapshot(state, { application: "mt-c" }, date));
  Object.assign(state.subscriptions[0], {
    rules: Array.from({ length: 10000 }, (_, i) => ({ rule: `${i}.example.org` })),
  });
  const large = JSON.stringify(exportSnapshot(state, { application: "mt-c" }, date));
  assert.equal(large, small);
});
