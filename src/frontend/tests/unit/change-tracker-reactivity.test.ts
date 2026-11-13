import assert from "node:assert/strict";
import { Buffer } from "node:buffer";

import trackerSource from "../../src/utils/change-tracker.svelte.ts" with { type: "text" };

// Keep this dependency in Deno's static module graph: the generated data URL
// must not require extra runtime filesystem permissions when run in isolation.
import "../../src/utils/cooperative-data.ts";

const runtimeProcess = (globalThis as typeof globalThis & { process?: { env?: unknown } }).process;
const environment = runtimeProcess && Object.getOwnPropertyDescriptor(runtimeProcess, "env");
if (runtimeProcess)
  Object.defineProperty(runtimeProcess, "env", { configurable: true, value: { NODE_ENV: "test" } });
const { default: ts } = await import("typescript");
const { compileModule } = await import("svelte/compiler");
const { derived, effect_root, flush, get, render_effect } = await import("svelte/internal/client");

// Most tracker tests use rune mocks. Compile the real class here so these
// checks exercise Svelte's actual dependency graph rather than a mock counter.
const javascript = ts.transpileModule(trackerSource, {
  compilerOptions: { target: ts.ScriptTarget.ESNext, module: ts.ModuleKind.ESNext },
}).outputText;
const compiled = compileModule(javascript, {
  generate: "client",
  filename: "change-tracker.svelte.js",
})
  .js.code.replaceAll(
    "'svelte/internal/client'",
    JSON.stringify(import.meta.resolve("svelte/internal/client")),
  )
  .replaceAll('"svelte"', JSON.stringify(import.meta.resolve("svelte/internal/client")))
  .replaceAll(
    '"svelte/reactivity"',
    JSON.stringify(
      new URL("../../reactivity/index-client.js", import.meta.resolve("svelte/internal/client"))
        .href,
    ),
  )
  .replaceAll(
    '"./cooperative-data"',
    JSON.stringify(new URL("../../src/utils/cooperative-data.ts", import.meta.url).href),
  );
const { ChangeTracker } = await import(
  `data:text/javascript;base64,${Buffer.from(compiled).toString("base64")}`
);
if (runtimeProcess && environment) Object.defineProperty(runtimeProcess, "env", environment);
else if (runtimeProcess) Reflect.deleteProperty(runtimeProcess, "env");

const fixture = () => [
  { id: "a", name: "First", rules: [{ id: "rule-a", rule: "first.test" }] },
  { id: "b", name: "Second", rules: [{ id: "rule-b", rule: "second.test" }] },
];

Deno.test("real tracker first property reads remain reactive inside a derived", () => {
  const tracker = new ChangeTracker(fixture());
  const selected = derived(() => tracker.data[0].rules[0].rule);
  let observed = "";
  const dispose = effect_root(() => {
    render_effect(() => {
      observed = get(selected);
    });
  });
  try {
    assert.equal(observed, "first.test");
    tracker.data[0].rules[0].rule = "edited.test";
    flush();
    assert.equal(observed, "edited.test");
  } finally {
    dispose();
  }
});

Deno.test(
  "real tracker subscriptions survive unmount, remount and stale reset cleanup",
  async () => {
    const tracker = new ChangeTracker(fixture());
    let observed = "";
    const mount = () =>
      effect_root(() => {
        render_effect(() => {
          observed = tracker.data[0].name;
        });
      });
    let dispose = mount();
    try {
      dispose();
      await Promise.resolve();
      tracker.data[0].name = "While unmounted";
      dispose = mount();
      assert.equal(observed, "While unmounted");
      tracker.data[0].name = "After remount";
      flush();
      assert.equal(observed, "After remount");
      dispose();
      // Remount before the old subscriber teardown microtask. Root array identity
      // is retained by reset, so an unguarded old cleanup could delete new cells.
      tracker.reset(fixture());
      dispose = mount();
      await Promise.resolve();
      tracker.data.reverse();
      flush();
      assert.equal(observed, "Second");
    } finally {
      dispose();
    }
  },
);

Deno.test(
  "real tracker reactivity isolates a field from sibling fields and collection structure",
  () => {
    const tracker = new ChangeTracker(fixture());
    const counts = { first: 0, second: 0, rules: 0, membership: 0, dirty: 0 };
    const dispose = effect_root(() => {
      render_effect(() => {
        tracker.data[0].name;
        counts.first++;
      });
      render_effect(() => {
        tracker.data[1].name;
        counts.second++;
      });
      render_effect(() => {
        tracker.data[0].rules[0].rule;
        counts.rules++;
      });
      render_effect(() => {
        tracker.data.map((item: { id: string }) => item.id);
        counts.membership++;
      });
      render_effect(() => {
        tracker.isDirty;
        counts.dirty++;
      });
    });
    try {
      tracker.data[0].name = "Edited";
      flush();
      assert.deepEqual(counts, { first: 2, second: 1, rules: 1, membership: 1, dirty: 2 });
      tracker.data[0].name = "Edited";
      flush();
      assert.deepEqual(counts, { first: 2, second: 1, rules: 1, membership: 1, dirty: 2 });
      tracker.data[0].rules[0].rule = "changed.test";
      flush();
      assert.deepEqual(counts, { first: 2, second: 1, rules: 2, membership: 1, dirty: 3 });
    } finally {
      dispose();
    }
  },
);

