import assert from "node:assert";
import { describe, it } from "jsr:@std/testing@1.0.19/bdd";

import { installSvelteRunesMocks } from "../mocks/setup-svelte-runes";

installSvelteRunesMocks();

const { ChangeTracker } = await import("../../src/utils/change-tracker.svelte");

type RuleItem = {
  id: string;
  name: string;
  type: string;
  rule: string;
  enable: boolean;
};

const createItem = (overrides: Partial<RuleItem> = {}): RuleItem => ({
  id: "1",
  name: "Test Rule",
  type: "namespace",
  rule: "example.com",
  enable: true,
  ...overrides,
});

describe("ChangeTracker", () => {
  it("should initialize clean", () => {
    const data = [createItem({ id: "1" })];
    const tracker = new ChangeTracker(data);

    assert.strictEqual(tracker.isDirty, false);
    assert.strictEqual(tracker.changes.added.length, 0);
    assert.strictEqual(tracker.changes.deleted.length, 0);
    assert.strictEqual(tracker.changes.mutated.length, 0);
  });

  describe("Changes: Mutated", () => {
    it("should track mutated objects", () => {
      const data = [createItem({ id: "1", name: "Old" })];
      const tracker = new ChangeTracker(data);
      const proxy = tracker.data;

      proxy[0].name = "New";

      assert.strictEqual(tracker.isDirty, true);
      assert.strictEqual(tracker.changes.mutated.length, 1);
      assert.strictEqual(tracker.changes.mutated[0].id, "1");
      assert.strictEqual(tracker.changes.added.length, 0);
      assert.strictEqual(tracker.changes.deleted.length, 0);
    });

    it("should stop tracking mutation if reverted", () => {
      const data = [createItem({ id: "1", name: "Old" })];
      const tracker = new ChangeTracker(data);
      const proxy = tracker.data;

      proxy[0].name = "New";
      assert.strictEqual(tracker.changes.mutated.length, 1);

      proxy[0].name = "Old";
      assert.strictEqual(tracker.changes.mutated.length, 0);
      assert.strictEqual(tracker.isDirty, false);
    });
  });

  describe("Changes: Added", () => {
    it("should track added objects", () => {
      const tracker = new ChangeTracker<RuleItem[]>([]);
      const proxy = tracker.data;
      const newItem = createItem({ id: "new1" });

      proxy.push(newItem);

      assert.strictEqual(tracker.isDirty, true);
      assert.strictEqual(tracker.changes.added.length, 1);
      assert.strictEqual(tracker.changes.added[0].id, "new1");
      assert.strictEqual(tracker.changes.mutated.length, 0);
    });

    it("should not track added object if it is subsequently removed", () => {
      const tracker = new ChangeTracker<RuleItem[]>([]);
      const proxy = tracker.data;
      const newItem = createItem({ id: "new1" });

      proxy.push(newItem);
      assert.strictEqual(tracker.changes.added.length, 1);

      proxy.pop();
      assert.strictEqual(tracker.changes.added.length, 0);
      assert.strictEqual(tracker.changes.deleted.length, 0); // Was never in original
      assert.strictEqual(tracker.isDirty, false);
    });
  });

  describe("Changes: Deleted", () => {
    it("should track deleted objects", () => {
      const data = [createItem({ id: "1" })];
      const tracker = new ChangeTracker(data);
      const proxy = tracker.data;

      proxy.pop();

      assert.strictEqual(tracker.isDirty, true);
      assert.strictEqual(tracker.changes.deleted.length, 1);
      assert.strictEqual(tracker.changes.deleted[0].id, "1");
      assert.strictEqual(tracker.changes.added.length, 0);
    });

    it("should not track deleted object if it is added back", () => {
      const item = createItem({ id: "1" });
      const tracker = new ChangeTracker([item]);
      const proxy = tracker.data;

      const removed = proxy.pop();
      assert.strictEqual(tracker.changes.deleted.length, 1);

      proxy.push(removed!);
      assert.strictEqual(tracker.changes.deleted.length, 0);
      assert.strictEqual(tracker.changes.added.length, 0); // Exists in original
      assert.strictEqual(tracker.isDirty, false);
    });
  });

  describe("Changes: Mixed Scenarios", () => {
    it("should track add, delete and mutate simultaneously", () => {
      const i1 = createItem({ id: "1" });
      const i2 = createItem({ id: "2" });
      const tracker = new ChangeTracker([i1, i2]);
      const proxy = tracker.data;

      proxy[0].name = "Mutated"; // Mutate 1
      proxy.pop(); // Delete 2
      proxy.push(createItem({ id: "3" })); // Add 3

      const { added, deleted, mutated } = tracker.changes;

      assert.strictEqual(mutated.length, 1);
      assert.strictEqual(mutated[0].id, "1");

      assert.strictEqual(deleted.length, 1);
      assert.strictEqual(deleted[0].id, "2");

      assert.strictEqual(added.length, 1);
      assert.strictEqual(added[0].id, "3");
    });

    it("should not consider reordering as mutation of objects", () => {
      const i1 = createItem({ id: "1" });
      const i2 = createItem({ id: "2" });
      const tracker = new ChangeTracker([i1, i2]);
      const proxy = tracker.data;

      proxy.reverse();

      // Array is dirty (order changed)
      assert.strictEqual(tracker.isDirty, true);

      // But objects themselves are not mutated, added, or deleted
      assert.strictEqual(tracker.changes.mutated.length, 0);
      assert.strictEqual(tracker.changes.added.length, 0);
      assert.strictEqual(tracker.changes.deleted.length, 0);
    });

    it("should handle nested structures correctly", () => {
      // Setup a parent object containing a list
      const root = {
        id: "root",
        list: [createItem({ id: "child1" })],
      };
      const tracker = new ChangeTracker(root);
      const proxy = tracker.data;

      // Add to nested list
      proxy.list.push(createItem({ id: "child2" }));

      assert.strictEqual(tracker.isDirty, true);
      assert.strictEqual(tracker.changes.added.length, 1);
      assert.strictEqual(tracker.changes.added[0].id, "child2");

      // Mutate existing nested item
      proxy.list[0].name = "Changed";
      assert.strictEqual(tracker.changes.mutated.length, 1);
      assert.strictEqual(tracker.changes.mutated[0].id, "child1");
    });
  });

  describe("Reset", () => {
    it("should clear changes on reset", () => {
      const tracker = new ChangeTracker([createItem({ id: "1" })]);
      tracker.data.pop(); // Delete

      assert.strictEqual(tracker.changes.deleted.length, 1);

      tracker.reset([]); // Reset to empty state (matches current state effectively)

      assert.strictEqual(tracker.isDirty, false);
      assert.strictEqual(tracker.changes.deleted.length, 0);
    });

    it("should update data in place on reset", () => {
      const oldData = [createItem({ id: "1" })];
      const newData = [createItem({ id: "2" })];
      const tracker = new ChangeTracker(oldData);

      tracker.reset(newData);

      assert.strictEqual(tracker.data[0].id, "2");
      assert.strictEqual(tracker.isDirty, false);
    });

    it("should expose a fresh root proxy after reset so add/delete acknowledgements still work", () => {
      const tracker = new ChangeTracker([createItem({ id: "1" })]);
      const firstProxy = tracker.data;

      tracker.reset([createItem({ id: "1" })]);

      const secondProxy = tracker.data;
      const added = createItem({ id: "2" });

      assert.notStrictEqual(secondProxy, firstProxy);

      secondProxy.push(added);
      tracker.acknowledgeNewItem(secondProxy, added, "end");
      assert.strictEqual(tracker.isDirty, false);

      secondProxy.pop();
      tracker.acknowledgeDelete(secondProxy, "2");
      assert.strictEqual(tracker.isDirty, false);
    });
  });

  describe("Nested Data (Groups -> Rules)", () => {
    // Функция-фабрика, возвращающая свежие данные для каждого теста
    const getComplexData = () => [
      {
        id: "g1",
        name: "Group 1",
        enable: true,
        rules: [
          { id: "r1", name: "Rule 1", type: "ns", rule: "abc.com", enable: true },
          { id: "r2", name: "Rule 2", type: "ns", rule: "xyz.com", enable: true },
        ],
      },
      {
        id: "g2",
        name: "Group 2",
        enable: true,
        rules: [],
      },
    ];

    it("should track adding a rule to a nested group", () => {
      // Генерируем данные внутри теста
      const tracker = new ChangeTracker(getComplexData());
      const proxy = tracker.data;

      const newRule = { id: "r3", name: "New Rule", type: "ns", rule: "new.com", enable: true };
      proxy[1].rules.push(newRule);

      assert.strictEqual(tracker.isDirty, true);

      const { added, mutated } = tracker.changes;
      assert.strictEqual(added.length, 1);
      assert.strictEqual(added[0].id, "r3");

      assert.strictEqual(mutated.length, 0);
    });

    it("should track deleting a rule from a nested group", () => {
      const tracker = new ChangeTracker(getComplexData());
      const proxy = tracker.data;

      proxy[0].rules.shift(); // Удаляем r1

      assert.strictEqual(tracker.isDirty, true);

      const { deleted } = tracker.changes;
      assert.strictEqual(deleted.length, 1);
      assert.strictEqual(deleted[0].id, "r1");
    });

    it("should track mutating a field deeply nested", () => {
      const tracker = new ChangeTracker(getComplexData());
      const proxy = tracker.data;

      // Теперь здесь порядок r1, r2 гарантирован, индекс 1 существует
      proxy[0].rules[1].name = "Renamed Rule 2";

      assert.strictEqual(tracker.isDirty, true);

      const { mutated } = tracker.changes;
      assert.strictEqual(mutated.length, 1);
      assert.strictEqual(mutated[0].id, "r2");
    });

    it("should track mutation of the group itself mixed with rule changes", () => {
      const tracker = new ChangeTracker(getComplexData());
      const proxy = tracker.data;

      proxy[0].name = "Renamed Group 1";
      proxy[0].rules.push({ id: "r99", name: "", type: "", rule: "", enable: true });

      const { added, mutated } = tracker.changes;

      const groupChange = mutated.find((x: any) => x.id === "g1");
      assert.ok(groupChange);

      const ruleAdd = added.find((x: any) => x.id === "r99");
      assert.ok(ruleAdd);
    });

    it("should handle group deletion (cascading delete)", () => {
      const tracker = new ChangeTracker(getComplexData());
      const proxy = tracker.data;

      proxy.shift(); // Удаляем g1 целиком

      const { deleted } = tracker.changes;

      assert.strictEqual(deleted.length, 3);
      assert.ok(deleted.find((x: any) => x.id === "g1"));
      assert.ok(deleted.find((x: any) => x.id === "r1"));
      assert.ok(deleted.find((x: any) => x.id === "r2"));
    });
  });
  describe("Partial Commits (acknowledge methods)", () => {
    it("should acknowledge a mutation and make it clean", () => {
      const data = [createItem({ id: "1", name: "Old" })];
      const tracker = new ChangeTracker(data);
      const proxy = tracker.data;

      proxy[0].name = "New";
      assert.strictEqual(tracker.isDirty, true);
      assert.strictEqual(tracker.changes.mutated.length, 1);

      tracker.acknowledgeUpdate(proxy[0]);

      assert.strictEqual(tracker.isDirty, false);
      assert.strictEqual(tracker.changes.mutated.length, 0);
      assert.strictEqual(tracker.changes.added.length, 0);
      assert.strictEqual(tracker.changes.deleted.length, 0);
    });

    it("should acknowledge a new item and make it clean", () => {
      const tracker = new ChangeTracker<RuleItem[]>([]);
      const proxy = tracker.data;
      const newItem = createItem({ id: "new1", name: "Added" });

      proxy.push(newItem);
      assert.strictEqual(tracker.isDirty, true);
      assert.strictEqual(tracker.changes.added.length, 1);

      tracker.acknowledgeNewItem(proxy, newItem, "end");

      assert.strictEqual(tracker.isDirty, false);
      assert.strictEqual(tracker.changes.added.length, 0);
      assert.strictEqual(tracker.changes.mutated.length, 0);
      assert.strictEqual(tracker.changes.deleted.length, 0);
    });

    it("should acknowledge a new item at start and maintain order", () => {
      const existing = createItem({ id: "1" });
      const tracker = new ChangeTracker([existing]);
      const proxy = tracker.data;

      const newItem = createItem({ id: "new1" });
      proxy.unshift(newItem); // Add to start

      assert.strictEqual(tracker.isDirty, true);

      tracker.acknowledgeNewItem(proxy, newItem, "start");

      assert.strictEqual(tracker.isDirty, false);
      assert.strictEqual(proxy[0].id, "new1");
      assert.strictEqual(proxy[1].id, "1");
    });

    it("should handle acknowledged update mixed with other dirty states", () => {
      const i1 = createItem({ id: "1", name: "A" });
      const i2 = createItem({ id: "2", name: "B" });
      const tracker = new ChangeTracker([i1, i2]);
      const proxy = tracker.data;

      proxy[0].name = "A_Changed"; // i1 dirty
      proxy[1].name = "B_Changed"; // i2 dirty

      assert.strictEqual(tracker.changes.mutated.length, 2);

      // Acknowledge only i2
      tracker.acknowledgeUpdate(proxy[1]);

      assert.strictEqual(tracker.isDirty, true);
      assert.strictEqual(tracker.changes.mutated.length, 1);
      assert.strictEqual(tracker.changes.mutated[0].id, "1");
    });

    it("should preserve unrelated dirty fields when acknowledging specific persisted fields", () => {
      const tracker = new ChangeTracker([
        {
          id: "sub1",
          name: "Original",
          interface: "all",
          interval: 60,
          enable: true,
          lastUpdate: 0,
          rules: [createItem({ id: "rule1" })],
        },
      ]);
      const proxy = tracker.data;

      proxy[0].name = "Unsaved";
      proxy[0].rules.push(createItem({ id: "rule-local" }));

      tracker.acknowledgeUpdate(proxy[0], {
        rules: [createItem({ id: "rule-server" })],
        lastUpdate: 123,
      });

      assert.strictEqual(proxy[0].name, "Unsaved");
      assert.strictEqual(proxy[0].lastUpdate, 123);
      assert.deepStrictEqual(
        proxy[0].rules.map((rule: RuleItem) => rule.id),
        ["rule-server"],
      );
      assert.strictEqual(tracker.isDirty, true);
      assert.deepStrictEqual(
        tracker.changes.mutated.map((item: { id: string }) => item.id),
        ["sub1"],
      );
      assert.strictEqual(tracker.changes.added.length, 0);
      assert.strictEqual(tracker.changes.deleted.length, 0);
    });

    it("should acknowledge a deletion and make array clean", () => {
      const item = createItem({ id: "1" });
      const tracker = new ChangeTracker([item]);
      const proxy = tracker.data;

      proxy.pop(); // Delete from array
      assert.strictEqual(tracker.isDirty, true);
      assert.strictEqual(tracker.changes.deleted.length, 1);

      tracker.acknowledgeDelete(proxy, "1");

      assert.strictEqual(tracker.isDirty, false);
      assert.strictEqual(tracker.changes.deleted.length, 0);
    });

    it("should clean up dirty state of children when parent is updated", () => {
      const tracker = new ChangeTracker([
        { id: "p1", name: "Parent", children: [{ id: "c1", name: "Child" }] },
      ]);
      const proxy = tracker.data;

      // Modify child
      proxy[0].children[0].name = "Child Modified";
      assert.strictEqual(tracker.isDirty, true);

      // Acknowledge parent update
      tracker.acknowledgeUpdate(proxy[0]);

      assert.strictEqual(
        tracker.isDirty,
        false,
        "Tracker should be clean after acknowledging parent update",
      );
    });

    it("should clean up dirty state of children when parent is deleted", () => {
      const tracker = new ChangeTracker([
        { id: "p1", name: "Parent", children: [{ id: "c1", name: "Child" }] },
      ]);
      const proxy = tracker.data;

      // Modify child
      proxy[0].children[0].name = "Child Modified";
      assert.strictEqual(tracker.isDirty, true);

      // Delete parent
      proxy.pop();
      tracker.acknowledgeDelete(proxy, "p1");

      if (tracker.isDirty) {
        console.log("Dirty Objects:", tracker["dirtyObjectProps"]);
        console.log("Dirty Arrays:", tracker["dirtyArrays"]);
      }

      assert.strictEqual(
        tracker.isDirty,
        false,
        "Tracker should be clean after acknowledging parent delete",
      );
    });
  });

  describe("Rebasing after an unrelated import", () => {
    type GroupItem = {
      id: string;
      name: string;
      rules: RuleItem[];
    };
    const group = (overrides: Partial<GroupItem> = {}): GroupItem => ({
      id: "group1",
      name: "Original group",
      rules: [createItem({ id: "rule1" }), createItem({ id: "rule2" })],
      ...overrides,
    });
    const ids = (items: readonly { id: string }[]) => items.map((item) => item.id);

    it("preserves a nested draft while accepting clean imported additions and updates", () => {
      const initial = [group(), group({ id: "group2", rules: [] })];
      const incoming = structuredClone(initial);
      incoming[1].name = "Updated by import";
      incoming.push(group({ id: "imported", rules: [] }));
      const tracker = new ChangeTracker(initial);
      tracker.data[0].rules[0].enable = false;

      assert.deepStrictEqual([...tracker.dirtyRootIds], ["group1"]);
      tracker.rebase(incoming);

      assert.strictEqual(tracker.data[0].rules[0].enable, false);
      assert.strictEqual(tracker.data[1].name, "Updated by import");
      assert.deepStrictEqual(ids(tracker.data), ["group1", "group2", "imported"]);
      assert.deepStrictEqual(ids(tracker.changes.mutated), ["rule1"]);
      assert.deepStrictEqual(tracker.changes.added, []);
      assert.deepStrictEqual(tracker.changes.deleted, []);
      assert.deepStrictEqual([...tracker.dirtyRootIds], ["group1"]);
      assert.strictEqual(incoming[0].rules[0].enable, true, "Server response stays untouched");

      tracker.data[0].rules[0].enable = true;
      assert.strictEqual(tracker.isDirty, false);
      assert.deepStrictEqual([...tracker.dirtyRootIds], []);
    });

    it("retains local root additions and deletions without dirtying imported objects", () => {
      const initial = [group(), group({ id: "group2", rules: [] })];
      const incoming = [...structuredClone(initial), group({ id: "imported", rules: [] })];
      const tracker = new ChangeTracker(initial);
      tracker.data.shift();
      tracker.data.push(group({ id: "local", rules: [createItem({ id: "local-rule" })] }));

      assert.deepStrictEqual(new Set(tracker.dirtyRootIds), new Set(["group1", "local"]));
      tracker.rebase(incoming);

      assert.deepStrictEqual(ids(tracker.data), ["group2", "local", "imported"]);
      assert.deepStrictEqual(ids(tracker.changes.added), ["local", "local-rule"]);
      assert.deepStrictEqual(ids(tracker.changes.deleted), ["group1", "rule1", "rule2"]);
      assert.deepStrictEqual(tracker.changes.mutated, []);
      assert.deepStrictEqual(new Set(tracker.dirtyRootIds), new Set(["group1", "local"]));

      tracker.data.splice(1, 1);
      tracker.data.unshift(structuredClone(incoming[0]));
      assert.strictEqual(tracker.isDirty, false);
      assert.deepStrictEqual([...tracker.dirtyRootIds], []);
    });

    it("preserves nested rule membership and order and can revert to the new baseline", () => {
      const initial = [group()];
      const incoming = [...structuredClone(initial), group({ id: "imported", rules: [] })];
      const tracker = new ChangeTracker(initial);
      tracker.data[0].rules.pop();
      tracker.data[0].rules.push(createItem({ id: "local-rule" }));
      tracker.data[0].rules.reverse();

      tracker.rebase(incoming);

      assert.deepStrictEqual(ids(tracker.data[0].rules), ["local-rule", "rule1"]);
      assert.deepStrictEqual(ids(tracker.changes.added), ["local-rule"]);
      assert.deepStrictEqual(ids(tracker.changes.deleted), ["rule2"]);
      assert.deepStrictEqual(tracker.changes.mutated, []);
      assert.deepStrictEqual([...tracker.dirtyRootIds], ["group1"]);

      tracker.data[0].rules.splice(0, 2, ...structuredClone(incoming[0].rules));
      assert.strictEqual(tracker.isDirty, false);
    });

    it("keeps local root order but excludes order-only changes from dirtyRootIds", () => {
      const initial = [group(), group({ id: "group2", rules: [] })];
      const incoming = [...structuredClone(initial), group({ id: "imported", rules: [] })];
      const tracker = new ChangeTracker(initial);
      tracker.data.reverse();

      assert.deepStrictEqual([...tracker.dirtyRootIds], []);
      tracker.rebase(incoming);

      assert.deepStrictEqual(ids(tracker.data), ["group2", "group1", "imported"]);
      assert.strictEqual(tracker.isDirty, true);
      assert.deepStrictEqual([...tracker.dirtyRootIds], []);
      assert.deepStrictEqual(tracker.changes, { added: [], deleted: [], mutated: [] });

      tracker.data.sort((a, b) => a.id.localeCompare(b.id));
      assert.strictEqual(tracker.isDirty, false);
    });

    it("uses the acknowledged subscription refresh as baseline while retaining metadata edits", () => {
      const initial = [{ ...group(), lastUpdate: 0 }];
      const tracker = new ChangeTracker(initial);
      tracker.data[0].name = "Unsaved subscription name";
      tracker.data[0].rules[0].enable = false;
      const refreshed = [createItem({ id: "server-rule", type: "domain" })];
      tracker.acknowledgeUpdate(tracker.data[0], { rules: refreshed, lastUpdate: 123 });
      const incoming = [
        { ...group({ rules: structuredClone(refreshed) }), lastUpdate: 123 },
        { ...group({ id: "imported", rules: [] }), lastUpdate: 456 },
      ];

      tracker.rebase(incoming);

      assert.strictEqual(tracker.data[0].name, "Unsaved subscription name");
      assert.strictEqual(tracker.data[0].lastUpdate, 123);
      assert.deepStrictEqual(ids(tracker.data[0].rules), ["server-rule"]);
      assert.deepStrictEqual(ids(tracker.changes.mutated), ["group1"]);
      assert.deepStrictEqual(tracker.changes.added, []);
      assert.deepStrictEqual(tracker.changes.deleted, []);
      assert.deepStrictEqual([...tracker.dirtyRootIds], ["group1"]);

      tracker.data[0].name = incoming[0].name;
      assert.strictEqual(tracker.isDirty, false);
      tracker.data[0].rules[0].enable = false;
      assert.deepStrictEqual(ids(tracker.changes.mutated), ["server-rule"]);
      tracker.data[0].rules[0].enable = true;
      assert.strictEqual(tracker.isDirty, false);
    });

    it("keeps acknowledge methods working after rebasing a draft", () => {
      const incoming = [group(), group({ id: "imported", rules: [] })];
      const tracker = new ChangeTracker([group()]);
      tracker.data[0].name = "Draft";
      tracker.rebase(incoming);
      tracker.acknowledgeUpdate(tracker.data[0]);
      assert.strictEqual(tracker.isDirty, false);
      assert.deepStrictEqual([...tracker.dirtyRootIds], []);

      const added = group({ id: "new-persisted", rules: [] });
      tracker.data.push(added);
      tracker.acknowledgeNewItem(tracker.data, added);
      assert.strictEqual(tracker.isDirty, false);
      tracker.data.pop();
      tracker.acknowledgeDelete(tracker.data, added.id);
      assert.strictEqual(tracker.isDirty, false);
      assert.deepStrictEqual([...tracker.dirtyRootIds], []);
    });

    it("rejects an ambiguous server deletion before changing a local draft", () => {
      const tracker = new ChangeTracker([group()]);
      tracker.data[0].name = "Draft";

      assert.throws(() => tracker.rebase([]), /local draft removed from the server/);
      assert.strictEqual(tracker.data[0].name, "Draft");
      assert.strictEqual(tracker.isDirty, true);
      assert.deepStrictEqual([...tracker.dirtyRootIds], ["group1"]);
    });
  });
});
