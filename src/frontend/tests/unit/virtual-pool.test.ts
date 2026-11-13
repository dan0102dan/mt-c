import { strict as assert } from "node:assert";

import { VirtualRowPool } from "../../src/lib/virtual/pool";

Deno.test("virtual row pool preserves logical slots and reuses departed rows", () => {
  const pool = new VirtualRowPool();
  const initial = pool.assign(["a", "b", "c"], () => true);
  const next = pool.assign(["b", "c", "d"], () => true);
  assert.equal(next[0], initial[1]);
  assert.equal(next[1], initial[2]);
  assert.equal(next[2], initial[0]);
  const scrolled = pool.assign(["x", "y", "z"], () => true);
  assert.deepEqual([...scrolled].sort(), [...initial].sort());
});

Deno.test("virtual row pool never recycles a protected editor or portal anchor", () => {
  const pool = new VirtualRowPool();
  const initial = pool.assign(["a", "b"], () => true);
  const next = pool.assign(["b", "c"], (slot) => slot !== initial[0]);
  assert.equal(next[0], initial[1]);
  assert.notEqual(next[1], initial[0]);
  const unprotected = pool.assign(["b", "c", "d"], () => true);
  assert.equal(unprotected[2], initial[0]);
});

Deno.test("disjoint jumps retain DOM order after scrolling in either direction", () => {
  const pool = new VirtualRowPool();
  pool.assign(["a", "b", "c"], () => true);
  pool.assign(["b", "c", "d"], () => true);
  const backwards = pool.assign(["a", "b", "c"], () => true);
  assert.deepEqual(
    pool.assign(["x", "y", "z"], () => true),
    backwards,
  );
});
