import { strict as assert } from "node:assert";

import { ImportPlannerSession } from "../../src/modules/config-transfer/import-planner";
import { type Snapshot } from "../../src/modules/config-transfer/model";

const group = (id: string, name: string) => ({
  id,
  name,
  interface: "blackhole",
  color: "#ffffff",
  priority: 300,
  enable: true,
  rules: [{ id: "11223344", name: "", rule: "example.test", type: "namespace", enable: true }],
});
const current = (): Snapshot => ({
  groups: [group("aabbccdd", "Saved")],
  subscriptions: [],
  profiles: [],
  settings: {},
});

Deno.test("worker session returns small metadata and applies only the newest selection", () => {
  const baseline = current();
  const incoming = { groups: [group("55667788", "A"), group("99aabbcc", "B")] };
  const original = JSON.stringify({ baseline, incoming });
  const session = new ImportPlannerSession(baseline, incoming);
  const first = session.preview({ version: 1, mode: "replace", excluded: [], choices: [] });
  assert.equal(first.next.groups.length, 2);
  assert.equal("rules" in first.next.groups[0], false);
  session.preview({ version: 2, mode: "replace", excluded: ["groups:1"], choices: [] });
  assert.throws(() => session.apply(1), /changed/);
  const plan = session.apply(2);
  assert.deepEqual(
    plan.next.groups.map((item) => item.name),
    ["A"],
  );
  assert.equal(plan.next.groups[0].rules.length, 1);
  assert.equal(JSON.stringify({ baseline, incoming }), original);
  plan.next.groups[0].rules[0].name = "Detached";
  assert.equal(incoming.groups[0].rules[0].name, "");
});

Deno.test("worker session keeps allocated copy IDs stable while preview choices change", () => {
  const baseline = current();
  const session = new ImportPlannerSession(baseline, { groups: [group("aabbccdd", "Incoming")] });
  session.preview({
    version: 1,
    mode: "merge",
    excluded: [],
    choices: [["groups:aabbccdd", "copy"]],
  });
  const first = session.apply(1).next.groups[1];
  session.preview({
    version: 2,
    mode: "merge",
    excluded: [],
    choices: [["groups:aabbccdd", "keep"]],
  });
  session.preview({
    version: 3,
    mode: "merge",
    excluded: [],
    choices: [["groups:aabbccdd", "copy"]],
  });
  const next = session.apply(3).next.groups[1];
  assert.equal(next.id, first.id);
  assert.equal(next.rules[0].id, first.rules[0].id);
  assert.notEqual(next.id, baseline.groups[0].id);
  assert.notEqual(next.rules[0].id, baseline.groups[0].rules[0].id);
});
