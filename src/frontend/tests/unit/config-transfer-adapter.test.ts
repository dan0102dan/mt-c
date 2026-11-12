import { strict as assert } from "node:assert";

import type { ServerConfig } from "../../src/modules/config-transfer/adapter";
import {
  applyRuleOverrides,
  assertPlanCurrent,
  exportSnapshot,
  planImport,
  type RuleOverride,
} from "../../src/modules/config-transfer/model";
import type { Subscription } from "../../src/types";
import { installSvelteRunesMocks } from "../mocks/setup-svelte-runes";

installSvelteRunesMocks();
const {
  assertImportApplied,
  configForApply,
  decodePortable,
  snapshotFromServer,
  subscriptionEditorsForImport,
} = await import("../../src/modules/config-transfer/adapter");

Deno.test("applied import verification rejects missing or different profiles", () => {
  const original = server();
  const expected = snapshotFromServer(original);
  assert.doesNotThrow(() => assertImportApplied(expected, original));
  for (const profiles of [
    [],
    [{ ...original.profiles![0], name: "Ignored import" }],
    [{ ...original.profiles![0], interfaces: ["wg1", "wg0"] }],
  ]) {
    assert.throws(() => assertImportApplied(expected, { ...original, profiles }), /not confirmed/);
  }
});

Deno.test("applied import verification ignores caches and equivalent profile defaults", () => {
  const original = server();
  const expected = snapshotFromServer(original);
  expected.subscriptions[0].overrides = [];
  const actual = structuredClone(original);
  actual.profiles![0].on_unavailable = "blackhole";
  actual.subscriptions[0].last_update++;
  actual.subscriptions[0].rules = [];
  assert.doesNotThrow(() => assertImportApplied(expected, actual));
});

Deno.test(
  "applied import verification accepts backend color and empty profile canonicalization",
  () => {
    for (const [color, canonical] of [
      ["#AABBCC", "#aabbcc"],
      ["", "#ffffff"],
      ["banana", "#ffffff"],
      ["#fff", "#ffffff"],
      ["#abcdef\n", "#ffffff"],
    ]) {
      const original = server();
      original.groups = decodePortable(
        JSON.stringify({
          groups: [
            { id: "12345678", name: "Legacy", interface: "wg0", profile: "", color, rules: [] },
          ],
        }),
      ).groups!;
      const expected = snapshotFromServer(original);
      expected.subscriptions[0].profile = "";
      const before = structuredClone(expected);
      const actual = configForApply(original, expected);
      actual.groups[0].color = canonical;
      delete actual.groups[0].profile;
      delete actual.subscriptions[0].profile;
      assert.doesNotThrow(() => assertImportApplied(expected, actual));
      assert.deepEqual(expected, before);

      actual.groups[0].color = "#123456";
      assert.throws(() => assertImportApplied(expected, actual), /not confirmed/);
      actual.groups[0].color = canonical;
      actual.groups[0].profile = "vpn";
      assert.throws(() => assertImportApplied(expected, actual), /not confirmed/);
    }
  },
);

const server = (): ServerConfig => ({
  configVersion: "0.8.5",
  app: { logLevel: "info", keeneticPolicy: { enabled: false, policyId: "", mode: "include" } },
  groups: [],
  profiles: [{ id: "vpn", name: "VPN", interfaces: ["wg0", "wg1"] }],
  subscriptions: [
    {
      id: "11223344",
      name: "Cloud",
      interface: "wg0",
      profile: "vpn",
      url: "https://example.org/list",
      enable: true,
      priority: 100,
      interval: 86400,
      last_update: 145,
      rules: [{ id: "12345678", rule: "example.org", type: "namespace", enable: true }],
      overrides: [],
    },
  ],
});

Deno.test("legacy group-only config remains a partial import", () => {
  const input = decodePortable(
    JSON.stringify({
      groups: [
        {
          id: "12345678",
          name: "Legacy",
          interface: "wg0",
          enable: true,
          rules: [],
          color: "#ffffff",
        },
      ],
    }),
  );
  assert.equal(input.groups?.length, 1);
  assert.equal(input.subscriptions, undefined);
  assert.equal(input.settings, undefined);
});

