import { strict as assert } from "node:assert";

import {
  projectImportDraft,
  rebaseImportDraft,
  stageImportDraft,
} from "../../src/modules/config-transfer/draft";
import {
  assertPlanCurrent,
  copy,
  planImport,
  type ImportOptions,
  type Snapshot,
} from "../../src/modules/config-transfer/model";

const saved = (): Snapshot => ({
  groups: [
    {
      id: "11223344",
      name: "Saved group",
      interface: "wg0",
      profile: "vpn",
      enable: true,
      priority: 300,
      rules: [{ id: "aabbccdd", name: "", rule: "example.org", type: "namespace", enable: true }],
    },
  ],
  subscriptions: [
    {
      id: "55667788",
      name: "Saved list",
      interface: "wg0",
      profile: "vpn",
      enable: true,
      priority: 100,
      url: "https://example.org/list",
      interval: 86400,
      overrides: [],
    },
  ],
  profiles: [
    { id: "vpn", name: "Saved VPN", interfaces: ["wg0", "wg1"], on_unavailable: "blackhole" },
  ],
  settings: {
    keeneticPolicy: { enabled: false, policyId: "", mode: "include" },
    daemonOnly: "keep",
  },
});
const options = (extra: Partial<ImportOptions> = {}): ImportOptions => ({
  mode: "merge",
  selected: new Set(["groups"]),
  newId: (section) => (section === "profiles" ? "copied_vpn" : "deadbeef"),
  ...extra,
});

Deno.test("staging imports has no shared editor or saved baseline references", () => {
  const server = saved();
  const editor = copy(server);
  const plan = planImport(editor, { groups: [] }, options({ mode: "replace" }));
  const pending = stageImportDraft(server, editor, plan);
  assert.deepEqual(server, editor);
  assert.deepEqual(pending.sections, ["groups"]);
  assert.deepEqual(pending.baseline, server);
  server.groups[0].rules[0].enable = false;
  assert.equal(pending.baseline.groups[0].rules[0].enable, true);
  pending.baseline.profiles[0].interfaces.reverse();
  assert.equal(editor.profiles[0].interfaces[0], "wg0");
});

Deno.test(
  "group imports save edited dependencies without committing unrelated unfinished profiles",
  () => {
    const server = saved();
    const editor = copy(server);
    editor.profiles.push({ id: "unfinished", name: "", interfaces: [] });
    const incoming = {
      groups: [
        {
          ...copy(editor.groups[0]),
          id: "deadc0de",
          name: "Imported",
          profile: "imported",
          rules: [],
        },
      ],
      profiles: [{ id: "imported", name: "Imported VPN", interfaces: ["tun0", "tun1"] }],
    };
    const plan = planImport(editor, incoming, options());
    assertPlanCurrent(plan, editor);
    const pending = stageImportDraft(server, editor, plan);
    const current = copy(plan.next);
    current.profiles.find((profile) => profile.id === "imported")!.name = "Edited imported profile";
    current.profiles.find((profile) => profile.id === "imported")!.interfaces.reverse();
    current.groups[1].name = "Edited imported group";
    const target = projectImportDraft(pending, current, ["groups"]);
    assert.deepEqual(
      target.profiles.map((profile) => profile.id),
      ["vpn", "imported"],
    );
    assert.equal(target.profiles[1].name, "Edited imported profile");
    assert.equal(target.groups[1].name, "Edited imported group");
    assert.equal(target.groups[1].interface, "tun1");
    assert.deepEqual(target.subscriptions, server.subscriptions);
    assert.deepEqual(target.settings, server.settings);
    assert.equal(
      current.profiles.some((profile) => profile.id === "unfinished"),
      true,
    );
  },
);

Deno.test(
  "an unchanged dependency that exists only in the editor is included with its route",
  () => {
    const server = saved();
    const editor = copy(server);
    const profile = { id: "draft_vpn", name: "Draft VPN", interfaces: ["tun0"] };
    editor.profiles.push(profile);
    const plan = planImport(
      editor,
      {
        groups: [{ ...copy(editor.groups[0]), id: "deadc0de", profile: profile.id, rules: [] }],
        profiles: [profile],
      },
      options(),
    );
    const pending = stageImportDraft(server, editor, plan);
    const target = projectImportDraft(pending, plan.next, ["groups"]);
    assert.deepEqual(target.profiles.at(-1), profile);
    assert.equal(target.groups.at(-1)?.profile, profile.id);
  },
);

