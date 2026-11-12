import { strict as assert } from "node:assert";

import type { Group, Profile, Subscription } from "../../src/types";
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
const { ChangeTracker } = await import("../../src/utils/change-tracker.svelte");
const { profilesWithUsage, snapshotFromServer } = await import("../../src/modules/config-transfer/adapter");
const {
  acceptProfileImport,
  fetchProfiles,
  profileDraftIds,
  profiles,
  profilesDirty,
  stageProfileImport,
} = await import("../../src/data/profiles.svelte");
const { fetcher } = await import("../../src/utils/fetcher");
const { overlay, toast } = await import("../../src/utils/events");
if (runtimeProcess && environment) Object.defineProperty(runtimeProcess, "env", environment);
else if (runtimeProcess) Reflect.deleteProperty(runtimeProcess, "env");
if (storage) Object.defineProperty(globalThis, "localStorage", storage);
else Reflect.deleteProperty(globalThis, "localStorage");

function group(id = "11223344", name = "Original"): Group {
  return {
    id,
    name,
    color: "#256e94",
    interface: "wg0",
    enable: true,
    priority: 300,
    rules: [
      { id: "aabbccdd", name: "", rule: "example.com", type: "namespace", enable: true },
      { id: "aabbccee", name: "Other", rule: "example.org", type: "domain", enable: true },
    ],
  };
}
function subscription(id = "22334455", name = "Original source"): Subscription {
  return {
    id,
    name,
    url: "https://example.com/list.txt",
    interface: "wg0",
    enable: true,
    priority: 100,
    interval: 86400,
    lastUpdate: 123,
    rules: [{ id: "ffeeddcc", rule: "example.com", type: "namespace", enable: true }],
  };
}
function profile(id = "vpn", name = "VPN"): Profile {
  return { id, name, interfaces: ["wg0", "wg1"], on_unavailable: "blackhole" };
}
const snapshot = <T>(value: T): T => JSON.parse(JSON.stringify(value));

async function withoutRequests(run: () => void | Promise<void>) {
  const originals = {
    get: fetcher.get,
    post: fetcher.post,
    put: fetcher.put,
    delete: fetcher.delete,
    info: toast.info,
    success: toast.success,
    error: toast.error,
    show: overlay.show,
    hide: overlay.hide,
  };
  const requests: string[] = [];
  for (const method of ["get", "post", "put", "delete"] as const) {
    fetcher[method] = (async (url: string) => {
      requests.push(`${method} ${url}`);
      throw new Error("Import drafts must not make requests");
    }) as (typeof fetcher)[typeof method];
  }
  toast.info = toast.success = toast.error = overlay.show = () => {};
  overlay.hide = () => {};
  try {
    await run();
    assert.deepEqual(requests, []);
  } finally {
    Object.assign(fetcher, {
      get: originals.get,
      post: originals.post,
      put: originals.put,
      delete: originals.delete,
    });
    toast.info = originals.info;
    toast.success = originals.success;
    toast.error = originals.error;
    overlay.show = originals.show;
    overlay.hide = originals.hide;
  }
}

Deno.test(
  "replacing same-ID groups and rules is a reversible draft, including nested order",
  () => {
    const baseline = [group()];
    const expectedBaseline = structuredClone(baseline);
    const tracker = new ChangeTracker(structuredClone(baseline));
    const imported = structuredClone(baseline);
    imported[0].name = "Imported";
    imported[0].rules[0].enable = false;
    imported[0].rules.reverse();

    tracker.replaceDraft(imported);
    assert.equal(tracker.isDirty, true);
    assert.deepEqual(tracker.dirtyRootIds, new Set(["11223344"]));
    assert.deepEqual(snapshot(tracker.data), imported);
    assert.deepEqual(baseline, expectedBaseline);

    tracker.replaceDraft(expectedBaseline);
    assert.equal(tracker.isDirty, false);
    assert.deepEqual(tracker.dirtyRootIds, new Set());
    assert.deepEqual(tracker.changes, { added: [], deleted: [], mutated: [] });
  },
);

Deno.test("replace can remove and restore roots or rules without losing field tracking", () => {
  const baseline = [group()];
  const tracker = new ChangeTracker(structuredClone(baseline));
  tracker.replaceDraft([]);
  assert.equal(tracker.isDirty, true);
  assert.deepEqual(tracker.dirtyRootIds, new Set(["11223344"]));
  tracker.replaceDraft(baseline);
  assert.equal(tracker.isDirty, false);

  const removedRule = structuredClone(baseline);
  removedRule[0].rules.shift();
  tracker.replaceDraft(removedRule);
  const restoredRule = structuredClone(baseline);
  restoredRule[0].rules[0].name = "Restored and edited";
  tracker.replaceDraft(restoredRule);
  assert.equal(tracker.isDirty, true);
  assert.equal(tracker.data[0].rules[0].name, "Restored and edited");
  tracker.data[0].rules[0].name = "";
  assert.equal(tracker.isDirty, false);
  tracker.data[0].rules[0].enable = false;
  assert.deepEqual(tracker.dirtyRootIds, new Set(["11223344"]));
});