Deno.test("legacy missing group and rule IDs receive distinct editor identities", () => {
  const group = {
    name: "Legacy",
    interface: "wg0",
    rules: [{ rule: "one.example" }, { rule: "two.example" }],
  };
  const input = decodePortable(JSON.stringify({ groups: [group, group] }));
  const ids = input.groups!.flatMap((item) => [item.id, ...item.rules.map((rule) => rule.id)]);
  assert.equal(ids.length, 6);
  assert.equal(new Set(ids).size, ids.length);
  assert(ids.every((id) => /^[a-f\d]{8}$/.test(id)));
});

Deno.test("explicitly malformed import IDs are rejected instead of silently replaced", () => {
  const valid = {
    id: "12345678",
    interface: "wg0",
    rules: [{ id: "aabbccdd", rule: "example.org" }],
  };
  for (const id of [null, false, 12345678, "", "1234567", "123456789", "abcdefgh"]) {
    assert.throws(
      () => decodePortable(JSON.stringify({ groups: [{ ...valid, id }] })),
      /Invalid group ID/,
    );
    assert.throws(
      () =>
        decodePortable(
          JSON.stringify({ groups: [{ ...valid, rules: [{ ...valid.rules[0], id }] }] }),
        ),
      /Invalid rule ID/,
    );
  }
});

Deno.test("versioned portable groups require explicit group and rule IDs", () => {
  const version = { format: "mt-c-config", schemaVersion: 1 };
  assert.throws(
    () => decodePortable(JSON.stringify({ ...version, groups: [{ interface: "wg0", rules: [] }] })),
    /Invalid group ID/,
  );
  assert.throws(
    () =>
      decodePortable(
        JSON.stringify({
          ...version,
          groups: [{ id: "12345678", interface: "wg0", rules: [{ rule: "example.org" }] }],
        }),
      ),
    /Invalid rule ID/,
  );
});

Deno.test("hex IDs retain their backend identity instead of triggering schema fallback", () => {
  const original = server();
  const input = {
    groups: [
      { id: "AABBCCDD", interface: "wg0", rules: [{ id: "FACEFEED", rule: "example.org" }] },
    ],
    subscriptions: [{ ...original.subscriptions[0], id: "DEADBEEF" }],
  };
  const decoded = decodePortable(JSON.stringify(input));
  assert.equal(decoded.groups![0].id, "aabbccdd");
  assert.equal(decoded.groups![0].rules[0].id, "facefeed");
  assert.equal(decoded.subscriptions![0].id, "deadbeef");
  assert.deepEqual(decodePortable(JSON.stringify(input)), decoded);
});

Deno.test("legacy generated IDs avoid identities declared later in the file", () => {
  const originalRandom = Math.random;
  let calls = 0;
  Math.random = () => (calls++ < 8 ? 0 : 0.5);
  try {
    const input = decodePortable(
      JSON.stringify({
        groups: [
          { interface: "wg0", rules: [] },
          { id: "00000000", interface: "wg0", rules: [] },
        ],
      }),
    );
    assert.equal(input.groups![0].id, "88888888");
    assert.equal(input.groups![1].id, "00000000");
  } finally {
    Math.random = originalRandom;
  }
});

Deno.test("case variants cannot disguise a duplicate route identity", () => {
  const original = server();
  const incoming = decodePortable(
    JSON.stringify({
      groups: [{ id: "AABBCCDD", interface: "wg0", rules: [] }],
      subscriptions: [{ ...original.subscriptions[0], id: "aabbccdd" }],
    }),
  );
  const state = snapshotFromServer(original);
  const plan = planImport(state, incoming, {
    mode: "replace",
    selected: new Set(["groups", "subscriptions"]),
    newId: () => "deadbeef",
  });
  assert.throws(() => assertPlanCurrent(plan, state), /Group\/subscription ID collision: aabbccdd/);
});

