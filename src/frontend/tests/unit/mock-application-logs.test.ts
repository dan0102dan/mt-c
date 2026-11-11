import { strict as assert } from "node:assert";
import { Hono } from "hono";

import { registerMockLogRoutes } from "../../dev/logs-mock.ts";

const BASE = "/api/v1/logs";
function fixture(initialLevel = "info") {
  const app = new Hono();
  let level = initialLevel;
  const journal = registerMockLogRoutes(app, {
    getLogLevel: () => level,
    getRoutes: () => [{ name: "Test VPN", interface: "wg0" }],
    intervalMs: 60000, // Prevent wall-clock-dependent traffic in unit tests.
  });
  const capture = async () => {
    const res = await app.request(`${BASE}/capture`);
    assert.equal(res.status, 200);
    return res.json();
  };
  const set = async (enabled: boolean) => {
    const res = await app.request(`${BASE}/capture`, {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ enabled }),
    });
    assert.equal(res.status, 200);
    return res.json();
  };
  return { app, journal, capture, set, updateLevel: (next: string) => (level = next) };
}

function eventsFrom(response: Response) {
  assert.equal(response.status, 200);
  assert.match(response.headers.get("Content-Type") ?? "", /text\/event-stream/);
  assert(response.body);
  const reader = response.body.getReader();
  const decoder = new TextDecoder();
  let buffered = "";
  return {
    async next() {
      for (;;) {
        const end = buffered.indexOf("\n\n");
        if (end >= 0) {
          const frame = buffered.slice(0, end);
          buffered = buffered.slice(end + 2);
          if (frame.startsWith(":")) continue;
          const event = frame.match(/^event: ([^\n]+)/m)?.[1];
          const data = frame.match(/^data: (.+)$/m)?.[1];
          assert(event && data, `Invalid SSE frame ${frame}`);
          return { event, data: JSON.parse(data) as Record<string, unknown> };
        }
        const chunk = await reader.read();
        assert(!chunk.done && chunk.value, "SSE ended unexpectedly");
        buffered += decoder.decode(chunk.value, { stream: true });
      }
    },
    close: () => reader.cancel(),
  };
}

Deno.test("dev logs mock is disabled by default and only Start produces requests", async () => {
  const f = fixture();
  try {
    const state = await f.capture();
    assert.deepEqual(state, { enabled: false, session: "", through: "0", dropped: "0" });
    f.journal.generateRequest();
    assert.equal((await f.capture()).through, "0");
    const invalids = [{}, { enabled: "true" }, { enabled: true, foo: true }];
    for (const invalid of invalids) {
      const res = await f.app.request(`${BASE}/capture`, {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(invalid),
      });
      assert.equal(res.status, 400);
    }
    const started = await f.set(true);
    assert.match(started.session, /^[a-f0-9]{32}$/);
    assert.equal(started.through, "1");
    assert.deepEqual(await f.set(true), started, "Start is idempotent");
    f.journal.generateRequest();
    assert.equal((await f.capture()).through, "2");
    assert.deepEqual(await f.set(false), {
      enabled: false,
      session: "",
      through: "0",
      dropped: "0",
    });
    f.journal.generateRequest();
    assert.equal((await f.capture()).through, "0");
    assert.notEqual((await f.set(true)).session, started.session, "Start creates a new session");
  } finally {
    f.journal.close();
  }
});

Deno.test("dev SSE replays history, then streams live records and resumes by cursor", async () => {
  const f = fixture();
  try {
    const start = await f.set(true);
    f.journal.generateRequest();
    f.journal.generateRequest(); // debug is suppressed at info by config
    assert.equal((await f.capture()).through, "2");
    const first = eventsFrom(await f.app.request(`${BASE}/stream`));
    try {
      assert.equal((await first.next()).event, "capture");
      for (let id = 1; id <= 2; id++) {
        const frame = await first.next();
        assert.equal(frame.event, "log");
        assert.equal(frame.data.session, start.session);
        assert.equal(frame.data.id, String(id));
        assert.equal(frame.data.level, 2);
        const raw = atob(String(frame.data.data));
        assert.match(raw, /INF mock dns request id=/);
        assert.match(raw, /group=Test VPN interface=wg0/);
      }
      const ready = await first.next();
      assert.equal(ready.event, "ready");
      assert.equal(ready.data.through, "2");
      f.journal.generateRequest();
      const live = await first.next();
      assert.equal(live.event, "log");
      assert.equal(live.data.id, "3");
    } finally {
      await first.close();
    }
    const second = eventsFrom(
      await f.app.request(`${BASE}/stream?session=${start.session}&after=3`),
    );
    try {
      assert.equal((await second.next()).event, "capture");
      const ready = await second.next();
      assert.equal(ready.event, "ready");
      assert.equal(ready.data.through, "3");
      f.journal.generateRequest(); // warning request, id 5, delivered as journal record 4
      const next = await second.next();
      assert.equal(next.event, "log");
      assert.equal(next.data.id, "4");
      assert.equal(next.data.level, 3);
    } finally {
      await second.close();
    }
  } finally {
    f.journal.close();
  }
});

Deno.test("dev log detail level is enforced by the current mock config", async () => {
  const f = fixture("warn");
  try {
    assert.equal((await f.set(true)).through, "0");
    for (let i = 0; i < 3; i++) f.journal.generateRequest();
    assert.equal((await f.capture()).through, "0");
    f.journal.generateRequest(); // fifth demo request emits WARN
    assert.equal((await f.capture()).through, "1");
    f.updateLevel("disabled");
    for (let i = 0; i < 20; i++) f.journal.generateRequest();
    assert.equal((await f.capture()).through, "1");
    f.updateLevel("trace");
    f.journal.generateRequest();
    assert.equal((await f.capture()).through, "2");
  } finally {
    f.journal.close();
  }
});

Deno.test("dev stream validates session/cursor without starting capture", async () => {
  const f = fixture();
  try {
    for (const path of [
      `${BASE}/stream?session=abc&after=1`,
      `${BASE}/stream?session=${"a".repeat(32)}`,
      `${BASE}/stream?after=1`,
      `${BASE}/stream?session=${"a".repeat(32)}&after=01`,
    ]) {
      assert.equal((await f.app.request(path)).status, 400);
    }
    const start = await f.set(true);
    assert.equal(
      (await f.app.request(`${BASE}/stream?session=${start.session}&after=2`)).status,
      400,
    );
    assert.equal((await f.capture()).enabled, true);
  } finally {
    f.journal.close();
  }
});
