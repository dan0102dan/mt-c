import { strict as assert } from "node:assert";

import type { Group, Subscription } from "../../src/types";
import { installSvelteRunesMocks } from "../mocks/setup-svelte-runes";

installSvelteRunesMocks();
const storage = Object.getOwnPropertyDescriptor(globalThis, "localStorage");
Object.defineProperty(globalThis, "localStorage", {
  configurable: true,
  value: { getItem: () => null, setItem: () => {}, removeItem: () => {} },
});
const runtimeProcess = (globalThis as typeof globalThis & { process?: { env?: unknown } }).process;
const environment = runtimeProcess && Object.getOwnPropertyDescriptor(runtimeProcess, "env");
if (runtimeProcess)
  Object.defineProperty(runtimeProcess, "env", { configurable: true, value: { NODE_ENV: "test" } });
const { GroupsStore } = await import("../../src/modules/groups/groups.svelte");
const { SubscriptionsStore } = await import("../../src/modules/subscriptions/subscriptions.svelte");
const { fetcher } = await import("../../src/utils/fetcher");
if (runtimeProcess && environment) Object.defineProperty(runtimeProcess, "env", environment);
else if (runtimeProcess) Reflect.deleteProperty(runtimeProcess, "env");
if (storage) Object.defineProperty(globalThis, "localStorage", storage);
else Reflect.deleteProperty(globalThis, "localStorage");

const group = (): Group => ({
  id: "11223344",
  name: "Original",
  color: "#ffffff",
  interface: "blackhole",
  enable: true,
  priority: 300,
  rules: [{ id: "aabbccdd", name: "", type: "namespace", rule: "example.test", enable: true }],
});
const subscription = (): Subscription => ({
  id: "22334455",
  name: "Original",
  interface: "blackhole",
  enable: true,
  priority: 100,
  url: "https://example.test/list",
  interval: 86400,
  lastUpdate: 1,
  rules: [{ id: "aabbccdd", type: "namespace", rule: "example.test", enable: true }],
});

Deno.test(
  "native rule insertion supersedes a pending load only after a successful commit",
  async () => {
    const originalGet = fetcher.get;
    for (const cancel of [false, true]) {
      const store = new GroupsStore();
      let resolveLoad!: (value: unknown) => void;
      fetcher.get = (() =>
        new Promise((resolve) => {
          resolveLoad = resolve;
        })) as typeof fetcher.get;
      try {
        store.stageImport([group()], [group()]);
        const mounting = store.mount();
        const controller = new AbortController();
        if (cancel) controller.abort();
        const inserting = store.addRulesToGroup(
          0,
          [{ id: "abcdef01", name: "", type: "namespace", rule: "imported.test", enable: true }],
          { signal: controller.signal },
        );
        if (cancel) await assert.rejects(inserting, { name: "AbortError" });
        else await inserting;
        resolveLoad({ groups: [{ ...group(), name: "Fresh server" }] });
        await mounting;
        assert.equal(store.tracker.data[0].name, cancel ? "Fresh server" : "Original");
        assert.equal(store.tracker.data[0].rules.length, cancel ? 1 : 2);
        assert.equal(store.tracker.isDirty, !cancel);
      } finally {
        fetcher.get = originalGet;
        store.destroy();
      }
    }
  },
);