Deno.test("versioned empty sections are accepted", () => {
  const input = decodePortable(
    JSON.stringify({
      format: "mt-c-config",
      schemaVersion: 1,
      groups: [],
      subscriptions: [],
      profiles: [],
      settings: {},
    }),
  );
  assert.deepEqual(input.groups, []);
  assert.deepEqual(input.profiles, []);
});

Deno.test("imported override patterns enforce the backend UTF-8 byte limit", () => {
  for (const rule of [
    "a".repeat(4096),
    "я".repeat(2048),
    "界".repeat(1365) + "a",
    "😀".repeat(1024),
  ]) {
    const state = snapshotFromServer(server());
    state.subscriptions[0].overrides = [{ rule, enable: false }];
    const portable = exportSnapshot(state, { application: "mt-c" });
    assert.deepEqual(decodePortable(JSON.stringify(portable)).subscriptions![0].overrides, [
      { rule, enable: false },
    ]);
    portable.subscriptions![0].overrides![0].rule += "a";
    assert.throws(
      () => decodePortable(JSON.stringify(portable)),
      /Invalid or duplicate rule override/,
    );
  }
});

Deno.test("malformed and future files are rejected safely", () => {
  for (const input of [
    "{ broken",
    "[]",
    "{}",
    '{"format":"unknown","groups":[]}',
    '{"schemaVersion":2,"groups":[]}',
    '{"settings":{"__proto__":{"polluted":true}}}',
    '{"subscriptions":[{"id":"bad"}]}',
  ])
    assert.throws(() => decodePortable(input));
  assert.equal(({} as { polluted?: boolean }).polluted, undefined);
});

Deno.test("an exported unfinished profile does not prevent reading valid groups", () => {
  const state = snapshotFromServer(server());
  state.groups = [
    {
      id: "12345678",
      name: "Video",
      interface: "wg0",
      enable: true,
      priority: 300,
      color: "#ffffff",
      rules: [],
    },
  ];
  state.profiles.push({
    id: "unfinished",
    name: "",
    interfaces: [],
    on_unavailable: "blackhole",
  });
  const input = decodePortable(JSON.stringify(exportSnapshot(state, { application: "mt-c" })));
  assert.deepEqual(input.groups, state.groups);
  assert.deepEqual(input.profiles?.at(-1), state.profiles.at(-1));
});

Deno.test("profile shape and identity errors still reject the file before selection", () => {
  const valid = { id: "vpn", name: "VPN", interfaces: ["wg0"] };
  for (const profiles of [
    [null],
    [{ ...valid, id: 1 }],
    [{ ...valid, id: "../invalid" }],
    [{ ...valid, name: 1 }],
    [{ ...valid, interfaces: "wg0" }],
    [{ ...valid, interfaces: [1] }],
    [{ ...valid, on_unavailable: false }],
    [valid, valid],
  ]) {
    assert.throws(() => decodePortable(JSON.stringify({ groups: [], profiles })));
  }
});

Deno.test("snapshot captures unsaved group/profile/keenetic drafts", () => {
  const original = server();
  const state = snapshotFromServer(original, {
    groups: [
      {
        id: "12345678",
        name: "",
        interface: "",
        color: "#ffffff",
        enable: true,
        priority: 300,
        rules: [],
      },
    ],
    profiles: [{ id: "vpn", name: "Unfinished", interfaces: ["wg1", "wg0"] }],
    policy: { enabled: true, policy_id: "Policy0", mode: "exclude" },
  });
  assert.equal(state.groups[0].name, "");
  assert.equal(state.profiles[0].interfaces[0], "wg1");
  assert.deepEqual(state.settings.keeneticPolicy, {
    enabled: true,
    policyId: "Policy0",
    mode: "exclude",
  });
  const portable = exportSnapshot(state, { application: "mt-c" });
  assert.equal(portable.profiles?.[0].name, "Unfinished");
  assert.equal(original.profiles?.[0].name, "VPN");
});

