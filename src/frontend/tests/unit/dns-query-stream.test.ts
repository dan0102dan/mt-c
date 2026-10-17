import assert from "node:assert/strict";
import {
  applyDnsEvent, createEventParser, createSnapshotReceiver, subscribeDnsQueries,
  MAX_EVENT_SIZE, type StreamEvent,
} from "../../src/modules/settings/dns-query-stream.ts";

const row = (id = 1) => ({ id, time_ms: 123456, client: "192.168.1.10", domain: "example.org", qtype: 1, protocol: "udp" as const });
const event = (name: string, value: unknown): StreamEvent => ({ event: name, data: JSON.stringify(value) });
const snapshot = () => event("snapshot", { queries: [row()], capacity: 1000 });
const frame = (ev: StreamEvent) => `event: ${ev.event}\ndata: ${ev.data}\n\n`;
const tick = () => new Promise((resolve) => setTimeout(resolve, 0));

Deno.test("SSE handles every split, CRLF, multiline data and comments", () => {
  const wire = ": comment\r\nevent: query\r\ndata: first\r\ndata: second\r\n\r\nevent: clear\rdata: {}\r\r";
  for (let split = 0; split <= wire.length; split++) {
    const events: StreamEvent[] = [];
    const parse = createEventParser((ev) => events.push(ev));
    parse(wire.slice(0, split)); parse(""); parse(wire.slice(split));
    assert.deepEqual(events, [{ event: "query", data: "first\nsecond" }, { event: "clear", data: "{}" }]);
  }
  const events: StreamEvent[] = [];
  const parse = createEventParser((ev) => events.push(ev));
  for (const char of wire) parse(char);
  parse("event: query\ndata: incomplete");
  assert.equal(events.length, 2, "EOF never dispatches a partial event");
});

Deno.test("SSE preserves split UTF-8 and bounds each event, not the full stream", () => {
  const events: StreamEvent[] = [];
  const parse = createEventParser((ev) => events.push(ev));
  const decode = new TextDecoder();
  for (const byte of new TextEncoder().encode("data: пример.рф\n\n")) parse(decode.decode(Uint8Array.of(byte), { stream: true }));
  assert.equal(events[0].data, "пример.рф");
  assert.throws(() => parse("x".repeat(MAX_EVENT_SIZE + 1)));
});

Deno.test("a history exceeding 2 MiB arrives in bounded chunks and commits only at end", () => {
  const receive = createSnapshotReceiver();
  const completed: StreamEvent[] = [];
  const parse = createEventParser((ev) => { const ready = receive(ev); if (ready) completed.push(ready); });
  let bytes = 0;
  const send = (ev: StreamEvent) => {
    const text = frame(ev); bytes += text.length;
    assert(text.length < MAX_EVENT_SIZE);
    for (let i = 0; i < text.length; i += 101) parse(text.slice(i, i + 101));
  };
  send(event("snapshot_begin", { capacity: 3000, count: 3000, through: 3000 }));
  for (let offset = 0; offset < 3000; offset += 32) {
    const queries = Array.from({ length: Math.min(32, 3000 - offset) }, (_, i) => ({ ...row(offset + i + 1), domain: "\\200".repeat(200) }));
    send(event("snapshot_chunk", { offset, queries }));
    assert.equal(completed.length, 0);
  }
  send(event("snapshot_end", { through: 3000 }));
  assert(bytes > 2 * 1024 * 1024);
  assert.equal(completed.length, 1);
  assert.equal(completed[0].data, "", "no re-serialization into one giant event");
  let state = applyDnsEvent({ queries: [], capacity: 1000 }, completed[0]);
  assert.equal(state.capacity, 3000);
  assert.equal(state.queries.length, 3000);
  assert.equal(state.queries[0].id, 3000);
  assert.equal(state.queries.at(-1)?.id, 1);
  state = applyDnsEvent(state, event("query", row(3001)));
  assert.equal(state.queries.at(-1)?.id, 2);
});