Deno.test("unchanged replacement and repeated import do not create phantom dirty roots", () => {
  const baseline = [group()];
  const tracker = new ChangeTracker(structuredClone(baseline));
  tracker.replaceDraft(structuredClone(baseline));
  assert.equal(tracker.isDirty, false);
  tracker.data[0].name = "Edited after import";
  const saved = snapshot(tracker.data);
  tracker.reset(saved);
  assert.equal(tracker.isDirty, false);
  assert.deepEqual(tracker.dirtyRootIds, new Set());

  tracker.replaceDraft(baseline);
  assert.equal(tracker.data[0].name, "Original");
  assert.equal(tracker.isDirty, true);
  tracker.reset(structuredClone(baseline));
  tracker.replaceDraft(structuredClone(baseline));
  assert.equal(tracker.isDirty, false);
});

Deno.test(
  "group import stages detached drafts and accepts only confirmed saved values",
  async () => {
    await withoutRequests(() => {
      const store = new GroupsStore();
      const baseline = [group()];
      const imported = structuredClone(baseline);
      imported[0].rules[0].enable = false;
      store.persistencePending = true;
      store.stageImport(baseline, imported);

      assert.equal(store.tracker.isDirty, true);
      assert.equal(store.persistencePending, true);
      assert.equal(store.tracker.data[0].rules[0].enable, false);
      assert.equal(baseline[0].rules[0].enable, true);
      store.tracker.data[0].name = "Edited after import";
      assert.equal(imported[0].name, "Original");

      const saved = snapshot(store.tracker.data);
      store.acceptImport(saved);
      assert.equal(store.tracker.isDirty, false);
      assert.equal(store.persistencePending, false);
      store.stageImport(saved, baseline);
      assert.equal(store.tracker.data[0].name, "Original");
      assert.equal(store.tracker.isDirty, true);
      store.destroy();
    });
  },
);

Deno.test("confirmed group import preserves unrelated edits against the new baseline", async () => {
  await withoutRequests(() => {
    const store = new GroupsStore();
    const saved = [group(), { ...group("55667788", "Second"), rules: [] }];
    const draft = structuredClone(saved);
    draft[1].name = "Still unsaved";
    store.acceptImport(saved, draft);
    assert.deepEqual(store.tracker.dirtyRootIds, new Set(["55667788"]));
    store.tracker.data[1].name = "Second";
    assert.equal(store.tracker.isDirty, false);
    store.destroy();
  });
});

Deno.test("group staging keeps optimistic rule preconditions on the server baseline", async () => {
  const store = new GroupsStore();
  const baseline = [group()];
  const draft = structuredClone(baseline);
  draft[0].rules[0].enable = false;
  store.stageImport(baseline, draft);
  store.canSave = true;
  const oldPut = fetcher.put;
  const oldShow = overlay.show;
  const oldHide = overlay.hide;
  const oldSuccess = toast.success;
  let payload: unknown;
  fetcher.put = <T>(_url: string, value: unknown) => {
    payload = value;
    return Promise.resolve({ groups: structuredClone(draft) }) as Promise<T>;
  };
  overlay.show = overlay.hide = toast.success = () => {};
  try {
    await store.saveChanges();
    const groups = (
      payload as { groups: { ruleChanges: { enable: boolean; previous: { enable: boolean } }[] }[] }
    ).groups;
    assert.equal(groups[0].ruleChanges[0].previous.enable, true);
    assert.equal(groups[0].ruleChanges[0].enable, false);
    assert.equal(store.tracker.isDirty, false);
  } finally {
    fetcher.put = oldPut;
    overlay.show = oldShow;
    overlay.hide = oldHide;
    toast.success = oldSuccess;
    store.destroy();
  }
});

Deno.test(
  "pending import delegates group and subscription Save without separate mutations",
  async () => {
    await withoutRequests(async () => {
      for (const store of [new GroupsStore(), new SubscriptionsStore()]) {
        let saves = 0;
        store.importPending = true;
        store.delegateSave = async () => {
          saves++;
        };
        await store.saveChanges();
        assert.equal(saves, 1);
        store.delegateSave = undefined;
        await store.saveChanges();
        assert.equal(saves, 1);
        store.destroy();
      }
    });
  },
);