Deno.test("unsaved changes to subscription rule are exported as sparse override", () => {
  const original = server();
  const draft: Subscription = {
    id: "11223344",
    name: "Cloud",
    interface: "wg0",
    profile: "vpn",
    url: "https://example.org/list",
    enable: true,
    priority: 100,
    interval: 86400,
    lastUpdate: 145,
    rules: [{ id: "12345678", rule: "example.org", type: "domain", enable: false }],
  };
  const state = snapshotFromServer(original, { subscriptions: [draft] });
  assert.deepEqual(state.subscriptions[0].overrides, [
    {
      rule: "example.org",
      type: "domain",
      enable: false,
    },
  ]);
  const portable = exportSnapshot(state, { application: "mt-c" });
  assert(!("rules" in portable.subscriptions![0]));
});

Deno.test("unsaved edits preserve the other field of an existing subscription override", () => {
  const source = { id: "12345678", rule: "example.org", type: "namespace", enable: true };
  const savedOverrides: RuleOverride[] = [
    { rule: source.rule, type: "domain" },
    { rule: source.rule, enable: false },
  ];
  for (const override of savedOverrides) {
    const original = server();
    const saved = { ...source, ...override };
    original.subscriptions[0].rules = [saved];
    original.subscriptions[0].overrides = [override];
    const draft: Subscription = {
      ...original.subscriptions[0],
      lastUpdate: 145,
      rules: [{ ...saved, type: "domain", enable: false }],
    };
    const portable = exportSnapshot(snapshotFromServer(original, { subscriptions: [draft] }), {
      application: "mt-c",
    });
    const imported = decodePortable(JSON.stringify(portable));
    const restored = applyRuleOverrides([source], imported.subscriptions![0].overrides ?? []);
    assert.deepEqual(restored.rules, draft.rules);
    assert.deepEqual(original.subscriptions[0].overrides, [override]);
    assert.deepEqual(original.subscriptions[0].rules, [saved]);
  }
});

Deno.test("preserves cached rules for untouched subscriptions only", () => {
  const original = server();
  const state = snapshotFromServer(original);
  const existing = configForApply(original, state);
  assert.equal(existing.subscriptions[0].rules, undefined);
  assert.deepEqual(existing.subscriptions[0].overrides, []);
  assert.equal(existing.subscriptions[0].last_update, 145);
  state.subscriptions[0].url = "https://other.example.org/list";
  const changed = configForApply(original, state);
  assert.deepEqual(changed.subscriptions[0].rules, []);
  assert.equal(changed.subscriptions[0].last_update, 0);
});

function editorSubscriptions(original: ServerConfig): Subscription[] {
  return original.subscriptions.map(({ last_update, ...subscription }) => ({
    ...structuredClone(subscription),
    lastUpdate: last_update,
    rules: structuredClone(subscription.rules ?? []),
  }));
}

Deno.test(
  "explicit empty imported overrides restore enabled state and stay empty in the next snapshot",
  () => {
    const original = server();
    original.subscriptions[0].rules![0].enable = false;
    original.subscriptions[0].overrides = [{ rule: "example.org", enable: false }];
    const definitions = snapshotFromServer(original).subscriptions;
    definitions[0].overrides = [];
    const staged = subscriptionEditorsForImport(definitions, editorSubscriptions(original));
    assert.equal(staged[0].rules[0].enable, true);
    assert.deepEqual(staged[0].overrides, []);
    const snapshot = snapshotFromServer(original, {
      subscriptions: staged,
      subscriptionRuleBaseline: structuredClone(staged),
    });
    assert.deepEqual(snapshot.subscriptions[0].overrides, []);
    assert.deepEqual(configForApply(original, snapshot).subscriptions[0].overrides, []);
    assert.equal(original.subscriptions[0].rules![0].enable, false);
  },
);

