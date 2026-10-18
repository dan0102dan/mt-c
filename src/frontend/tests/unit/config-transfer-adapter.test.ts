import { strict as assert } from "node:assert";

import type { ServerConfig } from "../../src/modules/config-transfer/adapter";
import { exportSnapshot } from "../../src/modules/config-transfer/model";
import type { Subscription } from "../../src/types";
import { installSvelteRunesMocks } from "../mocks/setup-svelte-runes";

installSvelteRunesMocks();
const { configForApply, decodePortable, snapshotFromServer } =
  await import("../../src/modules/config-transfer/adapter");

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

Deno.test("preserves cached rules for untouched subscriptions only", () => {
  const original = server();
  const state = snapshotFromServer(original);
  const existing = configForApply(original, state);
  assert.equal(existing.subscriptions[0].rules, undefined);
  assert.equal(existing.subscriptions[0].last_update, 145);
  state.subscriptions[0].url = "https://other.example.org/list";
  const changed = configForApply(original, state);
  assert.deepEqual(changed.subscriptions[0].rules, []);
  assert.equal(changed.subscriptions[0].last_update, 0);
});

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
