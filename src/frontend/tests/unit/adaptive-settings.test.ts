import { strict as assert } from "node:assert";

import {
  adaptiveDefaults,
  adaptiveDraft,
  buildAdaptiveSettings,
  readAdaptiveStatus,
} from "../../src/modules/settings/adaptive/data";
import { HttpError } from "../../src/utils/http-error";
import { adaptiveStatus } from "../mocks/adaptive-settings";
import { installSvelteRunesMocks } from "../mocks/setup-svelte-runes";

installSvelteRunesMocks();
Object.assign($state, { raw: <T>(value: T): T => value });
const { AdaptiveSettingsState } = await import("../../src/modules/settings/adaptive/state.svelte");
const { fetcher } = await import("../../src/utils/fetcher");

async function withApi(run: () => Promise<void>) {
  const methods = { get: fetcher.get, put: fetcher.put, delete: fetcher.delete };
  try {
    await run();
  } finally {
    Object.assign(fetcher, methods);
  }
}

async function loaded() {
  const state = new AdaptiveSettingsState();
  fetcher.get = <T>() => Promise.resolve(adaptiveStatus() as T);
  await state.load();
  return state;
}
function enable(state: InstanceType<typeof AdaptiveSettingsState>) {
  state.draft.enabled = true;
  state.draft.interface = "wg0";
  state.draft.ingressInterfaces = ["br-lan"];
}

Deno.test(
  "adaptive settings require explicit client scope and reject ambiguous interface roles",
  () => {
    assert.deepEqual(
      buildAdaptiveSettings(adaptiveDraft(adaptiveDefaults)).settings,
      adaptiveDefaults,
    );
    const draft = adaptiveDraft(adaptiveDefaults);
    draft.enabled = true;
    assert.deepEqual(Object.keys(buildAdaptiveSettings(draft).errors), [
      "interface",
      "ingressInterfaces",
    ]);
    draft.interface = "wg0";
    for (const ingress of [
      ["wg0"],
      ["br-lan", "br-lan"],
      ["lo"],
      ["blackhole"],
      ["invalid name"],
      ["-br0"],
      Array.from({ length: 17 }, (_, i) => `br${i}`),
    ]) {
      draft.ingressInterfaces = ingress;
      assert.ok(buildAdaptiveSettings(draft).errors.ingressInterfaces);
    }
    draft.ingressInterfaces = ["br-lan"];
    assert.ok(buildAdaptiveSettings(draft).settings);
    draft.interface = "a".repeat(16);
    assert.ok(buildAdaptiveSettings(draft).errors.interface);
  },
);

Deno.test(
  "adaptive numeric validation preserves resource limits and rejects empty/fractional values",
  () => {
    const draft = adaptiveDraft({
      ...adaptiveDefaults,
      maxConnections: 16,
      maxEntries: 80,
      idleTimeoutMs: 8000,
    });
    for (const value of ["", "0", "99", "60001", "1.5", "1e3", "NaN"]) {
      draft.connectTimeoutMs = value;
      assert.ok(buildAdaptiveSettings(draft).errors.connectTimeoutMs);
    }
    draft.connectTimeoutMs = "100";
    draft.tlsTimeoutMs = "60000";
    draft.ttlSeconds = "29";
    assert.ok(buildAdaptiveSettings(draft).errors.ttlSeconds);
    draft.ttlSeconds = "86400";
    const result = buildAdaptiveSettings(draft).settings!;
    assert.equal(result.maxConnections, 16);
    assert.equal(result.maxEntries, 80);
    assert.equal(result.idleTimeoutMs, 8000);
    assert.notEqual(result.ingressInterfaces, draft.ingressInterfaces);
  },
);

Deno.test("adaptive response validation rejects malformed and duplicate metadata", () => {
  const response = adaptiveStatus();
  assert.deepEqual(readAdaptiveStatus(response), response);
  assert.throws(() => readAdaptiveStatus({ ...response, active: "yes" }));
  assert.throws(() =>
    readAdaptiveStatus({ ...response, interfaces: [{ id: "br-lan" }, { id: "br-lan" }] }),
  );
  assert.throws(() =>
    readAdaptiveStatus({
      ...response,
      entries: [
        { address: "203.0.113.2", family: 4, port: 22, alternate: true, expiresInSeconds: 20 },
      ],
    }),
  );
  assert.throws(() =>
    readAdaptiveStatus({ ...response, settings: { ...response.settings, maxConnections: 1025 } }),
  );
});

