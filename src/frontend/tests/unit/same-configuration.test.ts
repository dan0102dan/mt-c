import { strict as assert } from "node:assert";

import { fingerprint, sameConfiguration } from "../../src/modules/config-transfer/model";

Deno.test(
  "configuration equality preserves canonical fingerprint semantics for JSON drafts",
  () => {
    const values: unknown[] = [
      undefined,
      null,
      false,
      true,
      0,
      1,
      "",
      "0",
      [],
      {},
      { missing: undefined },
      [null],
      ["wg0", "wg1"],
      ["wg1", "wg0"],
      { name: "VPN", interfaces: ["wg0", "wg1"], optional: undefined },
      { interfaces: ["wg0", "wg1"], name: "VPN" },
      { name: "VPN", interfaces: ["wg1", "wg0"] },
      { rules: [{ id: "11223344", enable: true, rule: "example.test" }] },
      { rules: [{ rule: "example.test", enable: true, id: "11223344" }] },
      { rules: [{ id: "11223344", enable: false, rule: "example.test" }] },
      { overrides: [] },
      { overrides: [{ rule: "example.test", enable: false }] },
    ];
    for (const left of values) {
      for (const right of values) {
        assert.equal(
          sameConfiguration(left, right),
          fingerprint(left) === fingerprint(right),
          `${fingerprint(left)} compared with ${fingerprint(right)}`,
        );
      }
    }
  },
);

Deno.test("configuration equality detects a deep edit without mutating either draft", () => {
  const left = {
    groups: [{ id: "11223344", rules: [{ id: "aabbccdd", enable: true }] }],
    settings: { keeneticPolicy: { enabled: false, mode: "include" } },
  };
  const right = structuredClone(left);
  assert.equal(sameConfiguration(left, right), true);
  right.groups[0].rules[0].enable = false;
  const before = fingerprint(right);
  assert.equal(sameConfiguration(left, right), false);
  assert.equal(fingerprint(right), before);
  assert.equal(left.groups[0].rules[0].enable, true);
});