Deno.test(
  "rule edits after staging compare to imported values even when they equal the old server state",
  () => {
    const original = server();
    original.subscriptions[0].rules![0].enable = false;
    original.subscriptions[0].rules![0].type = "domain";
    original.subscriptions[0].overrides = [{ rule: "example.org", enable: false, type: "domain" }];
    const definitions = snapshotFromServer(original).subscriptions;
    definitions[0].overrides = [{ rule: "example.org", type: "namespace" }];
    const baseline = subscriptionEditorsForImport(definitions, editorSubscriptions(original));
    const edited = structuredClone(baseline);
    edited[0].rules[0].enable = false;
    edited[0].rules[0].type = "domain";
    const snapshot = snapshotFromServer(original, {
      subscriptions: edited,
      subscriptionRuleBaseline: baseline,
    });
    assert.deepEqual(snapshot.subscriptions[0].overrides, [
      { rule: "example.org", type: "domain", enable: false },
    ]);
    assert.equal(baseline[0].rules[0].enable, true);
    assert.equal(baseline[0].rules[0].type, "namespace");
  },
);

Deno.test(
  "removed source type overrides invalidate only the staged cache while keeping server cache ownership",
  () => {
    const original = server();
    original.subscriptions[0].rules![0].type = "domain";
    original.subscriptions[0].overrides = [{ rule: "example.org", type: "domain" }];
    const snapshot = snapshotFromServer(original);
    snapshot.subscriptions[0].overrides = [];
    const staged = subscriptionEditorsForImport(
      snapshot.subscriptions,
      editorSubscriptions(original),
    );
    assert.deepEqual(staged[0].rules, []);
    assert.equal(staged[0].lastUpdate, 145);
    const target = snapshotFromServer(original, {
      subscriptions: staged,
      subscriptionRuleBaseline: staged,
    });
    const payload = configForApply(original, target);
    assert.equal(payload.subscriptions[0].rules, undefined);
    assert.deepEqual(payload.subscriptions[0].overrides, []);
    assert.equal(original.subscriptions[0].rules![0].type, "domain");
  },
);

Deno.test("confirmed subscription caches take precedence over stale draft cache values", () => {
  const original = server();
  const stale = editorSubscriptions(original);
  stale[0].rules[0].type = "domain";
  stale[0].rules[0].enable = false;
  stale[0].overrides = [{ rule: "example.org", type: "domain", enable: false }];
  const confirmed = editorSubscriptions(original);
  const result = subscriptionEditorsForImport(
    snapshotFromServer(original).subscriptions,
    confirmed,
    stale,
  );
  assert.deepEqual(result[0].rules, original.subscriptions[0].rules);
  assert.deepEqual(result[0].overrides, []);
});

Deno.test("source cache stays local to both subscription identity and URL", () => {
  const original = server();
  const definitions = snapshotFromServer(original).subscriptions;
  for (const changed of [{ id: "deadbeef" }, { url: "https://other.example.org/list" }]) {
    const result = subscriptionEditorsForImport(
      [{ ...definitions[0], ...changed }],
      editorSubscriptions(original),
    );
    assert.deepEqual(result[0].rules, []);
    assert.equal(result[0].lastUpdate, 0);
  }
});

Deno.test("removing an unmatched type override does not hide unrelated cached rules", () => {
  const original = server();
  original.subscriptions[0].overrides = [{ rule: "not-in-cache.example", type: "domain" }];
  const definitions = snapshotFromServer(original).subscriptions;
  definitions[0].overrides = [];
  const staged = subscriptionEditorsForImport(definitions, editorSubscriptions(original));
  assert.deepEqual(staged[0].rules, original.subscriptions[0].rules);
  assert.deepEqual(staged[0].overrides, []);
});

Deno.test(
  "a changed source URL cannot use staged rule values as an edit baseline for another source",
  () => {
    const original = server();
    const draft = editorSubscriptions(original);
    draft[0].rules[0].enable = false;
    const unrelated = editorSubscriptions(original);
    unrelated[0].url = "https://different.example/list";
    unrelated[0].rules[0].enable = false;
    const result = snapshotFromServer(original, {
      subscriptions: draft,
      subscriptionRuleBaseline: unrelated,
    });
    assert.deepEqual(result.subscriptions[0].overrides, [{ rule: "example.org", enable: false }]);
  },
);

