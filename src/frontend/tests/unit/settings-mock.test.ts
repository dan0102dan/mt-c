import assert from "node:assert/strict";
import { Hono } from "hono";

import { registerMockSettingsRoutes } from "../../dev/settings-mock.ts";
import type { ServerConfig } from "../../src/modules/config-transfer/adapter.ts";
import { generalServerConfig, generalSettings } from "../mocks/general-settings.ts";

const BASE = "/api/v1/system/settings";

function fixture() {
  const app = new Hono();
  const state = { config: generalServerConfig(), writes: 0, fail: false, runtimeActive: true };
  registerMockSettingsRoutes(app, {
    get: () => structuredClone(state.config),
    apply: (candidate) => {
      if (state.fail) throw new Error("Cannot persist general settings");
      state.config = structuredClone(candidate) as ServerConfig;
      state.writes++;
      state.runtimeActive = true;
    },
    runtimeActive: () => state.runtimeActive,
  });
  return {
    app,
    state,
    get: async () => (await app.request(BASE)).json(),
    put: (body: unknown) =>
      app.request(BASE, {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      }),
    preview: (body: unknown) =>
      app.request(`${BASE}/preview`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      }),
  };
}

Deno.test("settings preview distinguishes live changes and remains read-only", async () => {
  const { get, preview, state } = fixture();
  const initial = await get();
  const settings = generalSettings();
  settings.httpWeb.enabled = false;
  settings.logLevel = "debug";
  settings.showAllInterfaces = true;
  settings.httpWeb.auth.enabled = true;
  settings.dnsProxy.disableFakePTR = true;
  settings.dnsProxy.disableDropAAAA = true;
  settings.netfilter.ipset.additionalTTLSeconds = 0;
  const response = await preview({ settings, revision: initial.revision });
  assert.equal(response.status, 200);
  assert.equal(response.headers.get("Cache-Control"), "no-store");
  assert.deepEqual(await response.json(), { dns_restart_required: false });
  const noOp = await preview({ settings: initial.settings, revision: initial.revision });
  assert.deepEqual(await noOp.json(), { dns_restart_required: false });
  settings.dnsProxy.upstream.address = "8.8.8.8";
  const dnsChange = await preview({ settings, revision: initial.revision });
  assert.deepEqual(await dnsChange.json(), { dns_restart_required: true });
  assert.deepEqual(await get(), initial);
  assert.equal(state.writes, 0);
});

Deno.test(
  "a denied DNS restart has no operation or write and can be confirmed explicitly",
  async () => {
    const { get, put, preview, state } = fixture();
    const initial = await get();
    const settings = { ...generalSettings(), logLevel: "debug" };
    assert.deepEqual(await (await preview({ settings, revision: initial.revision })).json(), {
      dns_restart_required: false,
    });
    state.runtimeActive = false;
    const rejected = await put({ settings, revision: initial.revision, allow_dns_restart: false });
    assert.equal(rejected.status, 409);
    assert.equal((await rejected.json()).code, "SETTINGS_RESTART_REQUIRED");
    assert.deepEqual(await get(), initial);
    assert.equal(state.writes, 0);
    assert.deepEqual(await (await preview({ settings, revision: initial.revision })).json(), {
      dns_restart_required: true,
    });
    assert.equal(
      (await put({ settings, revision: initial.revision, allow_dns_restart: true })).status,
      202,
    );
    assert.equal((await get()).operation.state, "succeeded");
    assert.equal(state.config.app.logLevel, "debug");
    assert.equal(state.runtimeActive, true);
    assert.equal(state.writes, 1);
  },
);

Deno.test("a prepared live request cannot escalate after its acknowledgement", async () => {
  for (const allow_dns_restart of [false, true]) {
    const { get, put, state } = fixture();
    const initial = await get();
    const settings = { ...generalSettings(), logLevel: "debug" };
    assert.equal(
      (await put({ settings, revision: initial.revision, allow_dns_restart })).status,
      202,
    );
    state.runtimeActive = false;
    const failed = await get();
    assert.equal(failed.operation.state, "failed");
    assert.deepEqual(failed.settings, initial.settings);
    assert.equal(failed.revision, initial.revision);
    assert.equal(state.writes, 0);
  }
});

Deno.test(
  "preview validates requests and revisions without bypassing a pending operation",
  async () => {
    const { app, get, put, preview, state } = fixture();
    const initial = await get();
    const payload = { settings: generalSettings(), revision: initial.revision };
    for (const body of [
      { ...payload, allow_dns_restart: true },
      { ...payload, settings: { ...payload.settings, logLevel: "verbose" } },
      { settings: payload.settings },
    ])
      assert.equal((await preview(body)).status, 400);
    assert.equal((await preview({ ...payload, revision: "stale" })).status, 409);
    assert.equal(
      (
        await app.request(`${BASE}/preview`, {
          method: "POST",
          headers: { "Content-Type": "text/plain" },
          body: JSON.stringify(payload),
        })
      ).status,
      400,
    );
    for (const value of [null, 0, "false"])
      assert.equal((await put({ ...payload, allow_dns_restart: value })).status, 400);
    const settings = { ...payload.settings, logLevel: "debug" };
    assert.equal(
      (await put({ settings, revision: initial.revision, allow_dns_restart: false })).status,
      202,
    );
    const blocked = await preview({ settings, revision: initial.revision });
    assert.equal(blocked.status, 409);
    assert.equal((await blocked.json()).code, "SETTINGS_BUSY");
    assert.equal(state.writes, 0);
    assert.equal((await get()).operation.state, "succeeded");
  },
);