Deno.test(
  "Ctrl+S saves a pending import even when the visible editor itself is unchanged",
  async () => {
    const previousDocument = Object.getOwnPropertyDescriptor(globalThis, "document");
    Object.defineProperty(globalThis, "document", {
      configurable: true,
      value: { querySelector: () => null },
    });
    try {
      await withoutRequests(async () => {
        for (const store of [new GroupsStore(), new SubscriptionsStore()]) {
          let saves = 0;
          let prevented = false;
          store.importPending = true;
          store.canSave = false;
          store.delegateSave = async () => {
            saves++;
          };
          store.handleSaveShortcut({
            key: "s",
            ctrlKey: true,
            preventDefault() {
              prevented = true;
            },
          } as KeyboardEvent);
          await Promise.resolve();
          assert.equal(saves, 1);
          assert.equal(prevented, true);
          store.destroy();
        }
      });
    } finally {
      if (previousDocument) Object.defineProperty(globalThis, "document", previousDocument);
      else Reflect.deleteProperty(globalThis, "document");
    }
  },
);

Deno.test(
  "subscription draft add, delete and sync never persist an imported configuration",
  async () => {
    await withoutRequests(async () => {
      const store = new SubscriptionsStore();
      const baseline = [subscription()];
      const imported = [{ ...subscription(), name: "Imported source" }];
      store.stageImport(baseline, imported);
      store.importPending = true;
      assert.equal(store.tracker.isDirty, true);
      await store.syncSubscription(0);
      assert.equal(store.tracker.data[0].lastUpdate, 123);
      await store.addSubscription({
        name: "New source",
        url: " https://example.org/another.txt ",
        interface: "wg1",
        interval: 3600,
      });
      assert.equal(store.tracker.data.length, 2);
      assert.equal(store.tracker.data[0].url, "https://example.org/another.txt");
      assert.deepEqual(snapshot(store.tracker.data[0].rules), []);
      await store.deleteSubscription(1, true);
      assert.equal(store.tracker.data.length, 1);
      assert.equal(store.tracker.data[0].name, "New source");
      assert.equal(baseline.length, 1);
      assert.equal(baseline[0].name, "Original source");
      store.acceptImport(snapshot(store.tracker.data));
      assert.equal(store.tracker.isDirty, false);
      store.destroy();
    });
  },
);

Deno.test("absent and empty subscription overrides do not leave a confirmed import dirty", () => {
  const store = new SubscriptionsStore();
  const baseline = [{ ...subscription(), overrides: [] }];
  const draft = [subscription()];
  store.stageImport(baseline, draft);
  assert.equal(store.tracker.isDirty, false);
  assert.deepEqual(store.tracker.dirtyRootIds, new Set());
  assert.deepEqual(baseline[0].overrides, []);
  store.acceptImport(baseline, draft);
  assert.equal(store.tracker.isDirty, false);

  const changed = [{ ...subscription(), overrides: [{ rule: "example.com", enable: false }] }];
  store.stageImport(baseline, changed);
  assert.equal(store.tracker.isDirty, true);
  store.acceptImport(changed);
  assert.equal(store.tracker.isDirty, false);
  store.stageImport(changed, draft);
  assert.deepEqual(snapshot(store.tracker.data[0].overrides), []);
  const exported = snapshotFromServer(
    {
      configVersion: "test",
      app: {},
      groups: [],
      profiles: [],
      subscriptions: [{ ...changed[0], last_update: 123 }],
    },
    { subscriptions: snapshot(store.tracker.data) },
  );
  assert.deepEqual(exported.subscriptions[0].overrides, []);
  store.destroy();
});

Deno.test(
  "GET started before import cannot replace a staged or confirmed group/subscription draft",
  async () => {
    for (const kind of ["groups", "subscriptions"] as const) {
      const store = kind === "groups" ? new GroupsStore() : new SubscriptionsStore();
      const pending = Promise.withResolvers<unknown>();
      const oldGet = fetcher.get;
      fetcher.get = <T>() => pending.promise as Promise<T>;
      try {
        const loading = store.mount();
        if (store instanceof GroupsStore)
          store.stageImport([group()], [group("11223344", "Imported")]);
        else store.stageImport([subscription()], [subscription("22334455", "Imported")]);
        pending.resolve({ [kind]: [] });
        await loading;
        assert.equal(store.tracker.data.length, 1);
        assert.equal(store.tracker.data[0].name, "Imported");
        assert.equal(store.tracker.isDirty, true);
        assert.equal(store.dataLoaded, true);
        assert.equal(store.fetchError, false);
      } finally {
        fetcher.get = oldGet;
        store.destroy();
      }
    }
  },
);