Deno.test("copied profile dependencies use the remapped identity in the Save payload", () => {
  const server = saved();
  const plan = planImport(
    server,
    {
      groups: [{ ...copy(server.groups[0]), id: "deadc0de", rules: [] }],
      profiles: [{ ...server.profiles[0], interfaces: ["tun0"] }],
    },
    options({ choices: new Map([["profiles:vpn", "copy"]]) }),
  );
  assert.deepEqual(plan.includedDependencies, ["copied_vpn"]);
  const pending = stageImportDraft(server, server, plan);
  const target = projectImportDraft(pending, plan.next, ["groups"]);
  assert.deepEqual(
    target.profiles.map((profile) => profile.id),
    ["vpn", "copied_vpn"],
  );
  assert.equal(target.groups.at(-1)?.profile, "copied_vpn");
});

Deno.test(
  "partial imports save a referenced local-only profile and exclude unrelated drafts",
  () => {
    const server = saved();
    const editor = copy(server);
    const required = { id: "local", name: "Local VPN", interfaces: ["tun0"] };
    editor.profiles.push(required, { id: "unfinished", name: "", interfaces: [] });
    for (const section of ["groups", "subscriptions"] as const) {
      for (const mode of ["merge", "replace"] as const) {
        const plan = planImport(
          editor,
          { [section]: [{ ...copy(editor[section][0]), id: "deadc0de", profile: required.id }] },
          options({ mode, selected: new Set([section]) }),
        );
        assertPlanCurrent(plan, editor);
        const pending = stageImportDraft(server, editor, plan);
        const target = projectImportDraft(pending, plan.next, [section]);
        assert.deepEqual(target.profiles, [...server.profiles, required]);
        assert.equal(target[section].at(-1)?.profile, required.id);
        assert.equal(target[section].at(-1)?.interface, "tun0");
      }
    }
  },
);

Deno.test("partial imports save an edited current primary and only derived consumer fields", () => {
  const server = saved();
  for (const section of ["groups", "subscriptions"] as const) {
    const editor = copy(server);
    editor.profiles[0].interfaces.reverse();
    editor.profiles.push({ id: "unfinished", name: "", interfaces: [] });
    const other = section === "groups" ? "subscriptions" : "groups";
    editor[other][0].name = "";
    const plan = planImport(
      editor,
      { [section]: [{ ...copy(editor[section][0]), id: "deadc0de" }] },
      options({ selected: new Set([section]) }),
    );
    assertPlanCurrent(plan, editor);
    const target = projectImportDraft(stageImportDraft(server, editor, plan), plan.next, [section]);
    assert.deepEqual(target.profiles, [editor.profiles[0]]);
    assert.equal(target[section].at(-1)?.interface, "wg1");
    assert.deepEqual(target[other], [{ ...server[other][0], interface: "wg1" }]);
    assert.equal(editor[other][0].name, "");
  }
});

Deno.test(
  "profile imports update consumer interfaces without saving unrelated group and subscription drafts",
  () => {
    const server = saved();
    const editor = copy(server);
    editor.groups[0].name = "Unrelated unfinished group";
    editor.groups[0].rules[0].rule = "";
    editor.subscriptions[0].url = "unfinished URL";
    editor.groups.push({
      ...copy(editor.groups[0]),
      id: "deadc0de",
      name: "Local new group",
      rules: [],
    });
    const plan = planImport(
      editor,
      { profiles: [{ ...editor.profiles[0], interfaces: ["tun0"] }] },
      options({ mode: "replace", selected: new Set(["profiles"]) }),
    );
    assertPlanCurrent(plan, editor);
    const pending = stageImportDraft(server, editor, plan);
    const current = copy(plan.next);
    current.profiles[0].interfaces = ["tun1", "tun0"];
    const target = projectImportDraft(pending, current, ["profiles", "settings"]);
    assert.deepEqual(target.groups, [{ ...server.groups[0], interface: "tun1" }]);
    assert.deepEqual(target.subscriptions, [{ ...server.subscriptions[0], interface: "tun1" }]);
    assert.deepEqual(target.profiles[0].interfaces, ["tun1", "tun0"]);
    assert.equal(current.groups.length, 2);
    assert.equal(current.groups[0].rules[0].rule, "");
  },
);