Deno.test("clear or a replacement begin discards partial snapshots without mixing generations", () => {
  const receive = createSnapshotReceiver();
  receive(event("snapshot_begin", { capacity: 1000, count: 2, through: 2 }));
  receive(event("snapshot_chunk", { offset: 0, queries: [row(1)] }));
  receive(event("snapshot_begin", { capacity: 1000, count: 1, through: 9 }));
  receive(event("snapshot_chunk", { offset: 0, queries: [row(9)] }));
  const complete = receive(event("snapshot_end", { through: 9 }));
  assert.deepEqual(complete?.snapshot?.queries, [row(9)]);
  receive(event("snapshot_begin", { capacity: 1000, count: 1, through: 10 }));
  assert.equal(receive(event("clear", {}))?.event, "clear");
  assert.throws(() => receive(event("snapshot_end", { through: 10 })));
  receive(event("snapshot_begin", { capacity: 1000, count: 0, through: 10 }));
  assert.deepEqual(receive(event("snapshot_end", { through: 10 }))?.snapshot?.queries, []);
});

Deno.test("malformed, reordered, oversized and incomplete snapshot batches fail closed", () => {
  for (const bad of [
    event("snapshot_chunk", { offset: 1, queries: [row(1)] }),
    event("snapshot_chunk", { offset: 0, queries: [row(2)] }),
    event("snapshot_chunk", { offset: 0, queries: [row(1), row(1)] }),
    event("snapshot_chunk", { offset: 0, queries: Array.from({ length: 33 }, (_, i) => row(i + 1)) }),
    event("snapshot_end", { through: 2 }), event("query", row(3)),
  ]) {
    const receive = createSnapshotReceiver();
    receive(event("snapshot_begin", { capacity: 1000, count: 2, through: 2 }));
    assert.throws(() => receive(bad));
  }
  for (const capacity of [0, -1, 60001, 1.5, null]) {
    assert.throws(() => createSnapshotReceiver()(event("snapshot_begin", { capacity, count: 0, through: 0 })));
  }
});

Deno.test("finite snapshots replace, live queries deduplicate and eviction honors server capacity", () => {
  let state = applyDnsEvent({ queries: [], capacity: 1000 }, snapshot());
  state = applyDnsEvent(state, event("query", row()));
  assert.equal(state.queries.length, 1);
  for (let id = 2; id <= 1010; id++) state = applyDnsEvent(state, event("query", row(id)));
  assert.equal(state.queries.length, 1000);
  assert.equal(state.queries.at(-1)?.id, 11);
  state = applyDnsEvent(state, event("clear", {}));
  assert.equal(state.queries.length, 0);
  assert.throws(() => applyDnsEvent(state, event("query", {})));
  assert.throws(() => applyDnsEvent(state, event("snapshot", {})));
});

Deno.test("one authenticated fetch stages history then delivers live events; stop aborts it", async () => {
  let reads = 0, aborted = false;
  let stream!: ReadableStreamDefaultController<Uint8Array>;
  const events: StreamEvent[] = [];
  const fakeFetch: typeof fetch = async (url, init) => {
    reads++;
    assert.equal(url, "/api/v1/system/dns/stream");
    assert.equal(new Headers(init?.headers).get("Authorization"), "Bearer test-token");
    return new Response(new ReadableStream<Uint8Array>({ start(controller) {
      stream = controller;
      init?.signal?.addEventListener("abort", () => { aborted = true; controller.error(new Error("aborted")); }, { once: true });
    } }), { headers: { "Content-Type": "text/event-stream" } });
  };
  const stop = subscribeDnsQueries({ url: "/api/v1/system/dns/stream", token: "test-token", onEvent: (ev) => events.push(ev), onState: () => {}, onUnauthorized: () => assert.fail("unauthorized"), fetch: fakeFetch });
  const send = async (ev: StreamEvent) => { stream.enqueue(new TextEncoder().encode(frame(ev))); await tick(); };
  try {
    await tick();
    await send(event("snapshot_begin", { capacity: 1000, count: 1, through: 1 }));
    await send(event("snapshot_chunk", { offset: 0, queries: [row()] }));
    assert.equal(events.length, 0);
    await send(event("snapshot_end", { through: 1 }));
    await send(event("query", row(2)));
    assert.equal(reads, 1);
    assert.equal(events.length, 2);
    assert.deepEqual(events[0].snapshot?.queries, [row()]);
  } finally { stop(); await tick(); }
  assert.equal(aborted, true);
});

Deno.test("401 ends the subscription and never falls back to unauthenticated polling", async () => {
  let calls = 0, unauthorized = 0;
  const stop = subscribeDnsQueries({ url: "/stream", token: "expired", onEvent: () => assert.fail("unexpected event"), onState: () => {}, onUnauthorized: () => unauthorized++, fetch: async () => { calls++; return new Response(null, { status: 401 }); } });
  await tick(); stop();
  assert.equal(unauthorized, 1);
  assert.equal(calls, 1);
});