Deno.test("portable override changes retained cache but not source", () => {
  const original = server();
  const state = snapshotFromServer(original);
  state.subscriptions[0].overrides = [{ rule: "example.org", enable: false }];
  const result = configForApply(original, state);
  assert.equal(result.subscriptions[0].rules, undefined);
  assert.deepEqual(result.subscriptions[0].overrides, [{ rule: "example.org", enable: false }]);
  assert.equal(original.subscriptions[0].rules[0].enable, true);
});

Deno.test("all profiles including unused definitions remain in export", () => {
  const original = server();
  original.profiles?.push({ id: "unused", name: "Unused", interfaces: ["tun0"] });
  const state = snapshotFromServer(original);
  const portable = exportSnapshot(state, { application: "mt-c" });
  assert.deepEqual(
    portable.profiles?.map((p) => p.id),
    ["vpn", "unused"],
  );
});

Deno.test("portable export contains only WebUI settings and no daemon credentials", () => {
  const original = server();
  original.app.httpWeb = { auth: { password: "private-password" }, host: { port: 6969 } };
  original.app.dnsProxy = { upstream: "private-dns", timeout: "10s" };
  original.app.netfilter = { disableIPv4: false };
  original.app.keeneticPolicy = {
    enabled: true,
    policyId: "Policy0",
    mode: "exclude",
    runtimeMark: 123,
  };
  const exported = exportSnapshot(snapshotFromServer(original), { application: "mt-c" });
  assert.deepEqual(exported.settings, {
    keeneticPolicy: { enabled: true, policyId: "Policy0", mode: "exclude" },
  });
  assert(!JSON.stringify(exported).includes("private-password"));
  delete original.app.keeneticPolicy;
  assert(
    !Object.hasOwn(
      exportSnapshot(snapshotFromServer(original), { application: "mt-c" }),
      "settings",
    ),
  );
});

Deno.test(
  "older portable files ignore daemon fields while retaining editable policy fields",
  () => {
    const imported = decodePortable(
      JSON.stringify({
        settings: {
          logLevel: "debug",
          httpWeb: { auth: { password: "other" } },
          dnsProxy: { timeout: "1s" },
          netfilter: { disableIPv4: true },
          link: [],
          showAllInterfaces: true,
          keeneticPolicy: { enabled: true, policyId: "Policy0", mode: "exclude", runtimeMark: 123 },
        },
      }),
    );
    assert.deepEqual(imported.settings, {
      keeneticPolicy: { enabled: true, policyId: "Policy0", mode: "exclude" },
    });
  },
);

Deno.test(
  "applying portable settings preserves daemon configuration even if planning is bypassed",
  () => {
    const original = server();
    original.app.httpWeb = { auth: { password: "keep" } };
    original.app.dnsProxy = { timeout: "10s" };
    original.app.keeneticPolicy = { enabled: false, policyId: "", mode: "include", internal: 42 };
    const target = snapshotFromServer(original);
    target.settings = {
      logLevel: "debug",
      httpWeb: { auth: { password: "replace" } },
      dnsProxy: { timeout: "1s" },
      keeneticPolicy: { enabled: true, policyId: "Policy0", mode: "exclude", internal: 99 },
    };
    const applied = configForApply(original, target);
    assert.equal(applied.app.logLevel, "info");
    assert.deepEqual(applied.app.httpWeb, original.app.httpWeb);
    assert.deepEqual(applied.app.dnsProxy, original.app.dnsProxy);
    assert.deepEqual(applied.app.keeneticPolicy, {
      enabled: true,
      policyId: "Policy0",
      mode: "exclude",
      internal: 42,
    });
    assert.equal((original.app.keeneticPolicy as { enabled: boolean }).enabled, false);
  },
);