Deno.test(
  "multiple imports compose intent and Replace clears an already locally emptied section",
  () => {
    const server = saved();
    const editor = copy(server);
    editor.groups = [];
    const groupPlan = planImport(editor, { groups: [] }, options({ mode: "replace" }));
    const first = stageImportDraft(server, editor, groupPlan);
    const subscriptionPlan = planImport(
      groupPlan.next,
      { subscriptions: [{ ...copy(server.subscriptions[0]), name: "Imported list" }] },
      options({ mode: "replace", selected: new Set(["subscriptions"]) }),
    );
    const composed = stageImportDraft(server, groupPlan.next, subscriptionPlan, first);
    const target = projectImportDraft(composed, subscriptionPlan.next, ["groups"]);
    assert.deepEqual(target.groups, []);
    assert.equal(target.subscriptions[0].name, "Imported list");
    assert.deepEqual(target.profiles, server.profiles);
    assert.deepEqual(first.sections, ["groups"]);
    assert.deepEqual(composed.sections, ["groups", "subscriptions"]);
  },
);

Deno.test(
  "Save on another page includes that page's latest edits while retaining the imported section",
  () => {
    const server = saved();
    const plan = planImport(server, { groups: [] }, options({ mode: "replace" }));
    const pending = stageImportDraft(server, server, plan);
    const current = copy(plan.next);
    current.subscriptions[0].name = "Edited list";
    current.profiles.push({ id: "unfinished", name: "", interfaces: [] });
    const target = projectImportDraft(pending, current, ["subscriptions"]);
    assert.deepEqual(target.groups, []);
    assert.equal(target.subscriptions[0].name, "Edited list");
    assert.deepEqual(target.profiles, server.profiles);
  },
);

Deno.test(
  "accepting Save preserves unrelated drafts and edits made while the request is running",
  () => {
    const submitted = saved();
    submitted.groups[0].name = "Submitted group";
    const current = copy(submitted);
    current.groups[0].name = "Typed while saving";
    current.groups[0].rules.push({
      ...current.groups[0].rules[0],
      id: "facefeed",
      rule: "new.example",
    });
    current.profiles.push({ id: "unfinished", name: "", interfaces: [] });
    current.subscriptions = [];
    const confirmed = copy(submitted);
    confirmed.settings.daemonOnly = "server-owned update";
    const result = rebaseImportDraft(submitted, current, confirmed);
    assert.equal(result.groups[0].name, "Typed while saving");
    assert.equal(result.groups[0].rules.length, 2);
    assert.deepEqual(result.profiles, current.profiles);
    assert.deepEqual(result.subscriptions, []);
    assert.equal(result.settings.daemonOnly, "server-owned update");
    assert.deepEqual(submitted, savedWithName("Submitted group"));
    result.groups[0].rules[0].enable = false;
    assert.equal(current.groups[0].rules[0].enable, true);
    assert.equal(confirmed.groups[0].rules[0].enable, true);
  },
);

function savedWithName(name: string): Snapshot {
  const snapshot = saved();
  snapshot.groups[0].name = name;
  return snapshot;
}

Deno.test(
  "server canonicalization is accepted while later order and setting edits remain drafts",
  () => {
    const submitted = saved();
    submitted.profiles[0].name = "  VPN  ";
    submitted.groups.push({ ...copy(submitted.groups[0]), id: "deadbeef", rules: [] });
    const confirmed = copy(submitted);
    confirmed.profiles[0].name = "VPN";
    const current = copy(submitted);
    current.groups.reverse();
    current.settings.keeneticPolicy = { enabled: true, policyId: "NewPolicy", mode: "include" };
    const result = rebaseImportDraft(submitted, current, confirmed);
    assert.equal(result.profiles[0].name, "VPN");
    assert.deepEqual(
      result.groups.map((group) => group.id),
      ["deadbeef", "11223344"],
    );
    assert.deepEqual(result.settings, current.settings);
  },
);

Deno.test("derived primary normalization does not create false dirty consumers after Save", () => {
  const atSave = saved();
  atSave.profiles[0].interfaces = ["tun0"];
  atSave.groups[0].name = "Unrelated draft name";
  const submitted = saved();
  submitted.profiles[0].interfaces = ["tun0"];
  submitted.groups[0].interface = "tun0";
  submitted.subscriptions[0].interface = "tun0";
  const result = rebaseImportDraft(submitted, atSave, submitted, atSave);
  assert.equal(result.groups[0].interface, "tun0");
  assert.equal(result.subscriptions[0].interface, "tun0");
  assert.equal(result.groups[0].name, "Unrelated draft name");
  const current = copy(atSave);
  current.groups[0].profile = undefined;
  current.groups[0].interface = "later0";
  const later = rebaseImportDraft(submitted, current, submitted, atSave);
  assert.equal(later.groups[0].interface, "later0");
  assert.equal(later.groups[0].profile, undefined);
});
