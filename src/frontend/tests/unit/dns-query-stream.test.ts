import assert from "node:assert/strict";
import {
  applyDnsEvent,
  createEventParser,
  subscribeDnsQueries,
  type StreamEvent,
} from "../../src/modules/settings/dns-query-stream.ts";

const row = (id = 1) => ({ id, time_ms: 123456, client: "192.168.1.10", domain: "example.org", qtype: 1, protocol: "udp" as const });
const snapshot = () => ({ event: "snapshot", data: JSON.stringify({ queries: [row()], capacity: 1000 }) });
const frame = (event: StreamEvent) => `event: ${event.event}\ndata: ${event.data}\n\n`;
const tick = () => new Promise((resolve) => setTimeout(resolve, 0));

Deno.test("SSE handles every split, CRLF, multiline data and comments", () => {
  const wire = ": comment\r\nevent: query\r\ndata: first\r\ndata: second\r\n\r\nevent: clear\rdata: {}\r\r";
  for (let split = 0; split <= wire.length; split++) {
    const events: StreamEvent[] = [];
    const parse = createEventParser((event) => events.push(event));
    parse(wire.slice(0, split)); parse(""); parse(wire.slice(split));
    assert.deepEqual(events, [{ event: "query", data: "first\nsecond" }, { event: "clear", data: "{}" }]);
  }
  const events: StreamEvent[] = [];
  const parse = createEventParser((event) => events.push(event));
  for (const char of wire) parse(char);
  assert.equal(events.length, 2);
  parse("event: query\ndata: incomplete");
  assert.equal(events.length, 2, "EOF never dispatches a partial event");
});

Deno.test("SSE decoder preserves split UTF-8 and rejects oversized events", () => {
  const events: StreamEvent[] = [];
  const parse = createEventParser((event) => events.push(event));
  const decode = new TextDecoder();
  for (const byte of new TextEncoder().encode("data: пример.рф\n\n")) {
    parse(decode.decode(Uint8Array.of(byte), { stream: true }));
  }
  assert.equal(events[0].data, "пример.рф");
  assert.throws(() => parse("x".repeat(2 * 1024 * 1024 + 1)));
});

Deno.test("DNS SSE snapshots replace, queries deduplicate, clear resets and history stays bounded", () => {
  let state = applyDnsEvent({ queries: [], capacity: 1000 }, snapshot());
  state = applyDnsEvent(state, { event: "query", data: JSON.stringify(row()) });
  assert.equal(state.queries.length, 1);
  for (let id = 2; id <= 1010; id++) state = applyDnsEvent(state, { event: "query", data: JSON.stringify(row(id)) });
  assert.equal(state.queries.length, 1000);
  assert.equal(state.queries[0].id, 1010);
  assert.equal(state.queries.at(-1)?.id, 11);
  state = applyDnsEvent(state, { event: "clear", data: "{}" });
  assert.equal(state.queries.length, 0);
  state = applyDnsEvent(state, snapshot());
  assert.deepEqual(state.queries, [row()]);
  assert.throws(() => applyDnsEvent(state, { event: "query", data: "{}" }));
  assert.throws(() => applyDnsEvent(state, { event: "snapshot", data: "{}" }));
});

Deno.test("one authenticated fetch delivers multiple events; stopping cancels its reader", async () => {
  let reads = 0;
  let aborted = false;
  let stream!: ReadableStreamDefaultController<Uint8Array>;
  const events: StreamEvent[] = [];
  const fakeFetch: typeof fetch = async (url, init) => {
    reads++;
    assert.equal(url, "/api/v1/system/dns/stream");
    assert.equal(new Headers(init?.headers).get("Authorization"), "Bearer test-token");
    return new Response(new ReadableStream<Uint8Array>({
      start(controller) {
        stream = controller;
        init?.signal?.addEventListener("abort", () => { aborted = true; controller.error(new Error("aborted")); }, { once: true });
      },
    }), { headers: { "Content-Type": "text/event-stream" } });
  };
  let unauthorized = false;
  const stop = subscribeDnsQueries({ url: "/api/v1/system/dns/stream", token: "test-token", onEvent: (event) => events.push(event), onState: () => {}, onUnauthorized: () => (unauthorized = true), fetch: fakeFetch });
  try {
    await tick();
    stream.enqueue(new TextEncoder().encode(frame(snapshot())));
    await tick();
    stream.enqueue(new TextEncoder().encode(frame({ event: "query", data: JSON.stringify(row(2)) })));
    await tick();
    assert.equal(reads, 1);
    assert.equal(events.length, 2);
    assert.equal(unauthorized, false);
  } finally { stop(); await tick(); }
  assert.equal(aborted, true, "cleanup aborts the open HTTP response");
});

Deno.test("401 ends the subscription and never falls back to unauthenticated polling", async () => {
  let calls = 0;
  let unauthorized = 0;
  const stop = subscribeDnsQueries({ url: "/stream", token: "expired", onEvent: () => assert.fail("unexpected event"), onState: () => {}, onUnauthorized: () => unauthorized++, fetch: async () => { calls++; return new Response(null, { status: 401 }); } });
  await tick();
  stop();
  assert.equal(unauthorized, 1);
  assert.equal(calls, 1);
});
