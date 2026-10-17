import type { DnsQuery, DnsQueryResponse } from "./dns-query-data";

export type StreamEvent = { event: string; data: string };
export type StreamState = "connecting" | "live" | "reconnecting";
const MAX_EVENT_SIZE = 2 * 1024 * 1024;
const CAPACITY = 1000;

/** Incremental SSE parser: TCP/UTF-8 chunks are not event boundaries. */
export function createEventParser(emit: (event: StreamEvent) => void) {
  let buffer = "";
  let event = "message";
  let data: string[] = [];
  let size = 0;
  let skipLF = false;
  return (text: string) => {
    if (skipLF && text.length) {
      if (text.startsWith("\n")) text = text.slice(1);
      skipLF = false;
    }
    buffer += text;
    let start = 0;
    for (let i = 0; i < buffer.length; i++) {
      const char = buffer[i];
      if (char !== "\n" && char !== "\r") continue;
      const line = buffer.slice(start, i);
      size += line.length;
      if (size > MAX_EVENT_SIZE) throw new Error("DNS stream event too large");
      if (line === "") {
        if (data.length) emit({ event, data: data.join("\n") });
        event = "message";
        data = [];
        size = 0;
      } else if (!line.startsWith(":")) {
        const colon = line.indexOf(":");
        const field = colon < 0 ? line : line.slice(0, colon);
        let value = colon < 0 ? "" : line.slice(colon + 1);
        if (value.startsWith(" ")) value = value.slice(1);
        if (field === "event") event = value || "message";
        if (field === "data") data.push(value);
      }
      if (char === "\r") {
        if (buffer[i + 1] === "\n") i++;
        else if (i === buffer.length - 1) skipLF = true;
      }
      start = i + 1;
    }
    buffer = buffer.slice(start);
    if (size + buffer.length > MAX_EVENT_SIZE) throw new Error("DNS stream event too large");
  };
}

function isQuery(value: unknown): value is DnsQuery {
  if (!value || typeof value !== "object") return false;
  const row = value as DnsQuery;
  return (
    Number.isSafeInteger(row.id) && row.id > 0 &&
    Number.isFinite(row.time_ms) && typeof row.client === "string" && row.client.length <= 48 &&
    typeof row.domain === "string" && row.domain.length <= 1024 &&
    Number.isInteger(row.qtype) && row.qtype >= 0 && row.qtype <= 65535 &&
    (row.protocol === "udp" || row.protocol === "tcp")
  );
}

/** Reconnect/eviction snapshots replace state; individual events are bounded and idempotent. */
export function applyDnsEvent(state: DnsQueryResponse, event: StreamEvent): DnsQueryResponse {
  if (event.event === "clear") return { ...state, queries: [] };
  if (event.event !== "snapshot" && event.event !== "query") return state;
  const value: unknown = JSON.parse(event.data);
  if (event.event === "snapshot") {
    const snapshot = value as DnsQueryResponse | null;
    if (!snapshot || !Array.isArray(snapshot.queries) || snapshot.queries.length > CAPACITY ||
        !snapshot.queries.every(isQuery) ||
        new Set(snapshot.queries.map((row) => row.id)).size !== snapshot.queries.length) throw new Error("Invalid DNS snapshot");
    return { queries: snapshot.queries, capacity: CAPACITY };
  }
  if (!isQuery(value)) throw new Error("Invalid DNS query event");
  return {
    capacity: state.capacity,
    queries: [value, ...state.queries.filter((row) => row.id !== value.id)].slice(0, CAPACITY),
  };
}

type Options = {
  url: string;
  token: string;
  onEvent: (event: StreamEvent) => void;
  onState: (state: StreamState) => void;
  onUnauthorized: () => void;
  fetch?: typeof fetch;
};

/** Fetch streaming preserves Bearer auth; native EventSource cannot set that header.
 * There is no polling fallback. Only a failed/ended connection is retried. */
export function subscribeDnsQueries(options: Options): () => void {
  const request = options.fetch ?? globalThis.fetch;
  let stopped = false;
  let retryDelay = 1000;
  let retryTimer: ReturnType<typeof setTimeout> | undefined;
  let controller: AbortController | undefined;
  let watchdog: ReturnType<typeof setTimeout> | undefined;

  const connect = async () => {
    if (stopped) return;
    const attempt = new AbortController();
    controller = attempt;
    let reader: ReadableStreamDefaultReader<Uint8Array> | undefined;
    const watch = (ms: number) => {
      clearTimeout(watchdog);
      watchdog = setTimeout(() => attempt.abort(), ms);
    };
    try {
      watch(10000);
      const headers: Record<string, string> = { Accept: "text/event-stream" };
      if (options.token) headers.Authorization = `Bearer ${options.token}`;
      const response = await request(options.url, {
        headers, signal: attempt.signal, cache: "no-store",
      });
      if (response.status === 401 || response.status === 403) {
        stopped = true;
        options.onUnauthorized();
        return;
      }
      if (!response.ok || !response.body ||
          response.headers.get("Content-Type")?.split(";")[0].trim() !== "text/event-stream") {
        await response.body?.cancel();
        throw new Error("DNS stream unavailable");
      }
      watch(45000); // Three missed 15-second heartbeats, not a data refresh interval.
      reader = response.body.getReader();
      const decode = new TextDecoder();
      const parse = createEventParser((event) => {
        if (stopped) return;
        options.onEvent(event);
        retryDelay = 1000;
        options.onState("live");
      });
      for (;;) {
        const chunk = await reader.read();
        if (stopped) return;
        if (chunk.done) throw new Error("DNS stream ended");
        watch(45000);
        parse(decode.decode(chunk.value, { stream: true }));
      }
    } catch {
      if (!stopped) options.onState("reconnecting");
    } finally {
      clearTimeout(watchdog);
      attempt.abort();
      try { await reader?.cancel(); } catch { /* Transport may already be closed. */ }
      reader?.releaseLock();
      if (!stopped) {
        const delay = Math.round(retryDelay * (0.8 + Math.random() * 0.4));
        retryDelay = Math.min(retryDelay * 2, 30000);
        retryTimer = setTimeout(() => void connect(), delay);
      }
    }
  };
  options.onState("connecting");
  void connect();
  return () => {
    stopped = true;
    clearTimeout(retryTimer);
    clearTimeout(watchdog);
    controller?.abort();
  };
}
