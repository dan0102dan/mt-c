import type { Hono } from "hono";
import { streamSSE } from "hono/streaming";

const BASE = "/api/v1/logs";
const LOG_LEVELS = [
  "trace",
  "debug",
  "info",
  "warn",
  "error",
  "fatal",
  "panic",
  "nolevel",
  "disabled",
] as const;
const LOG_TAGS = ["TRC", "DBG", "INF", "WRN", "ERR", "FTL", "PNC", "???"] as const;
const FRAME_BYTES = 12 * 1024;
const MAX_STREAMS = 8;
const MAX_U64 = 18446744073709551615n;
const encoder = new TextEncoder();

type LogRecord = { level: number; bytes: Uint8Array };
type Route = { name: string; interface: string };
type Options = {
  getLogLevel: () => string;
  getRoutes?: () => readonly Route[];
  /** Override only for automated tests. */
  intervalMs?: number;
};

const domains = [
  "api.github.com",
  "registry.npmjs.org",
  "cdn.example.org",
  "cloudflare.com",
  "updates.example.org",
  "www.wikipedia.org",
  "media.example.net",
  "example.org",
];
const clients = ["192.168.1.101", "192.168.1.102", "192.168.1.104", "192.168.1.105"];
const qtypes = ["A", "AAAA", "HTTPS", "A", "AAAA", "A", "MX", "A"];
const kinds = [2, 2, 1, 2, 3, 2, 0, 2, 4, 2, 2, 1] as const;

/** Development-only imitation of daemon output. Neither a real DNS request nor
 * a browser poll is made; a new request is synthesized at a modest fixed pace. */
