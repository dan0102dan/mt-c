import assert from "node:assert/strict";

import { createDnsHistory } from "../../src/modules/settings/dns/history.ts";
import {
  createSnapshotReceiver,
  subscribeDnsQueries,
  type StreamEvent,
} from "../../src/modules/settings/dns/stream.ts";

const event = (name: string, value: unknown): StreamEvent => ({
  event: name,
  data: JSON.stringify(value),
});
const row = (id: number) => ({
  id,
  time_ms: id,
  client: "10.0.0.1",
  domain: "example.org",
  qtype: 1,
  protocol: "udp",
});
const history = (capacity = 1000) =>
  createDnsHistory({ queries: [], capacity, capture: null, clientNames: new Map() });
const capture = (enabled: boolean, capacity = 1000) =>
  event("capture", { enabled, capacity, count: 0, memory_used: 256, memory_limit: 1048576 });

Deno.test("ingest ring retains bounded rows and does not mutate published views", () => {
  const h = history();
  h.ingest(capture(true));
  for (let i = 1; i <= 1000; i++) h.ingest(event("query", row(i)));
  const old = h.view();
  for (let i = 1001; i <= 100000; i++) h.ingest(event("query", row(i)));
  const current = h.view();
  assert.equal(current.queries.length, 1000);
  assert.equal(current.queries[0].id, 100000);
  assert.equal(current.queries.at(-1)?.id, 99001);
  assert.equal(old.queries[0].id, 1000);
  assert.equal(old.queries.at(-1)?.id, 1);
  assert.strictEqual(current.queries, h.view().queries, "no copy without new rows");
});

Deno.test("byte eviction, duplicates, clear and off preserve sequence semantics", () => {
  const h = history(8);
  h.ingest(capture(true, 8));
  for (let id = 1; id <= 8; id++) h.ingest(event("query", row(id)));
  h.ingest(event("query", { ...row(9), retained_from: 7, memory_used: 1024 }));
  assert.deepEqual(
    h.view().queries.map((q) => q.id),
    [9, 8, 7],
  );
  const retained = h.view().queries;
  h.ingest(event("query", row(8)));
  assert.strictEqual(h.view().queries, retained);
  assert.throws(() => h.ingest(event("query", row(11))), /Gap/);
  h.ingest(event("clear", {}));
  assert.equal(h.view().queries.length, 0);
  h.ingest(event("query", row(10)));
  assert.equal(h.view().queries.length, 1);
  h.ingest(capture(false, 8));
  h.ingest(event("query", row(11)));
  assert.equal(h.view().queries.length, 0);
  h.ingest(capture(true, 8));
  h.ingest(event("query", row(11)));
  assert.equal(h.view().queries[0].id, 11);
});

Deno.test("alias updates replace metadata without copying or replacing query history", () => {
  const h = history();
  h.ingest(event("query", row(1)));
  const rows = h.view().queries;
  h.ingest({ event: "clients", data: "", clients: [{ ip: "10.0.0.1", name: "Phone" }] });
  const first = h.view();
  h.ingest({ event: "clients", data: "", clients: [{ ip: "10.0.0.1", name: "Renamed" }] });
  assert.strictEqual(h.view().queries, rows);
  assert.equal(h.view().clientNames.get("10.0.0.1"), "Renamed");
  assert.equal(first.clientNames.get("10.0.0.1"), "Phone");
  h.ingest({ event: "clients", data: "", clients: [] });
  assert.equal(h.view().clientNames.size, 0);
});

Deno.test("legacy labels cannot retain clients after their last row is evicted", () => {
  const h = history(8);
  for (let id = 1; id <= 10000; id++) {
    h.ingest(
      event("query", {
        ...row(id),
        client: `10.${(id >> 16) & 255}.${(id >> 8) & 255}.${id & 255}`,
        client_name: `Device ${id}`,
      }),
    );
  }
  assert.equal(h.view().clientNames.size, 8);
  h.ingest(event("clear", {}));
  assert.equal(h.view().clientNames.size, 0);
});

Deno.test("bounded alias chunks commit atomically while a DNS snapshot is staged", () => {
  const receive = createSnapshotReceiver();
  receive(event("snapshot_begin", { capacity: 1000, count: 1, through: 1 }));
  assert.equal(receive(event("clients_begin", { count: 1 })), null);
  assert.equal(
    receive(
      event("clients_chunk", { offset: 0, clients: [{ ip: "::ffff:10.0.0.1", name: "Телефон" }] }),
    ),
    null,
  );
  const clients = receive(event("clients_end", {}));
  assert.deepEqual(clients?.clients, [{ ip: "10.0.0.1", name: "Телефон" }]);
  receive(event("snapshot_chunk", { offset: 0, queries: [row(1)] }));
  assert.deepEqual(receive(event("snapshot_end", { through: 1 }))?.snapshot?.queries, [row(1)]);
  for (const bad of [
    event("clients_begin", { count: 2049 }),
    event("clients_begin", { count: -1 }),
    event("clients_chunk", { offset: 0, clients: [] }),
    event("clients_end", {}),
  ])
    assert.throws(() => createSnapshotReceiver()(bad));
  receive(event("clients_begin", { count: 2 }));
  assert.throws(() =>
    receive(
      event("clients_chunk", {
        offset: 0,
        clients: [
          { ip: "10.0.0.1", name: "a" },
          { ip: "::ffff:10.0.0.1", name: "b" },
        ],
      }),
    ),
  );
});

Deno.test(
  "optional journal unavailable response stops retries instead of hiding forwarding health",
  async () => {
    let calls = 0;
    const states: string[] = [];
    const stop = subscribeDnsQueries({
      url: "/stream",
      token: "x",
      onEvent: () => assert.fail("event"),
      onUnauthorized: () => assert.fail("auth"),
      onState: (s) => states.push(s),
      fetch: async () => {
        calls++;
        return new Response(null, { status: 503, headers: { "X-DNS-Log-Available": "0" } });
      },
    });
    await new Promise((r) => setTimeout(r, 10));
    stop();
    assert.equal(calls, 1);
    assert.equal(states.at(-1), "unavailable");
  },
);
