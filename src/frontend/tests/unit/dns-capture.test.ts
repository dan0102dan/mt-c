import assert from "node:assert/strict";

import type { DnsQuery } from "../../src/modules/settings/dns/data.ts";
import { applyCaptureHistory, parseCaptureState } from "../../src/modules/settings/dns/history.ts";

const row = (id: number): DnsQuery => ({
  id,
  time_ms: id,
  client: "192.0.2.1",
  domain: "example.org",
  qtype: 1,
  protocol: "udp",
});
Deno.test("capture state validates byte budget and actual enabled state", () => {
  const disabled = {
    enabled: false,
    count: 0,
    capacity: 8192,
    memory_used: 400,
    memory_limit: 1048576,
  };
  assert.deepEqual(parseCaptureState(disabled), disabled);
  assert.throws(() => parseCaptureState({ ...disabled, enabled: "false" }));
  assert.throws(() => parseCaptureState({ ...disabled, memory_used: 1048577 }));
  assert.throws(() => parseCaptureState({ ...disabled, count: 8193 }));
});
Deno.test("UI retention follows byte-pressure eviction before the slot limit", () => {
  const before = { queries: [row(3), row(2), row(1)], capacity: 8192 };
  const event = { event: "query", data: JSON.stringify({ ...row(4), retained_from: 3 }) };
  const after = applyCaptureHistory(before, event);
  assert.deepEqual(
    after.queries.map((q) => q.id),
    [4, 3],
  );
  assert.deepEqual(
    before.queries.map((q) => q.id),
    [3, 2, 1],
  );
  assert.throws(() =>
    applyCaptureHistory(before, {
      ...event,
      data: JSON.stringify({ ...row(4), retained_from: 5 }),
    }),
  );
});