Deno.test(
  "profile imports remain drafts with isolated arrays and accepted definitions",
  async () => {
    await withoutRequests(() => {
      const baseline = [profile()];
      const draft = [profile("vpn", "Imported VPN"), profile("new", "New VPN")];
      profiles.persistencePending = true;
      stageProfileImport(baseline, draft);
      assert.equal(profiles.list.length, 1);
      assert.equal(profiles.list[0].name, "VPN");
      assert.deepEqual(profileDraftIds(), new Set(["vpn", "new"]));
      assert.equal(profilesDirty(), true);
      assert.equal(profiles.persistencePending, true);
      profiles.draft[0].interfaces.reverse();
      assert.deepEqual(profiles.list[0].interfaces, ["wg0", "wg1"]);
      assert.deepEqual(draft[0].interfaces, ["wg0", "wg1"]);
      acceptProfileImport(snapshot(profiles.draft));
      assert.equal(profilesDirty(), false);
      assert.deepEqual(profileDraftIds(), new Set());
      stageProfileImport(snapshot(profiles.list), baseline);
      assert.equal(profilesDirty(), true);
      assert.equal(profiles.draft.length, 1);
    });
  },
);

Deno.test(
  "profile fetch predating import cannot publish stale definitions or erase its draft",
  async () => {
    acceptProfileImport([profile()]);
    const response = Promise.withResolvers<unknown>();
    const oldGet = fetcher.get;
    fetcher.get = <T>() => response.promise as Promise<T>;
    try {
      const pending = fetchProfiles();
      stageProfileImport([profile()], [profile("vpn", "Imported")]);
      response.resolve({ profiles: [] });
      await pending;
      assert.equal(profiles.list[0].name, "VPN");
      assert.equal(profiles.draft[0].name, "Imported");
      assert.equal(profilesDirty(), true);
    } finally {
      fetcher.get = oldGet;
    }
  },
);

Deno.test("read-only import acceptance never clears an earlier pending disk write", async () => {
  await withoutRequests(() => {
    const groups = new GroupsStore();
    const subscriptions = new SubscriptionsStore();
    try {
      // These flags are set by PERSISTENCE_FAILED + applied:true responses.
      // Reconciliation makes no write and must keep Save and unload warnings.
      groups.persistencePending = true;
      groups.acceptImport([group()], [group()], { persisted: false });
      assert.equal(groups.persistencePending, true);

      subscriptions.persistencePending = true;
      subscriptions.acceptImport([subscription()], [subscription()], { persisted: false });
      assert.equal(subscriptions.persistencePending, true);

      profiles.persistencePending = true;
      acceptProfileImport([profile()], [profile()], { persisted: false });
      assert.equal(profiles.persistencePending, true);
      assert.equal(profilesDirty(), true);
      assert.deepEqual(profileDraftIds(), new Set());

      // Only a confirmed successful write is allowed to acknowledge them.
      groups.acceptImport([group()]);
      subscriptions.acceptImport([subscription()]);
      acceptProfileImport([profile()]);
      assert.equal(groups.persistencePending, false);
      assert.equal(subscriptions.persistencePending, false);
      assert.equal(profiles.persistencePending, false);
      assert.equal(profilesDirty(), false);
    } finally {
      groups.destroy();
      subscriptions.destroy();
      profiles.persistencePending = false;
    }
  });
});

Deno.test("staging import restores profile usage from saved group and subscription consumers", () => {
  const definitions = [profile("vpn", "Original"), profile("free", "Unused")];
  const original = snapshot(definitions);
  const server = {
    groups: [
      { ...group("11111111"), profile: "vpn" },
      { ...group("22222222"), profile: "vpn" },
    ],
    subscriptions: [
      { ...subscription("33333333"), profile: "vpn" },
      { ...subscription("44444444"), profile: undefined },
    ],
  };
  const counted = profilesWithUsage(definitions, server);
  assert.deepEqual(counted.map((item) => item.usage), [
    { groups: 2, subscriptions: 1 },
    { groups: 0, subscriptions: 0 },
  ]);
  assert.deepEqual(definitions, original, "usage must not mutate portable definitions");
  stageProfileImport(counted, [profile("vpn", "Imported"), profile("free", "Unused")]);
  assert.deepEqual(profiles.list[0].usage, { groups: 2, subscriptions: 1 });
  assert.equal(profiles.list[0].name, "Original");
  assert.equal(profiles.draft[0].name, "Imported");
  assert.equal(profilesDirty(), true);
  acceptProfileImport(counted);
});
