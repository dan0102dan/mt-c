import type { Hono } from "hono";
import { streamSSE } from "hono/streaming";

const API_BASE = "/api/v1";

export function registerMockDnsRoutes(app: Hono) {
  const sampleQueries: Array<{
    id: number;
    time_ms: number;
    client: string;
    domain: string;
    qtype: number;
    protocol: "udp" | "tcp";
  }> = [
    {
      id: 4,
      time_ms: Date.now() - 2000,
      client: "192.168.1.101",
      domain: "github.com",
      qtype: 1,
      protocol: "udp",
    },
    {
      id: 3,
      time_ms: Date.now() - 7000,
      client: "192.168.1.104",
      domain: "example.org",
      qtype: 28,
      protocol: "udp",
    },
    {
      id: 2,
      time_ms: Date.now() - 12000,
      client: "192.168.1.101",
      domain: "github.com",
      qtype: 65,
      protocol: "tcp",
    },
    {
      id: 1,
      time_ms: Date.now() - 17000,
      client: "192.168.1.103",
      domain: "example.net",
      qtype: 1,
      protocol: "udp",
    },
  ];
  const dnsQueries: typeof sampleQueries = [];
  let enabled = false;
  let dnsThrough = 0;
  const capacity = 1000;
  function captureState() {
    return {
      enabled,
      memory_limit: enabled ? 1024 * 1024 : 0,
      // Fixture estimate; native allocator accounting belongs to C.
      memory_used: dnsQueries.reduce((bytes, row) => bytes + 64 + row.domain.length, 0),
      count: dnsQueries.length,
      capacity,
      dropped: 0,
    };
  }
  const dnsSubscribers = new Set<(event: string, data: unknown) => void>();
  app.get(`${API_BASE}/system/dns/capture`, (c) => {
    c.header("Cache-Control", "no-store");
    return c.json(captureState());
  });
  app.put(`${API_BASE}/system/dns/capture`, async (c) => {
    const text = await c.req.text();
    let body: unknown;
    try {
      body = JSON.parse(text);
    } catch {
      return c.json({ error: "expected a boolean enabled field" }, 400);
    }
    if (
      new TextEncoder().encode(text).length > 128 ||
      !body ||
      typeof body !== "object" ||
      Array.isArray(body) ||
      Object.keys(body).length !== 1 ||
      !("enabled" in body) ||
      typeof body.enabled !== "boolean"
    )
      return c.json({ error: "expected a boolean enabled field" }, 400);
    if (enabled !== body.enabled) {
      enabled = body.enabled;
      dnsQueries.length = 0;
      if (enabled) {
        // Simulate incoming traffic only after explicit recording consent.
        dnsQueries.push(
          ...[...sampleQueries]
            .reverse()
            .map((row) => ({
              ...row,
              id: ++dnsThrough,
              time_ms: Date.now(),
            }))
            .reverse(),
        );
      }
      for (const emit of dnsSubscribers) emit("capture", captureState());
      for (const row of [...dnsQueries].reverse())
        for (const emit of dnsSubscribers) emit("query", row);
    }
    c.header("Cache-Control", "no-store");
    return c.json(captureState());
  });
  app.get(`${API_BASE}/system/dns/queries`, (c) => {
    const before = c.req.query("before");
    const limit = c.req.query("limit");
    const validId = (value: string) =>
      /^\d+$/.test(value) && Number.isSafeInteger(Number(value)) && Number(value) > 0;
    if (
      (before !== undefined && !validId(before)) ||
      (limit !== undefined && (!validId(limit) || Number(limit) > 128))
    )
      return c.json({ error: "before must be a positive ID; limit must be 1..128" }, 400);
    const rows = dnsQueries.filter((row) => before === undefined || row.id < Number(before));
    const queries = rows.slice(0, Number(limit ?? 128));
    c.header("Cache-Control", "no-store");
    return c.json({
      queries,
      capacity,
      retained_from: dnsThrough - dnsQueries.length + 1,
      next_before: rows.length > queries.length ? queries[queries.length - 1].id : 0,
    });
  });
  app.get(`${API_BASE}/system/dns/stream`, (c) => {
    if (dnsSubscribers.size >= 8)
      return c.json({ error: "DNS stream subscriber limit reached" }, 503);
    c.header("Cache-Control", "no-store, no-cache, no-transform");
    c.header("X-Accel-Buffering", "no");
    return streamSSE(c, async (stream) => {
      let finish!: () => void;
      const done = new Promise<void>((resolve) => (finish = resolve));
      let ready = false;
      let restart = false;
      const emit = (event: string, data: unknown) => {
        if (!ready) {
          restart = true;
          return;
        }
        void stream.writeSSE({ event, data: JSON.stringify(data) }).catch(finish);
      };
      dnsSubscribers.add(emit);
      stream.onAbort(finish);
      // Keep-alive comments only; data is pushed by events, not sampled on a timer.
      const heartbeat = setInterval(() => {
        void stream.write(": heartbeat\n\n").catch(finish);
      }, 15000);
      try {
        do {
          restart = false;
          const queries = [...dnsQueries].reverse();
          await stream.writeSSE({ event: "capture", data: JSON.stringify(captureState()) });
          await stream.writeSSE({
            event: "snapshot_begin",
            data: JSON.stringify({ capacity: 1000, count: queries.length, through: dnsThrough }),
          });
          for (let offset = 0; !restart && offset < queries.length; offset += 32) {
            await stream.writeSSE({
              event: "snapshot_chunk",
              data: JSON.stringify({ offset, queries: queries.slice(offset, offset + 32) }),
            });
          }
          if (!restart)
            await stream.writeSSE({
              event: "snapshot_end",
              data: JSON.stringify({ through: dnsThrough }),
            });
        } while (restart);
        ready = true;
        await done;
      } finally {
        clearInterval(heartbeat);
        dnsSubscribers.delete(emit);
      }
    });
  });
  app.delete(`${API_BASE}/system/dns/queries`, (c) => {
    dnsQueries.length = 0;
    const state = captureState();
    for (const emit of dnsSubscribers) emit("clear", state);
    c.header("Cache-Control", "no-store");
    return c.json({ ...state, cleared: true });
  });
}