Deno.test(
  "real tracker reactivity observes array membership, deletion and raw baseline publication",
  () => {
    const tracker = new ChangeTracker(fixture());
    let names: string[] = [];
    let firstRule = "";
    let fields: string[] = [];
    const dispose = effect_root(() => {
      render_effect(() => {
        names = tracker.data.map((item: { name: string }) => item.name);
      });
      render_effect(() => {
        firstRule = tracker.data[0].rules[0]?.rule ?? "";
      });
      render_effect(() => {
        fields = Object.keys(tracker.data[0]);
      });
    });
    try {
      tracker.data.reverse();
      flush();
      assert.deepEqual(names, ["Second", "First"]);
      assert.equal(firstRule, "second.test");
      tracker.data[0].extra = "field";
      flush();
      assert(fields.includes("extra"));
      delete tracker.data[0].extra;
      flush();
      assert(!fields.includes("extra"));
      tracker.acknowledgeUpdate(tracker.data[0], {
        name: "Canonical",
        rules: [{ id: "new-rule", rule: "canonical.test" }],
      });
      flush();
      assert.deepEqual(names, ["Canonical", "First"]);
      assert.equal(firstRule, "canonical.test");
      tracker.reset(fixture());
      flush();
      assert.deepEqual(names, ["First", "Second"]);
      assert.equal(firstRule, "first.test");
    } finally {
      dispose();
    }
  },
);

Deno.test(
  "bulk preparation publishes one complete collection and keeps tracker reversions",
  async () => {
    const tracker = new ChangeTracker(fixture());
    const array = tracker.data[0].rules;
    const original = array[0];
    let lengths: number[] = [];
    const dispose = effect_root(() => {
      render_effect(() => {
        lengths.push(tracker.data[0].rules.length);
      });
    });
    try {
      const incoming = Array.from({ length: 100000 }, (_, index) => ({
        id: `new-${index}`,
        rule: `item-${index}.test`,
      }));
      const prepared = await tracker.preparePrepend(array, incoming, { budgetMs: 0 });
      assert.equal(array.length, 1);
      assert.equal(tracker.isDirty, false);
      assert.deepEqual(lengths, [1]);
      prepared.commit();
      flush();
      assert.equal(tracker.data[0].rules, array);
      assert.equal(array.length, 100001);
      assert.equal(array.at(-1), original);
      assert.deepEqual(lengths, [1, 100001]);
      incoming[0].rule = "outside.test";
      assert.equal(array[0].rule, "item-0.test");
      array[0].rule = "edited.test";
      flush();
      assert.equal(array[0].rule, "edited.test");
      array.splice(0, incoming.length);
      flush();
      assert.equal(tracker.isDirty, false);
      assert.equal(array[0], original);
      assert.deepEqual(lengths, [1, 100001, 1]);
      assert.throws(prepared.commit, /Draft changed/);
    } finally {
      dispose();
    }
  },
);

Deno.test("bulk preparation rejects stale edits and cancellation without publishing", async () => {
  const tracker = new ChangeTracker(fixture());
  const array = tracker.data[0].rules;
  const prepared = await tracker.preparePrepend(array, [{ id: "new", rule: "new.test" }]);
  tracker.data[1].name = "Concurrent";
  assert.throws(prepared.validate, /Draft changed/);
  assert.throws(prepared.commit, /Draft changed/);
  assert.equal(array.length, 1);
  assert.equal(tracker.data[1].name, "Concurrent");
  const controller = new AbortController();
  const cancelled = await tracker.preparePrepend(
    array,
    [{ id: "cancelled", rule: "cancelled.test" }],
    { signal: controller.signal },
  );
  controller.abort();
  assert.throws(cancelled.commit, { name: "AbortError" });
  assert.equal(array.length, 1);
});

Deno.test("bulk preparation rejects mutations and cancellation while still building", async () => {
  const tracker = new ChangeTracker(fixture());
  const array = tracker.data[0].rules;
  const incoming = Array.from({ length: 5000 }, (_, index) => ({
    id: `new-${index}`,
    rule: `new-${index}.test`,
  }));
  const stale = tracker.preparePrepend(array, incoming, { budgetMs: 0 });
  tracker.data[1].name = "Concurrent";
  await assert.rejects(stale, /Draft changed/);
  assert.equal(array.length, 1);
  const controller = new AbortController();
  const cancelled = tracker.preparePrepend(array, incoming, {
    budgetMs: 0,
    signal: controller.signal,
  });
  controller.abort();
  await assert.rejects(cancelled, { name: "AbortError" });
  assert.equal(array.length, 1);
  assert.equal(tracker.data[1].name, "Concurrent");
});

Deno.test("atomic replacement reorders 100k rules without losing identity or saved order", () => {
  const rules = Array.from({ length: 100000 }, (_, index) => ({
    id: `rule-${index}`,
    rule: `item-${index}.test`,
  }));
  const tracker = new ChangeTracker([{ id: "group", rules }]);
  const array = tracker.data[0].rules;
  const original = array.slice();
  let first = "";
  let publications = 0;
  const dispose = effect_root(() => {
    render_effect(() => {
      first = tracker.data[0].rules[0].rule;
      publications++;
    });
  });
  try {
    tracker.replaceArray(array, original.slice().reverse());
    flush();
    assert.equal(first, "item-99999.test");
    assert.equal(array.length, 100000);
    assert.equal(tracker.data[0].rules, array);
    assert.equal(array.at(-1), original[0]);
    assert.equal(tracker.isDirty, true);
    assert.equal(publications, 2);
    tracker.replaceArray(array, original);
    flush();
    assert.equal(first, "item-0.test");
    assert.equal(tracker.isDirty, false);
    assert.equal(publications, 3);
    tracker.replaceArray(array, original);
    flush();
    assert.equal(publications, 3);
    array[0].rule = "edited.test";
    flush();
    assert.equal(first, "edited.test");
    array[0].rule = "item-0.test";
    assert.equal(tracker.isDirty, false);
  } finally {
    dispose();
  }
});