Deno.test("adaptive initial load is reused and status refresh keeps unsaved edits", async () =>
  withApi(async () => {
    const state = await loaded();
    let reads = 0;
    const next = adaptiveStatus();
    next.stats.accepted = 3;
    fetcher.get = <T>() => {
      reads++;
      return Promise.resolve(next as T);
    };
    await state.load();
    assert.equal(reads, 0);
    enable(state);
    await state.load(true);
    assert.equal(reads, 1);
    assert.equal(state.snapshot?.stats.accepted, 3);
    assert.equal(state.draft.enabled, true);
    assert.equal(state.snapshot?.settings.enabled, false);
    assert.equal(state.dirty, true);
  }),
);

Deno.test("adaptive save confirms canonical settings and blocks duplicate requests", async () =>
  withApi(async () => {
    const state = await loaded();
    enable(state);
    const gate = Promise.withResolvers<unknown>();
    let writes = 0;
    let sent: unknown;
    fetcher.put = <T>(_path: string, body: unknown) => {
      writes++;
      sent = body;
      return gate.promise as Promise<T>;
    };
    const saving = state.save();
    await state.save();
    assert.equal(writes, 1);
    assert.equal(state.saving, true);
    const confirmed = adaptiveStatus();
    confirmed.settings = state.built.settings!;
    confirmed.active = true;
    assert.deepEqual(sent, { settings: confirmed.settings });
    gate.resolve(confirmed);
    await saving;
    assert.equal(state.snapshot?.active, true);
    assert.equal(state.dirty, false);
    assert.equal(state.saving, false);
  }),
);

Deno.test(
  "failed adaptive activation preserves draft and does not claim an active runtime",
  async () =>
    withApi(async () => {
      const state = await loaded();
      enable(state);
      fetcher.put = () =>
        Promise.reject(
          new HttpError(
            503,
            JSON.stringify({
              ...adaptiveStatus(),
              code: "TPROXY_UNAVAILABLE",
              error: "TPROXY is not available",
            }),
          ),
        );
      await assert.rejects(state.save());
      assert.equal(state.snapshot?.active, false);
      assert.equal(state.snapshot?.settings.enabled, false);
      assert.equal(state.draft.enabled, true);
      assert.equal(state.dirty, true);
      assert.equal(state.canSave, true);
      assert.ok(state.saveError);
    }),
);

Deno.test("failed rollback updates the runtime without accepting the rejected draft", async () =>
  withApi(async () => {
    const state = new AdaptiveSettingsState();
    const previous = adaptiveStatus();
    previous.settings = {
      ...previous.settings,
      enabled: true,
      interface: "wg0",
      ingressInterfaces: ["br-lan"],
    };
    previous.active = true;
    fetcher.get = <T>() => Promise.resolve(previous as T);
    await state.load();
    state.draft.interface = "wan0";
    fetcher.put = () =>
      Promise.reject(
        new HttpError(
          503,
          JSON.stringify({
            ...previous,
            active: false,
            error: "Restoring previous routing also failed",
            code: "TPROXY_UNAVAILABLE",
          }),
        ),
      );
    await assert.rejects(state.save());
    assert.equal(state.snapshot?.active, false);
    assert.equal(state.snapshot?.settings.interface, "wg0");
    assert.equal(state.draft.interface, "wan0");
    assert.equal(state.dirty, true);
    assert.equal(state.canSave, true);
  }),
);

Deno.test("an unconfirmed request requires explicit refresh while retaining the draft", async () =>
  withApi(async () => {
    const state = await loaded();
    enable(state);
    fetcher.put = () => Promise.reject(new Error("Connection closed"));
    await assert.rejects(state.save());
    assert.equal(state.failed, true);
    assert.equal(state.canSave, false);
    assert.equal(state.dirty, true);
    await state.load(true);
    assert.equal(state.failed, false);
    assert.equal(state.canSave, true);
    assert.equal(state.draft.enabled, true);
  }),
);

