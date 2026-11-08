import { strict as assert } from "node:assert";

import type { PolicySettings, Status } from "../../src/modules/settings/keenetic/state.svelte";

import { installSvelteRunesMocks } from "../mocks/setup-svelte-runes";

installSvelteRunesMocks();
const { fetchPolicySettings, policy, policyDirty } =
  await import("../../src/modules/settings/keenetic/state.svelte");
const { fetcher } = await import("../../src/utils/fetcher");

const original: PolicySettings = { enabled: true, policy_id: "original", mode: "include" };
function status(settings: PolicySettings, state: Status["runtime"]["state"] = "ready"): Status {
  return { settings, runtime: { state } };
}
function seed() {
  Object.assign(policy, {
    draft: { ...original },
    baseline: JSON.stringify(original),
    runtime: { state: "ready" },
    loaded: true,
    loading: false,
    failed: false,
    saving: false,
    persistencePending: false,
  });
}
async function withGet(response: () => Promise<unknown>, run: () => Promise<void>) {
  const previous = fetcher.get;
  fetcher.get = <T>() => response() as Promise<T>;
  try {
    await run();
  } finally {
    fetcher.get = previous;
  }
}

Deno.test("fresh policy refresh ignores a pending pre-YAML response", async () => {
  seed();
  const stale = Promise.withResolvers<unknown>();
  const fresh = Promise.withResolvers<unknown>();
  const confirmed: PolicySettings = { enabled: true, policy_id: "from-yaml", mode: "exclude" };
  let requests = 0;
  await withGet(
    () => (++requests === 1 ? stale.promise : fresh.promise),
    async () => {
      const pending = fetchPolicySettings(true);
      const refresh = fetchPolicySettings(false, { fresh: true });
      assert.equal(requests, 1);
      stale.resolve(status({ enabled: false, policy_id: "stale", mode: "include" }, "disabled"));
      await pending;
      assert.equal(requests, 2);
      assert.deepEqual(policy.draft, original);
      assert.equal(policy.baseline, JSON.stringify(original));
      assert.deepEqual(policy.runtime, { state: "ready" });

      fresh.resolve(status(confirmed, "applying"));
      await refresh;
    },
  );
  assert.deepEqual(policy.draft, confirmed);
  assert.equal(policy.baseline, JSON.stringify(confirmed));
  assert.equal(policy.runtime.state, "applying");
  assert.equal(policyDirty(), false);
  assert.equal(policy.failed, false);
  assert.equal(policy.loading, false);
});

Deno.test("policy edits made during a fresh refresh retain their draft and baseline", async () => {
  seed();
  const response = Promise.withResolvers<unknown>();
  await withGet(
    () => response.promise,
    async () => {
      const refresh = fetchPolicySettings(false, { fresh: true });
      policy.draft.policy_id = "local-unsaved";
      policy.draft.mode = "exclude";
      response.resolve(
        status({ enabled: false, policy_id: "from-yaml", mode: "include" }, "disabled"),
      );
      await refresh;
    },
  );
  assert.deepEqual(policy.draft, { ...original, policy_id: "local-unsaved", mode: "exclude" });
  assert.equal(policy.baseline, JSON.stringify(original));
  assert.equal(policy.runtime.state, "disabled");
  assert.equal(policyDirty(), true);
  assert.equal(policy.failed, false);
  assert.equal(policy.loading, false);
});

Deno.test(
  "a failed forced policy refresh is observable without discarding loaded settings",
  async () => {
    for (const response of [
      async () => {
        throw new Error("Connection lost");
      },
      async () => ({ settings: original, runtime: { state: "invalid" } }),
    ]) {
      seed();
      policy.draft.policy_id = "local-unsaved";
      const draft = structuredClone(policy.draft);
      const runtime = structuredClone(policy.runtime);
      await withGet(response, async () => {
        await fetchPolicySettings(false, { fresh: true });
      });
      assert.equal(policy.failed, true);
      assert.equal(policy.loaded, true);
      assert.equal(policy.loading, false);
      assert.deepEqual(policy.draft, draft);
      assert.equal(policy.baseline, JSON.stringify(original));
      assert.deepEqual(policy.runtime, runtime);

      await withGet(
        async () => status(original),
        async () => {
          await fetchPolicySettings(false, { fresh: true });
        },
      );
      assert.equal(policy.failed, false);
      assert.deepEqual(policy.draft, draft);
      assert.equal(policyDirty(), true);
    }
  },
);
