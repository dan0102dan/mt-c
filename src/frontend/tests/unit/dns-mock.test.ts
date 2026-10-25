import assert from "node:assert/strict";
import { Hono } from "hono";

import { registerMockDnsRoutes } from "../../dev/dns-mock.ts";
import { parseCaptureState } from "../../src/modules/settings/dns/history.ts";

const base = "/api/v1/system/dns";
function fixture() {
  const app = new Hono();
  registerMockDnsRoutes(app);
  const capture = async () =>
    parseCaptureState(await (await app.request(`${base}/capture`)).json());
  const save = (body: unknown) =>
    app.request(`${base}/capture`, {
      method: "PUT",
      body: JSON.stringify(body),
      headers: { "Content-Type": "application/json" },
    });
  return { app, capture, save };
}

Deno.test("DNS mock supports explicit capture, idempotent start, clear and stop", async () => {
  const { app, capture, save } = fixture();
  assert.equal((await capture()).enabled, false);
  assert.deepEqual((await (await app.request(`${base}/queries`)).json()).queries, []);
  const started = await save({ enabled: true });
  assert.equal(started.status, 200);
  const state = parseCaptureState(await started.json());
  assert.equal(state.enabled, true);
  assert.ok(state.count > 0);
  assert.ok(state.memory_used > 0);
  assert.equal(state.memory_limit, 1024 * 1024);
  assert.deepEqual(parseCaptureState(await (await save({ enabled: true })).json()), state);
  const page = await (await app.request(`${base}/queries?limit=2`)).json();
  assert.equal(page.queries.length, 2);
  assert.ok(page.next_before > 0);
  const next = await (
    await app.request(`${base}/queries?before=${page.next_before}&limit=2`)
  ).json();
  assert.equal(next.queries.length, 2);
  assert.ok(next.queries.every((row: { id: number }) => row.id < page.next_before));
  const cleared = await (await app.request(`${base}/queries`, { method: "DELETE" })).json();
  assert.equal(cleared.cleared, true);
  assert.equal(parseCaptureState(cleared).enabled, true);
  assert.equal(cleared.count, 0);
  assert.equal((await capture()).count, 0);
  assert.equal((await save({ enabled: false })).status, 200);
  const stopped = await capture();
  assert.equal(stopped.enabled, false);
  assert.equal(stopped.memory_used, 0);
  assert.equal(stopped.memory_limit, 0);
  assert.equal(stopped.count, 0);
});

Deno.test("DNS mock rejects invalid capture requests without changing state", async () => {
  const { app, capture, save } = fixture();
  for (const body of [null, [], {}, { enabled: "true" }, { enabled: true, extra: 1 }])
    assert.equal((await save(body)).status, 400);
  for (const body of ["{", " ".repeat(129) + '{"enabled":true}'])
    assert.equal((await app.request(`${base}/capture`, { method: "PUT", body })).status, 400);
  assert.equal((await capture()).enabled, false);
  for (const query of ["limit=0", "limit=129", "before=-1", "before=abc"])
    assert.equal((await app.request(`${base}/queries?${query}`)).status, 400);
});

Deno.test("DNS mock streams capture changes to connected viewers", async () => {
  const { app, capture, save } = fixture();
  const response = await app.request(`${base}/stream`);
  const reader = response.body!.getReader();
  const decoder = new TextDecoder();
  let buffer = "";
  async function until(marker: string) {
    while (!buffer.includes(marker)) {
      const chunk = await reader.read();
      assert.equal(chunk.done, false);
      buffer += decoder.decode(chunk.value, { stream: true });
    }
    const result = buffer;
    buffer = "";
    return result;
  }
  try {
    const initial = await until("event: snapshot_end");
    assert.match(initial, /event: capture/);
    assert.match(initial, /"enabled":false/);
    assert.equal((await capture()).enabled, false);
    const started = parseCaptureState(await (await save({ enabled: true })).json());
    assert.match(await until("event: query"), /"enabled":true/);
    // Drain all queued sample rows before checking the next transition.
    await until(`"id":${started.count},`);
    await app.request(`${base}/queries`, { method: "DELETE" });
    assert.match(await until("event: clear"), /"count":0/);
    await save({ enabled: false });
    assert.match(await until("event: capture"), /"enabled":false/);
  } finally {
    await reader.cancel();
  }
});

Deno.test("DNS mock releases subscriber slots when requests are aborted", async () => {
  const { app } = fixture();
  const readers: ReadableStreamDefaultReader<Uint8Array>[] = [];
  const controllers: AbortController[] = [];
  try {
    for (let i = 0; i < 8; i++) {
      const controller = new AbortController();
      controllers.push(controller);
      const response = await app.request(`${base}/stream`, { signal: controller.signal });
      assert.equal(response.status, 200);
      const reader = response.body!.getReader();
      readers.push(reader);
      await reader.read();
    }
    const excess = await app.request(`${base}/stream`);
    assert.equal(excess.status, 503);
    await excess.body?.cancel();
    for (const controller of controllers) controller.abort();
    const replacement = await app.request(`${base}/stream`);
    readers.push(replacement.body!.getReader());
    assert.equal(replacement.status, 200);
  } finally {
    for (const reader of readers) await reader.cancel();
  }
});

Deno.test("DNS mock history covers multiple pages, dates, devices and record types", async () => {
  const { app, save } = fixture();
  const state = parseCaptureState(await (await save({ enabled: true })).json());
  const rows: Array<{
    id: number;
    time_ms: number;
    client: string;
    domain: string;
    qtype: number;
    protocol: string;
  }> = [];
  let before = 0;
  do {
    const page = await (
      await app.request(`${base}/queries?limit=128${before ? `&before=${before}` : ""}`)
    ).json();
    rows.push(...page.queries);
    before = page.next_before;
  } while (before);
  assert.equal(rows.length, state.count);
  assert.ok(rows.length > 128);
  assert.equal(new Set(rows.map((row) => row.id)).size, rows.length);
  assert.ok(rows[0].time_ms - rows.at(-1)!.time_ms > 48 * 60 * 60 * 1000);
  assert.ok(new Set(rows.map((row) => row.client)).size >= 6);
  assert.ok(new Set(rows.map((row) => row.domain)).size > 30);
  assert.ok(new Set(rows.map((row) => row.qtype)).size > 3);
  assert.deepEqual(new Set(rows.map((row) => row.protocol)), new Set(["udp", "tcp"]));
  assert.ok(rows.every((row, index) => !index || row.id < rows[index - 1].id));
});