export function registerMockLogRoutes(app: Hono, options: Options) {
  let enabled = false;
  let session = "";
  let records: LogRecord[] = [];
  let generation = 0;
  let requests = 0;
  let timer: ReturnType<typeof setInterval> | undefined;
  let activeStreams = 0;
  const listeners = new Set<() => void>();

  const wakeAll = () => {
    for (const wake of listeners) wake();
  };
  const capture = () => ({ enabled, session, through: String(records.length), dropped: "0" });
  const minLevel = () => {
    const level = LOG_LEVELS.indexOf(options.getLogLevel() as (typeof LOG_LEVELS)[number]);
    return level < 0 ? 2 : level;
  };
  const record = (level: number, message: string) => {
    if (!enabled || level < minLevel() || level >= LOG_TAGS.length) return;
    const time = new Date().toISOString().replace(/\.\d{3}Z$/, "Z");
    records.push({
      level,
      bytes: encoder.encode(`${time} ${LOG_TAGS[level]} ${message}\n`),
    });
    wakeAll();
  };
  const generateRequest = () => {
    if (!enabled) return;
    const id = ++requests;
    const index = (id - 1) % domains.length;
    const domain = domains[index];
    const client = clients[id % clients.length];
    const type = qtypes[index];
    const routes = options.getRoutes?.() ?? [];
    const route = routes.length
      ? routes[id % routes.length]
      : { name: "default", interface: "wan" };
    const prefix = `mock dns request id=${id} client=${client} domain=${domain} type=${type}`;
    const level = kinds[(id - 1) % kinds.length];
    switch (level) {
      case 0:
        record(level, `${prefix} transport=udp received`);
        break;
      case 1:
        record(level, `${prefix} evaluating group=${route.name}`);
        break;
      case 3:
        record(level, `${prefix} upstream timeout; trying fallback resolver`);
        break;
      case 4:
        record(level, `${prefix} upstream unavailable; query rejected`);
        break;
      default:
        record(
          level,
          `${prefix} group=${route.name} interface=${route.interface} elapsed_ms=${4 + (id % 37)}`,
        );
    }
  };
  const setRecording = (next: boolean) => {
    if (next === enabled) return;
    if (timer !== undefined) clearInterval(timer);
    timer = undefined;
    enabled = next;
    records = [];
    requests = 0;
    session = next ? crypto.randomUUID().replaceAll("-", "") : "";
    generation++;
    if (next) {
      generateRequest(); // Show a sample request immediately after Start.
      timer = setInterval(generateRequest, options.intervalMs ?? 1100);
    }
    wakeAll();
  };

  app.get(`${BASE}/capture`, (c) => {
    c.header("Cache-Control", "no-store");
    return c.json(capture());
  });
  app.put(`${BASE}/capture`, async (c) => {
    const raw = await c.req.text();
    let value: unknown;
    try {
      value = JSON.parse(raw);
    } catch {
      return c.json({ error: "expected only a boolean enabled field" }, 400);
    }
    if (
      encoder.encode(raw).byteLength > 128 ||
      !value ||
      typeof value !== "object" ||
      Array.isArray(value) ||
      Object.keys(value).length !== 1 ||
      !("enabled" in value) ||
      typeof value.enabled !== "boolean"
    ) {
      return c.json({ error: "expected only a boolean enabled field" }, 400);
    }
    setRecording(value.enabled);
    c.header("Cache-Control", "no-store");
    return c.json(capture());
  });

  app.get(`${BASE}/stream`, (c) => {
    const cursorSession = c.req.query("session");
    const after = c.req.query("after");
    if (
      (cursorSession !== undefined || after !== undefined) &&
      (!cursorSession ||
        !/^[a-f0-9]{32}$/.test(cursorSession) ||
        !after ||
        !/^(0|[1-9][0-9]{0,19})$/.test(after) ||
        BigInt(after) > MAX_U64)
    ) {
      return c.json({ error: "invalid log session or cursor" }, 400);
    }
    if (enabled && cursorSession === session && BigInt(after ?? "0") > BigInt(records.length))
      return c.json({ error: "log cursor is ahead of the journal" }, 400);
    if (activeStreams >= MAX_STREAMS)
      return c.json({ error: "application log stream subscriber limit reached" }, 503);
    c.header("Cache-Control", "no-store, no-cache, no-transform");
    c.header("X-Accel-Buffering", "no");
    return streamSSE(c, async (stream) => {
      activeStreams++;
      let aborted = false;
      let currentGeneration = -1;
      let currentSession = "";
      let nextId = 1;
      let through = 0;
      let ready = false;
      let pendingWake = false;
      let resolveWake: (() => void) | undefined;
      const wake = () => {
        pendingWake = true;
        resolveWake?.();
      };
      const cleanup = () => {
        if (aborted) return;
        aborted = true;
        wake();
      };
      const abort = () => stream.abort();
      listeners.add(wake);
      stream.onAbort(cleanup);
      c.req.raw.signal.addEventListener("abort", abort, { once: true });
      if (c.req.raw.signal.aborted) abort();
      try {
        while (!aborted) {
          if (currentGeneration !== generation) {
            currentGeneration = generation;
            currentSession = session;
            nextId = enabled && cursorSession === session ? Number(after) + 1 : 1;
            through = records.length;
            ready = false;
            await stream.writeSSE({ event: "capture", data: JSON.stringify(capture()) });
            continue;
          }
          const boundary = ready ? records.length : through;
          if (enabled && nextId <= boundary) {
            const item = records[nextId - 1];
            if (!item) continue;
            for (let offset = 0; offset < item.bytes.length && !aborted; offset += FRAME_BYTES) {
              const bytes = item.bytes.subarray(offset, offset + FRAME_BYTES);
              await stream.writeSSE({
                event: "log",
                data: JSON.stringify({
                  session: currentSession,
                  id: String(nextId),
                  offset,
                  length: item.bytes.length,
                  level: item.level,
                  end: offset + bytes.length === item.bytes.length,
                  data: btoa(String.fromCharCode(...bytes)),
                }),
              });
              if (currentGeneration !== generation) break;
            }
            if (currentGeneration === generation) nextId++;
            continue;
          }
          if (!ready) {
            ready = true;
            await stream.writeSSE({
              event: "ready",
              data: JSON.stringify({
                session: currentSession,
                through: String(through),
              }),
            });
            continue;
          }
          if (pendingWake) {
            pendingWake = false;
            continue;
          }
          const heartbeat = await new Promise<boolean>((resolve) => {
            const timeout = setTimeout(() => done(true), 15000);
            const done = (timedOut: boolean) => {
              clearTimeout(timeout);
              resolveWake = undefined;
              resolve(timedOut);
            };
            resolveWake = () => done(false);
          });
          pendingWake = false;
          if (heartbeat && !aborted) await stream.write(": heartbeat\n\n");
        }
      } catch {
        /* Disconnects and transient transport failures are expected. */
      } finally {
        cleanup();
        listeners.delete(wake);
        activeStreams--;
        c.req.raw.signal.removeEventListener("abort", abort);
      }
    });
  });

  // Only exposed to tests; not an HTTP endpoint or production API.
  return {
    capture,
    generateRequest,
    close() {
      setRecording(false);
    },
  };
}
