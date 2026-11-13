import assert from "node:assert";
import { describe, it } from "jsr:@std/testing@1.0.19/bdd";

import { installSvelteRunesMocks } from "../mocks/setup-svelte-runes";

installSvelteRunesMocks();

type Rule = {
  id: string;
  enable: boolean;
  name: string;
  rule: string;
  type: string;
};

type Group = {
  priority: number;
  id: string;
  name: string;
  color: string;
  interface: string;
  enable: boolean;
  rules: Rule[];
};

const createMemoryStorage = () => {
  const store = new Map<string, string>();

  return {
    getItem(key: string) {
      return store.has(key) ? store.get(key)! : null;
    },
    setItem(key: string, value: string) {
      store.set(key, String(value));
    },
    removeItem(key: string) {
      store.delete(key);
    },
    clear() {
      store.clear();
    },
    key(index: number) {
      return Array.from(store.keys())[index] ?? null;
    },
    get length() {
      return store.size;
    },
  };
};

const patchGlobal = (key: string, value: unknown) => {
  const original = Object.getOwnPropertyDescriptor(globalThis, key);
  Object.defineProperty(globalThis, key, {
    value,
    configurable: true,
    writable: true,
  });

  return () => {
    if (original) {
      Object.defineProperty(globalThis, key, original);
      return;
    }
    delete (globalThis as any)[key];
  };
};

const restoreLocalStorage = patchGlobal("localStorage", createMemoryStorage());
type ProcessLike = { env?: unknown };
const processObj = (globalThis as unknown as { process?: ProcessLike }).process;
const originalProcessEnvDescriptor = processObj
  ? Object.getOwnPropertyDescriptor(processObj, "env")
  : undefined;
if (processObj) {
  Object.defineProperty(processObj, "env", {
    value: { NODE_ENV: "test" },
    configurable: true,
    writable: true,
  });
}
const { GroupsStore } = await import("../../src/modules/groups/groups.svelte");
if (processObj) {
  if (originalProcessEnvDescriptor) {
    Object.defineProperty(processObj, "env", originalProcessEnvDescriptor);
  } else {
    delete processObj.env;
  }
}
restoreLocalStorage();

const makeRule = (id: string, name = id, pattern = `${id}.example.com`): Rule => ({
  id,
  enable: true,
  name,
  rule: pattern,
  type: "domain",
});

const makeGroup = (id: string, rules: Rule[] = []): Group => ({
  priority: 300,
  id,
  name: `group-${id}`,
  color: "#ffffff",
  interface: "all",
  enable: true,
  rules,
});

const createStore = (groups: Group[] = []) => {
  const store = new GroupsStore();
  store.data.splice(0, store.data.length, ...structuredClone(groups));
  store.dataRevision = 0;
  store.valid_rules = true;
  store.open_state = {};
  for (const group of store.data) {
    store.open_state[group.id] = false;
  }
  return store;
};

const groupIds = (store: InstanceType<typeof GroupsStore>) => store.data.map((group) => group.id);
const ruleIds = (store: InstanceType<typeof GroupsStore>, groupIndex: number) =>
  store.data[groupIndex]?.rules.map((rule) => rule.id) ?? [];

const withPatchedGlobal = async <T>(key: string, value: unknown, action: () => T | Promise<T>) => {
  const restore = patchGlobal(key, value);
  try {
    return await action();
  } finally {
    restore();
  }
};

