import assert from "node:assert/strict";

import { parseCaptureState } from "../../src/modules/settings/dns-capture";
import { createDnsHistory } from "../../src/modules/settings/dns-history";
import {
  DNS_HISTORY_MEMORY_LIMIT,
  MAX_DNS_CAPACITY,
} from "../../src/modules/settings/dns-query-limits";
import {
  createSnapshotReceiver,
  type StreamEvent,
} from "../../src/modules/settings/dns-query-stream";

const event = (name: string, value: unknown): StreamEvent => ({
  event: name,
  data: JSON.stringify(value),
});
const row = (id: number) => ({
  id,
  time_ms: id,
  client: "192.0.2.1",
  domain: "example.org",
  qtype: 1,
  protocol: "udp",
});
Deno.test("byte-derived validation accepts histories past both obsolete row ceilings", () => {
  const capture = {
    enabled: true,
    memory_limit: DNS_HISTORY_MEMORY_LIMIT,
    memory_used: DNS_HISTORY_MEMORY_LIMIT,
    count: 100000,
    capacity: MAX_DNS_CAPACITY,
  };
  assert.equal(parseCaptureState(capture).count, 100000);
  assert.throws(() => parseCaptureState({ ...capture, capacity: MAX_DNS_CAPACITY + 1 }));
  assert.throws(() => parseCaptureState({ ...capture, memory_used: DNS_HISTORY_MEMORY_LIMIT + 1 }));
  assert.throws(() =>
    parseCaptureState({ ...capture, memory_limit: DNS_HISTORY_MEMORY_LIMIT + 1 }),
  );
});
Deno.test("large chunked snapshot and live byte eviction preserve every retained ID", () => {
  const h = createDnsHistory({ queries: [], capacity: 1, capture: null, clientNames: new Map() });
  const receive = createSnapshotReceiver();
  h.ingest(
    event("capture", {
      enabled: true,
      memory_limit: DNS_HISTORY_MEMORY_LIMIT,
      memory_used: DNS_HISTORY_MEMORY_LIMIT,
      count: 100000,
      capacity: MAX_DNS_CAPACITY,
    }),
  );
  receive(event("snapshot_begin", { count: 100000, through: 100000, capacity: MAX_DNS_CAPACITY }));
  for (let offset = 0; offset < 100000; offset += 32) {
    assert.equal(
      receive(
        event("snapshot_chunk", {
          offset,
          queries: Array.from({ length: Math.min(32, 100000 - offset) }, (_, i) =>
            row(offset + i + 1),
          ),
        }),
      ),
      null,
    );
  }
  const complete = receive(event("snapshot_end", { through: 100000 }));
  assert(complete);
  h.ingest(complete);
  const first = h.view();
  assert.equal(first.queries.length, 100000);
  for (let i = 0; i < 100000; i++) assert.equal(first.queries[i].id, 100000 - i);
  h.ingest(event("query", { ...row(100001), retained_from: 409 }));
  const next = h.view();
  assert.equal(next.queries.length, 99593);
  assert.equal(next.queries[0].id, 100001);
  assert.equal(next.queries.at(-1)?.id, 409);
  assert.equal(first.queries.length, 100000, "published snapshot stays immutable");
});