Deno.test(
  "settings mock exposes only the typed General projection with explicit units",
  async () => {
    const { app, get, state } = fixture();
    const response = await app.request(BASE);
    assert.equal(response.headers.get("Cache-Control"), "no-store");
    const initial = await response.json();
    assert.deepEqual(initial.settings, generalSettings());
    assert.match(initial.revision, /^[0-9a-f]{64}$/);
    assert.equal(initial.operation, null);
    state.config.app.dnsProxy = { ...(state.config.app.dnsProxy as object), timeout: "1m500ms" };
    state.config.app.netfilter = {
      ...(state.config.app.netfilter as object),
      ipset: { tablePrefix: "keep_", additionalTTL: "1h30m" },
    };
    const converted = await get();
    assert.equal(converted.settings.dnsProxy.timeoutMs, 60500);
    assert.equal(converted.settings.netfilter.ipset.additionalTTLSeconds, 5400);
    assert.equal(state.writes, 0);
  },
);

Deno.test(
  "settings mock applies asynchronously to the latest model and retains all unowned state",
  async () => {
    const { get, put, state } = fixture();
    const initial = await get();
    const settings = generalSettings();
    settings.dnsProxy.timeoutMs = 7500;
    settings.netfilter.ipset.additionalTTLSeconds = 0;
    settings.logLevel = "debug";
    const accepted = await put({ settings, revision: initial.revision });
    assert.equal(accepted.status, 202);
    const ack = await accepted.json();
    assert.equal(ack.operation.state, "applying");
    assert.match(ack.operation.id, /^[0-9a-f]{32}$/);
    assert.equal(ack.operation.target_revision, ack.target_revision);
    assert.deepEqual(ack.next_web, {
      enabled: true,
      address: "0.0.0.0",
      port: 8080,
      auth_enabled: false,
    });
    assert.equal(state.writes, 0);
    assert.equal((await put({ settings, revision: initial.revision })).status, 409);

    state.config.groups[0].name = "Changed after ACK";
    state.config.profiles![0].name = "Latest profile";
    state.config.subscriptions[0].last_update = 456;
    state.config.subscriptions[0].rules!.push({
      id: "ffeeddcc",
      type: "domain",
      rule: "new.example",
      enable: true,
    });
    state.config.app.keeneticPolicy = { enabled: true, policyId: "Policy2", mode: "exclude" };
    const latest = structuredClone(state.config);
    const applied = await get();
    assert.deepEqual(applied.settings, settings);
    assert.equal(applied.revision, ack.target_revision);
    assert.equal(applied.operation.id, ack.operation.id);
    assert.equal(applied.operation.state, "succeeded");
    assert.deepEqual(state.config.groups, latest.groups);
    assert.deepEqual(state.config.profiles, latest.profiles);
    assert.deepEqual(state.config.subscriptions, latest.subscriptions);
    assert.deepEqual(state.config.app.keeneticPolicy, latest.app.keeneticPolicy);
    const http = state.config.app.httpWeb as Record<string, unknown>;
    const netfilter = state.config.app.netfilter as Record<string, unknown>;
    assert.equal(http.skin, "retained-skin");
    assert.deepEqual(netfilter.iptables, { chainPrefix: "KEEP_" });
    assert.equal((netfilter.ipset as Record<string, unknown>).tablePrefix, "keep_");
    assert.equal(netfilter.startMarkTableIndex, 1298220905);
    assert.equal(state.writes, 1);
  },
);

Deno.test(
  "settings revisions ignore unrelated writes but reject a stale General draft",
  async () => {
    const { get, put, state } = fixture();
    const initial = await get();
    state.config.groups[0].name = "Saved group";
    assert.equal((await get()).revision, initial.revision);
    const settings = { ...generalSettings(), logLevel: "debug" };
    assert.equal((await put({ settings, revision: initial.revision })).status, 202);
    const applied = await get();
    assert.notEqual(applied.revision, initial.revision);
    const conflict = await put({
      settings: { ...settings, logLevel: "trace" },
      revision: initial.revision,
    });
    assert.equal(conflict.status, 409);
    assert.equal((await conflict.json()).code, "SETTINGS_CONFLICT");
    assert.equal(state.config.app.logLevel, "debug");
    assert.equal(state.config.groups[0].name, "Saved group");
    assert.equal(state.writes, 1);
  },
);

Deno.test("a General change made before the queued apply is not overwritten", async () => {
  const { get, put, state } = fixture();
  const initial = await get();
  const settings = { ...generalSettings(), logLevel: "debug" };
  const ack = await (await put({ settings, revision: initial.revision })).json();
  state.config.app.logLevel = "warn";
  const result = await get();
  assert.equal(result.operation.id, ack.operation.id);
  assert.equal(result.operation.state, "failed");
  assert.equal(result.settings.logLevel, "warn");
  assert.equal(state.config.app.logLevel, "warn");
  assert.equal(state.writes, 0);
});