describe("GroupsStore mutations and validation", () => {
  it("validates offscreen rules without consulting mounted input elements", async () => {
    const rules = Array.from({ length: 1000 }, (_, index) => makeRule(`r${index}`));
    const store = createStore([makeGroup("g1", rules)]);
    await withPatchedGlobal(
      "document",
      {
        querySelector: () => {
          throw new Error("DOM validation is incomplete");
        },
      },
      () => {
        store.checkRulesValidityState();
        assert.strictEqual(store.valid_rules, true);
        store.data[0].rules[999].rule = "";
        store.checkRulesValidityState();
        assert.strictEqual(store.valid_rules, false);
        store.data[0].rules[999].rule = "restored.example.test";
        store.checkRulesValidityState();
        assert.strictEqual(store.valid_rules, true);
      },
    );
  });

  it("uses each rule type's validator for offscreen wildcard patterns", () => {
    const rule = { ...makeRule("wildcard"), type: "wildcard", rule: "*.example.test" };
    const store = createStore([makeGroup("g1", [rule])]);
    store.checkRulesValidityState();
    assert.strictEqual(store.valid_rules, true);
    store.data[0].rules[0].rule = "**.example.test";
    store.checkRulesValidityState();
    assert.strictEqual(store.valid_rules, false);
    store.data[0].rules[0].rule = "*.example.test";
    store.data[0].rules[0].type = "domain";
    store.checkRulesValidityState();
    assert.strictEqual(store.valid_rules, false);
  });

  it("markDataRevision increments revision counter", () => {
    const store = createStore();
    store.markDataRevision();
    store.markDataRevision();
    assert.strictEqual(store.dataRevision, 2);
  });

  it("addGroup prepends group, adds default rule and opens it", async () => {
    const store = createStore([makeGroup("g1", [makeRule("r1")])]);

    await withPatchedGlobal("document", { querySelector: () => null }, async () => {
      await store.addGroup();
    });

    assert.strictEqual(store.data.length, 2);
    assert.strictEqual(store.data[0].rules.length, 1);
    assert.strictEqual(store.open_state[store.data[0].id], true);
    assert.deepStrictEqual(groupIds(store).slice(1), ["g1"]);
    assert.strictEqual(store.dataRevision, 2);
  });

  it("deleteGroup respects confirm and removes open_state entry", async () => {
    const store = createStore([makeGroup("g1"), makeGroup("g2")]);
    store.open_state.g1 = true;
    store.open_state.g2 = true;

    await withPatchedGlobal(
      "confirm",
      () => false,
      () => {
        store.deleteGroup(0);
        assert.deepStrictEqual(groupIds(store), ["g1", "g2"]);
        assert.strictEqual(store.dataRevision, 0);
      },
    );

    await withPatchedGlobal(
      "confirm",
      () => true,
      () => {
        store.deleteGroup(0);
        assert.deepStrictEqual(groupIds(store), ["g2"]);
        assert.strictEqual("g1" in store.open_state, false);
        assert.strictEqual(store.dataRevision, 1);
      },
    );
  });

  it("changeGroupIndex supports before/after and clamps target indexes", () => {
    const store = createStore([makeGroup("a"), makeGroup("b"), makeGroup("c")]);

    store.changeGroupIndex(0, 2, "before");
    assert.deepStrictEqual(groupIds(store), ["b", "a", "c"]);

    store.changeGroupIndex(0, 99, "after");
    assert.deepStrictEqual(groupIds(store), ["a", "c", "b"]);

    store.changeGroupIndex(0, 0, "before");
    assert.deepStrictEqual(groupIds(store), ["a", "c", "b"]);
    assert.strictEqual(store.dataRevision, 2);
  });

  it("handleGroupSlotDrop applies group reorder semantics", () => {
    const store = createStore([makeGroup("a"), makeGroup("b"), makeGroup("c")]);

    store.handleGroupSlotDrop(
      { group_id: "a", group_index: 0, name: "group-a", color: "#111111", count: 0 },
      { group_index: 2, insert: "before" },
    );

    assert.deepStrictEqual(groupIds(store), ["b", "a", "c"]);
    assert.strictEqual(store.dataRevision, 1);
  });

  it("addRuleToGroup prepends and marks rules invalid for empty fields", async () => {
    const store = createStore([makeGroup("g1", [makeRule("r1")])]);

    await store.addRuleToGroup(0, makeRule("r2", "", ""), false);
    assert.deepStrictEqual(ruleIds(store, 0), ["r2", "r1"]);
    assert.strictEqual(store.valid_rules, false);
    assert.strictEqual(store.dataRevision, 1);

    await store.addRuleToGroup(99, makeRule("r3"), false);
    assert.deepStrictEqual(ruleIds(store, 0), ["r2", "r1"]);
    assert.strictEqual(store.dataRevision, 1);
  });

  it("deleteRuleFromGroup removes by index and keeps out-of-range behavior", () => {
    const store = createStore([makeGroup("g1", [makeRule("r1"), makeRule("r2"), makeRule("r3")])]);

    store.deleteRuleFromGroup(0, 1);
    assert.deepStrictEqual(ruleIds(store, 0), ["r1", "r3"]);
    assert.strictEqual(store.dataRevision, 1);

    store.deleteRuleFromGroup(0, 99);
    assert.deepStrictEqual(ruleIds(store, 0), ["r1", "r3"]);
    assert.strictEqual(store.dataRevision, 2);
  });

  it("changeRuleIndex reorders rules inside and across groups", () => {
    const store = createStore([
      makeGroup("g1", [makeRule("a"), makeRule("b"), makeRule("c")]),
      makeGroup("g2", [makeRule("d")]),
    ]);

    store.changeRuleIndex(0, 0, 0, 2, "c", "before");
    assert.deepStrictEqual(ruleIds(store, 0), ["b", "a", "c"]);

    store.changeRuleIndex(0, 0, 1, 0, "d", "after");
    assert.deepStrictEqual(ruleIds(store, 0), ["a", "c"]);
    assert.deepStrictEqual(ruleIds(store, 1), ["d", "b"]);
    assert.strictEqual(store.dataRevision, 2);
  });

  it("sortGroupRules and restoreGroupRulesOrder update order with return flags", () => {
    const store = createStore([makeGroup("g1", [makeRule("r1", "beta"), makeRule("r2", "alpha")])]);
    const initial = ruleIds(store, 0);

    const sorted = store.sortGroupRules(0, "name", "asc");
    assert.strictEqual(sorted, true);
    assert.deepStrictEqual(ruleIds(store, 0), ["r2", "r1"]);

    const restored = store.restoreGroupRulesOrder(0, initial);
    assert.strictEqual(restored, true);
    assert.deepStrictEqual(ruleIds(store, 0), initial);

    const failed = store.restoreGroupRulesOrder(0, ["missing"]);
    assert.strictEqual(failed, false);
  });

  it("addGroups prepends in source order and overwriteGroups resets state", async () => {
    const store = createStore([makeGroup("base")]);

    await store.addGroups([]);
    assert.deepStrictEqual(groupIds(store), ["base"]);
    assert.strictEqual(store.dataRevision, 0);

    await store.addGroups([makeGroup("g1"), makeGroup("g2")]);
    assert.deepStrictEqual(groupIds(store), ["g1", "g2", "base"]);
    assert.strictEqual(store.open_state.g1, true);
    assert.strictEqual(store.open_state.g2, true);

    store.dataLoaded = true;
    store.getPanelState("g1").sortField = "name";
    await store.overwriteGroups([makeGroup("new1")]);
    assert.deepStrictEqual(groupIds(store), ["new1"]);
    assert.strictEqual("g1" in store.open_state, false);
    assert.strictEqual(store.open_state.new1, true);
    assert.strictEqual(store.dataLoaded, true);
    assert.deepStrictEqual(store.panelState, {});
  });

  it("addRulesToGroup prepends rules and no-ops for empty or invalid target", async () => {
    const store = createStore([makeGroup("g1", [makeRule("r1")])]);

    await store.addRulesToGroup(0, []);
    assert.deepStrictEqual(ruleIds(store, 0), ["r1"]);
    assert.strictEqual(store.dataRevision, 0);

    await store.addRulesToGroup(0, [makeRule("r2"), makeRule("r3")]);
    assert.deepStrictEqual(ruleIds(store, 0), ["r2", "r3", "r1"]);
    assert.strictEqual(store.dataRevision, 1);

    await store.addRulesToGroup(99, [makeRule("r4")]);
    assert.deepStrictEqual(ruleIds(store, 0), ["r2", "r3", "r1"]);
    assert.strictEqual(store.dataRevision, 1);
  });

  it("toConfigPayload returns detached snapshot of current groups", () => {
    const store = createStore([makeGroup("g1", [makeRule("r1")])]);

    const payload = store.toConfigPayload();
    payload.groups[0].name = "mutated";
    payload.groups[0].rules[0].name = "mutated-rule";

    assert.strictEqual(store.data[0].name, "group-g1");
    assert.strictEqual(store.data[0].rules[0].name, "r1");
  });

  it("imports 100k rules atomically, keeps order and avoids existing client identities", async () => {
    const baseline = [makeGroup("g1", [makeRule("saved-rule")])];
    const store = createStore(baseline);
    store.tracker.reset(baseline);
    const rules = Array.from({ length: 100000 }, (_, index) =>
      makeRule((0x30000000 + index).toString(16), `import-${index}`),
    );
    rules[0].id = "saved-rule";
    const target = store.data[0].rules;
    const importing = store.addRulesToGroup(0, rules);
    assert.strictEqual(target.length, 1, "preparation must not publish chunks");
    await importing;
    assert.strictEqual(store.data[0].rules, target);
    assert.strictEqual(target.length, 100001);
    assert.strictEqual(new Set(target.map((rule) => rule.id)).size, target.length);
    assert.strictEqual(target[0].name, "import-0");
    assert.strictEqual(target[99999].name, "import-99999");
    assert.strictEqual(target[100000].id, "saved-rule");
    assert.strictEqual(rules[0].id, "saved-rule", "source remains detached");
    assert.strictEqual(store.dataRevision, 1);
    assert.strictEqual(store.tracker.isDirty, true);
    target.splice(0, 100000);
    assert.strictEqual(store.tracker.isDirty, false, "removing the batch restores the baseline");
  });

  it("aborted or stale large native imports retain the live draft", async () => {
    const store = createStore([makeGroup("g1", [makeRule("saved-rule")])]);
    const rules = Array.from({ length: 10000 }, (_, index) => makeRule(`incoming-${index}`));
    const controller = new AbortController();
    const cancelled = store.addRulesToGroup(0, rules, { signal: controller.signal });
    controller.abort();
    await assert.rejects(cancelled, { name: "AbortError" });
    assert.deepStrictEqual(ruleIds(store, 0), ["saved-rule"]);
    const stale = store.addRulesToGroup(0, rules);
    store.data[0].name = "Changed while preparing";
    store.markDataRevision();
    await assert.rejects(
      stale,
      /(?:Configuration changed while importing rules|Draft changed during bulk insertion)/,
    );
    assert.strictEqual(store.data[0].name, "Changed while preparing");
    assert.deepStrictEqual(ruleIds(store, 0), ["saved-rule"]);
  });

  it("shares duplicate indexes and bounds detailed per-group conflict caches", () => {
    const groups = Array.from({ length: 50 }, (_, group) =>
      makeGroup(
        `g${group}`,
        Array.from({ length: 200 }, (_, rule) =>
          makeRule(`g${group}-r${rule}`, "", `shared-${rule}.example.test`),
        ),
      ),
    );
    const store = createStore(groups);
    store.refreshDuplicateRuleIds();
    assert.strictEqual(store.duplicateRuleIds.size, 10000);
    assert.strictEqual(store.duplicateGroupIds.size, 50);
    assert.strictEqual(store.duplicateConflictsByGroup.size, 0, "closed menus create no copies");
    for (const groupId of ["g0", "g1", "g2"]) {
      const conflicts = store.getDuplicateConflictsForGroup(groupId);
      assert.strictEqual(conflicts.length, 10000);
      assert.strictEqual(conflicts.filter((item) => item.inCurrentGroup).length, 200);
      assert.strictEqual(store.duplicateConflictsByGroup.size <= 2, true);
    }
    store.data[0].rules[0].rule = "now-unique.example.test";
    store.refreshDuplicateRuleIds();
    assert.strictEqual(store.isRuleDuplicate("g0-r0"), false);
    assert.strictEqual(store.duplicateConflictsByGroup.size, 0);
  });
});