Deno.test(
  "adaptive persistence failure requires a complete applied acknowledgement and leaves Save available",
  async () =>
    withApi(async () => {
      const state = await loaded();
      enable(state);
      const applied = adaptiveStatus();
      applied.settings = state.built.settings!;
      applied.active = true;
      fetcher.put = () =>
        Promise.reject(
          new HttpError(
            500,
            JSON.stringify({
              ...applied,
              error: "Disk full",
              code: "PERSISTENCE_FAILED",
              applied: true,
            }),
          ),
        );
      await assert.rejects(state.save());
      assert.equal(state.snapshot?.active, true);
      assert.equal(state.persistencePending, true);
      assert.equal(state.canSave, true);
      fetcher.put = <T>() => Promise.resolve(applied as T);
      await state.save();
      assert.equal(state.persistencePending, false);
      assert.equal(state.dirty, false);
    }),
);

Deno.test("retrying inactive saved settings does not manufacture an unsaved draft", async () =>
  withApi(async () => {
    const state = new AdaptiveSettingsState();
    const saved = adaptiveStatus();
    saved.settings = {
      ...saved.settings,
      enabled: true,
      interface: "wg0",
      ingressInterfaces: ["br-lan"],
    };
    saved.error = "TPROXY support is unavailable";
    fetcher.get = <T>() => Promise.resolve(saved as T);
    await state.load();
    assert.equal(state.dirty, false);
    assert.equal(state.canSave, false);
    assert.equal(state.canRetryActivation, true);
    let sent: unknown;
    fetcher.put = <T>(_path: string, body: unknown) => {
      sent = body;
      return Promise.resolve({ ...saved, active: true, error: null } as T);
    };
    await state.retryActivation();
    assert.deepEqual(sent, { settings: saved.settings });
    assert.equal(state.snapshot?.active, true);
    assert.equal(state.dirty, false);
    assert.equal(state.canRetryActivation, false);
  }),
);

Deno.test(
  "malformed acknowledgement and unrelated success cannot discard the adaptive draft",
  async () =>
    withApi(async () => {
      for (const response of ["malformed", "different"]) {
        const state = await loaded();
        enable(state);
        fetcher.put = <T>() =>
          response === "malformed"
            ? Promise.reject(
                new HttpError(
                  500,
                  JSON.stringify({
                    code: "PERSISTENCE_FAILED",
                    applied: true,
                    settings: state.built.settings,
                  }),
                ),
              )
            : Promise.resolve(adaptiveStatus() as T);
        await assert.rejects(state.save());
        assert.equal(state.draft.enabled, true);
        assert.equal(state.snapshot?.settings.enabled, false);
        assert.equal(state.persistencePending, false);
        assert.equal(state.dirty, true);
      }
    }),
);

Deno.test("clearing learned routes leaves edited settings and persistence retry intact", async () =>
  withApi(async () => {
    const state = await loaded();
    enable(state);
    state.persistencePending = true;
    fetcher.delete = <T>() => Promise.resolve(adaptiveStatus() as T);
    await state.clear();
    assert.equal(state.draft.interface, "wg0");
    assert.deepEqual(state.draft.ingressInterfaces, ["br-lan"]);
    assert.equal(state.persistencePending, true);
    assert.equal(state.canSave, true);
  }),
);

Deno.test("an unavailable adaptive API is retryable without automatic repeat requests", async () =>
  withApi(async () => {
    const state = new AdaptiveSettingsState();
    let reads = 0;
    fetcher.get = () => {
      reads++;
      return Promise.reject(new Error("Missing endpoint"));
    };
    await state.load();
    await state.load();
    assert.equal(reads, 1);
    assert.equal(state.failed, true);
    fetcher.get = <T>() => Promise.resolve(adaptiveStatus() as T);
    await state.load(true);
    assert.equal(state.failed, false);
    assert.ok(state.snapshot);
  }),
);

Deno.test("destroying the adaptive editor rejects a late initial response", async () =>
  withApi(async () => {
    const state = new AdaptiveSettingsState();
    const gate = Promise.withResolvers<unknown>();
    fetcher.get = <T>() => gate.promise as Promise<T>;
    const loading = state.load();
    state.destroy();
    gate.resolve(adaptiveStatus());
    await loading;
    assert.equal(state.snapshot, undefined);
  }),
);