Deno.test("a failed mock apply keeps the active settings and permits a later retry", async () => {
  const { get, put, state } = fixture();
  const initial = await get();
  const settings = { ...generalSettings(), logLevel: "debug" };
  state.fail = true;
  await put({ settings, revision: initial.revision });
  const failed = await get();
  assert.equal(failed.operation.state, "failed");
  assert.equal(failed.operation.error, "Cannot persist general settings");
  assert.deepEqual(failed.settings, initial.settings);
  assert.equal(failed.revision, initial.revision);
  assert.equal(state.writes, 0);
  state.fail = false;
  assert.equal((await put({ settings, revision: failed.revision })).status, 202);
  assert.equal((await get()).operation.state, "succeeded");
  assert.equal(state.config.app.logLevel, "debug");
});

Deno.test("a no-op settings save succeeds without restarting or writing", async () => {
  const { get, put, state } = fixture();
  const initial = await get();
  const response = await put({ settings: initial.settings, revision: initial.revision });
  assert.equal(response.status, 200);
  const ack = await response.json();
  assert.equal(ack.operation.state, "succeeded");
  assert.equal(ack.target_revision, initial.revision);
  assert.equal((await get()).operation.state, "succeeded");
  assert.equal(state.writes, 0);
});

Deno.test(
  "invalid projections and bounds cannot change the model or start an operation",
  async () => {
    const { get, put, state } = fixture();
    const initial = await get();
    const settings = generalSettings();
    const invalid = [
      null,
      {},
      { ...settings, groups: [] },
      { ...settings, keeneticPolicy: { enabled: true } },
      { ...settings, httpWeb: { ...settings.httpWeb, skin: "other" } },
      { ...settings, netfilter: { ...settings.netfilter, startMarkTableIndex: 0 } },
      {
        ...settings,
        netfilter: {
          ...settings.netfilter,
          ipset: { additionalTTLSeconds: 0, tablePrefix: "other_" },
        },
      },
      { ...settings, httpWeb: { ...settings.httpWeb, enabled: "true" } },
      {
        ...settings,
        httpWeb: { ...settings.httpWeb, host: { address: "router.example", port: 8080 } },
      },
      { ...settings, httpWeb: { ...settings.httpWeb, host: { address: "0.0.0.0", port: 0 } } },
      { ...settings, httpWeb: { ...settings.httpWeb, host: { address: "0.0.0.0", port: 65536 } } },
      { ...settings, dnsProxy: { ...settings.dnsProxy, timeoutMs: 0 } },
      { ...settings, dnsProxy: { ...settings.dnsProxy, timeoutMs: 4294967296 } },
      { ...settings, dnsProxy: { ...settings.dnsProxy, maxConcurrent: 0 } },
      { ...settings, dnsProxy: { ...settings.dnsProxy, maxIdleConns: -1 } },
      { ...settings, dnsProxy: { ...settings.dnsProxy, maxConcurrent: 1.5 } },
      { ...settings, netfilter: { ...settings.netfilter, ipset: { additionalTTLSeconds: -1 } } },
      { ...settings, link: ["br0", "br0"] },
      { ...settings, link: ["interface-too-long"] },
      { ...settings, link: ["bad/name"] },
      { ...settings, logLevel: "verbose" },
    ];
    for (const value of invalid) {
      const response = await put({ settings: value, revision: initial.revision });
      assert.equal(response.status, 400, JSON.stringify(value));
      assert.equal((await response.json()).code, "SETTINGS_INVALID");
    }
    assert.equal((await put({ settings, revision: initial.revision, groups: [] })).status, 400);
    assert.deepEqual(await get(), initial);
    assert.equal(state.writes, 0);
  },
);

Deno.test("settings mock accepts all documented unsigned boundaries", async () => {
  const { get, put } = fixture();
  const initial = await get();
  const settings = generalSettings();
  settings.httpWeb.host.port = 65535;
  settings.dnsProxy.host.port = 1;
  settings.dnsProxy.maxIdleConns = 0;
  settings.dnsProxy.maxConcurrent = 4294967295;
  settings.dnsProxy.timeoutMs = 4294967295;
  settings.netfilter.ipset.additionalTTLSeconds = 4294967295;
  assert.equal((await put({ settings, revision: initial.revision })).status, 202);
  assert.deepEqual((await get()).settings, settings);
});

Deno.test("malformed or cross-site requests do not start a mock operation", async () => {
  const { app, get } = fixture();
  const initial = await get();
  const body = JSON.stringify({ settings: initial.settings, revision: initial.revision });
  for (const options of [
    { body, headers: {} },
    { body: "{", headers: { "Content-Type": "application/json" } },
    { body, headers: { "Content-Type": "application/json", "Sec-Fetch-Site": "cross-site" } },
  ])
    assert.equal((await app.request(BASE, { method: "PUT", ...options })).status, 400);
  assert.deepEqual(await get(), initial);
});