for (const kind of ["groups", "subscriptions"] as const) {
  Deno.test(
    `${kind}: preparation publishes nothing until commit and retains reversibility`,
    async () => {
      const store = kind === "groups" ? new GroupsStore() : new SubscriptionsStore();
      try {
        if (kind === "groups") {
          const live = store as InstanceType<typeof GroupsStore>;
          const saved = [group()],
            draft = [group()];
          draft[0].rules[0].enable = false;
          live.stageImport(saved, saved);
          const published = live.tracker;
          const pending = live.prepareImport(saved, draft);
          assert.strictEqual(live.tracker, published);
          assert.equal(published.data[0].rules[0].enable, true);
          const prepared = await pending;
          assert.strictEqual(live.tracker, published);
          prepared.commit();
          assert.notStrictEqual(live.tracker, published);
          assert.equal(live.tracker.data[0].rules[0].enable, false);
          assert.equal(live.tracker.isDirty, true);
          live.tracker.data[0].rules[0].enable = true;
          assert.equal(live.tracker.isDirty, false);
          assert.equal(saved[0].rules[0].enable, true);
          assert.equal(draft[0].rules[0].enable, false);
        } else {
          const live = store as InstanceType<typeof SubscriptionsStore>;
          const saved = [subscription()],
            draft = [subscription()];
          draft[0].rules[0].enable = false;
          live.stageImport(saved, saved);
          const published = live.tracker;
          const pending = live.prepareImport(saved, draft);
          assert.strictEqual(live.tracker, published);
          const prepared = await pending;
          assert.strictEqual(live.tracker, published);
          prepared.commit();
          assert.notStrictEqual(live.tracker, published);
          assert.equal(live.tracker.data[0].rules[0].enable, false);
          assert.equal(live.tracker.isDirty, true);
          live.tracker.data[0].rules[0].enable = true;
          assert.equal(live.tracker.isDirty, false);
          assert.equal(draft[0].rules[0].enable, false);
        }
      } finally {
        store.destroy();
      }
    },
  );

  Deno.test(`${kind}: a completed background load survives abandoned preparation`, async () => {
    const originalGet = fetcher.get;
    const store = kind === "groups" ? new GroupsStore() : new SubscriptionsStore();
    let resolveLoad!: (value: unknown) => void;
    const result = new Promise((resolve) => {
      resolveLoad = resolve;
    });
    fetcher.get = (() => result) as typeof fetcher.get;
    try {
      const mounting = store.mount();
      const prepared =
        kind === "groups"
          ? await (store as InstanceType<typeof GroupsStore>).prepareImport(
              [group()],
              [{ ...group(), name: "Imported" }],
            )
          : await (store as InstanceType<typeof SubscriptionsStore>).prepareImport(
              [subscription()],
              [{ ...subscription(), name: "Imported" }],
            );
      assert.equal(store.dataLoaded, false);
      resolveLoad(
        kind === "groups"
          ? { groups: [{ ...group(), name: "Fresh server" }] }
          : { subscriptions: [{ ...subscription(), name: "Fresh server" }] },
      );
      await mounting;
      assert.equal(store.dataLoaded, true);
      assert.equal(store.tracker.data[0].name, "Fresh server");
      assert.throws(prepared.validate, /Configuration changed/);
      assert.throws(prepared.commit, /Configuration changed/);
      assert.equal(store.tracker.data[0].name, "Fresh server");
    } finally {
      fetcher.get = originalGet;
      store.destroy();
    }
  });

  Deno.test(`${kind}: changes to the live draft invalidate prepared commit`, async () => {
    const store = kind === "groups" ? new GroupsStore() : new SubscriptionsStore();
    try {
      const prepared =
        kind === "groups"
          ? await (store as InstanceType<typeof GroupsStore>).prepareImport(
              [group()],
              [{ ...group(), name: "Imported" }],
            )
          : await (store as InstanceType<typeof SubscriptionsStore>).prepareImport(
              [subscription()],
              [{ ...subscription(), name: "Imported" }],
            );
      const original = store.tracker;
      store.markDataRevision();
      assert.throws(prepared.validate, /Configuration changed/);
      assert.throws(prepared.commit, /Configuration changed/);
      assert.strictEqual(store.tracker, original);
    } finally {
      store.destroy();
    }
  });

  Deno.test(`${kind}: identical preparation stays clean and the first commit wins`, async () => {
    const store = kind === "groups" ? new GroupsStore() : new SubscriptionsStore();
    try {
      const prepare = () =>
        kind === "groups"
          ? (store as InstanceType<typeof GroupsStore>).prepareImport([group()], [group()])
          : (store as InstanceType<typeof SubscriptionsStore>).prepareImport(
              [subscription()],
              [subscription()],
            );
      const first = await prepare();
      const competing = await prepare();
      first.commit();
      const committed = store.tracker;
      assert.equal(committed.isDirty, false);
      assert.equal(committed.data[0].name, "Original");
      assert.throws(first.commit, /Configuration changed/);
      assert.throws(competing.validate, /Configuration changed/);
      assert.throws(competing.commit, /Configuration changed/);
      assert.strictEqual(store.tracker, committed);
    } finally {
      store.destroy();
    }
  });

  Deno.test(`${kind}: committed preparation supersedes a pending server response`, async () => {
    const originalGet = fetcher.get;
    const store = kind === "groups" ? new GroupsStore() : new SubscriptionsStore();
    let resolveLoad!: (value: unknown) => void;
    const result = new Promise((resolve) => {
      resolveLoad = resolve;
    });
    fetcher.get = (() => result) as typeof fetcher.get;
    try {
      const mounting = store.mount();
      const prepared =
        kind === "groups"
          ? await (store as InstanceType<typeof GroupsStore>).prepareImport(
              [group()],
              [{ ...group(), name: "Imported" }],
            )
          : await (store as InstanceType<typeof SubscriptionsStore>).prepareImport(
              [subscription()],
              [{ ...subscription(), name: "Imported" }],
            );
      prepared.commit();
      const committed = store.tracker;
      resolveLoad(
        kind === "groups"
          ? { groups: [{ ...group(), name: "Old server" }] }
          : { subscriptions: [{ ...subscription(), name: "Old server" }] },
      );
      await mounting;
      assert.strictEqual(store.tracker, committed);
      assert.equal(store.tracker.data[0].name, "Imported");
      assert.equal(store.tracker.isDirty, true);
      assert.equal(store.dataLoaded, true);
    } finally {
      fetcher.get = originalGet;
      store.destroy();
    }
  });
}

for (const kind of ["groups", "subscriptions"] as const) {
  Deno.test(
    `${kind}: accepted PCRE baseline survives metadata and staged imports but not pattern edits`,
    async () => {
      const store = kind === "groups" ? new GroupsStore() : new SubscriptionsStore();
      const saved = { ...subscription(), ...group() };
      saved.rules[0].type = "regex";
      saved.rules[0].rule = kind === "groups" ? "(?i)EXAMPLE" : "DOMAIN-REGEX,(?i)EXAMPLE";
      try {
        store.stageImport([saved], [saved]);
        store.checkRulesValidityState();
        assert.equal(store.valid_rules, true);
        store.data[0].name = "Metadata";
        store.checkRulesValidityState();
        assert.equal(store.valid_rules, true);
        store.data[0].rules[0].rule += "(";
        store.checkRulesValidityState();
        assert.equal(store.valid_rules, false);
        store.data[0].rules[0].rule = saved.rules[0].rule;
        store.data[0].rules[0].id = "new-rule";
        store.checkRulesValidityState();
        assert.equal(store.valid_rules, false);
        const prepared = await store.prepareImport(
          [saved],
          [{ ...saved, name: "Imported metadata" }],
        );
        prepared.commit();
        assert.equal(store.valid_rules, true);
      } finally {
        store.destroy();
      }
    },
  );
}
