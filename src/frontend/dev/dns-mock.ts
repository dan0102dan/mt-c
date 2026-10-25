import type { Hono } from "hono";
import { streamSSE } from "hono/streaming";

import type { DnsQuery } from "../src/modules/settings/dns/data.ts";

const API_BASE = "/api/v1";

export function registerMockDnsRoutes(app: Hono) {
  const dnsQueries: DnsQuery[] = [];
  const devices = [
    { client: "192.168.1.101", client_name: "Рабочий ноутбук" },
    { client: "192.168.1.102", client_name: "Living room TV" },
    { client: "192.168.1.103" },
    { client: "2001:db8:1234:5678::104", client_name: "Телефон Александра" },
    { client: "192.168.1.105", client_name: "Home server — media and backups" },
    { client: "2001:db8:1234:5678:abcd:ef01:2345:6789" },
  ];
  const domains = [
    "github.com",
    "example.org",
    "example.net",
    "api.github.com",
    "dns.google",
    "cloudflare-dns.com",
    "www.wikipedia.org",
    "registry.npmjs.org",
    "updates.example.org",
    "_dns.resolver.arpa",
    "very-long-service-name.eu-central-1.streaming.media.example.org",
    "telemetry.device-with-a-long-name.home-network.example.net",
  ];
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
        const now = Date.now() - 1000;
        for (let burst = 0; burst < 120; burst++) {
          const types =
            burst % 5 === 0 ? [15, 16, 64] : burst % 7 === 0 ? [12, 33, 257] : [1, 28, 65];
          const domain =
            burst % 3 === 0
              ? `service-${burst % 40}.regional-edge.cdn.example.com`
              : domains[burst % domains.length];
          for (const [offset, qtype] of types.entries()) {
            dnsQueries.push({
              ...devices[burst % devices.length],
              id: ++dnsThrough,
              time_ms: now - (119 - burst) * 36 * 60 * 1000 + offset * 20,
              domain,
              qtype,
              protocol: burst % 4 === 0 ? "tcp" : "udp",
            });
          }
        }
        dnsQueries.reverse();
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
      // Keep-alive comments only; data is pushed by events, not sampled on a timer.
      const heartbeat = setInterval(() => {
        void stream.write(": heartbeat\n\n").catch(finish);
      }, 15000);
      const cleanup = () => {
        clearInterval(heartbeat);
        dnsSubscribers.delete(emit);
        finish();
      };
      stream.onAbort(cleanup);
      // Hono's response-body cancellation hook alone does not cover Deno's
      // request abort signal. Release the slot immediately on either path.
      const signal = c.req.raw.signal;
      const abort = () => stream.abort();
      signal.addEventListener("abort", abort, { once: true });
      if (signal.aborted) abort();
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
        signal.removeEventListener("abort", abort);
        cleanup();
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
