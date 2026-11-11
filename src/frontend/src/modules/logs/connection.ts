import { createEventParser } from "../settings/dns/stream";
import type { LogCursor } from "./history";

export type LogConnectionState = "idle" | "connecting" | "live" | "reconnecting" | "unavailable";
type Options = {
  url: string;
  token: string;
  cursor: () => LogCursor;
  receiver: () => (event: { event: string; data: string }) => "changed" | "ready" | "none";
  onState: (state: LogConnectionState) => void;
  onChange: () => void;
  onUnauthorized: () => void;
  fetch?: typeof fetch;
};

/** Same bounded SSE parser/auth transport pattern as DNS, not its retention
 * policy. There is one fetch stream, and retry only after a broken connection. */
export function subscribeApplicationLogs(options: Options): () => void {
  const request = options.fetch ?? globalThis.fetch;
  let stopped = false;
  let delay = 1000;
  let retry: ReturnType<typeof setTimeout> | undefined;
  let watchdog: ReturnType<typeof setTimeout> | undefined;
  let controller: AbortController | undefined;
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
      const cursor = options.cursor();
      const query = cursor.session
        ? `?${new URLSearchParams({ session: cursor.session, after: cursor.after })}` : "";
      const headers: Record<string, string> = { Accept: "text/event-stream" };
      if (options.token) headers.Authorization = `Bearer ${options.token}`;
      watch(10000);
      const response = await request(`${options.url}${query}`, {
        headers, signal: attempt.signal, cache: "no-store",
      });
      if (stopped) { await response.body?.cancel(); return; }
      if (response.status === 401 || response.status === 403) {
        stopped = true;
        await response.body?.cancel();
        options.onUnauthorized();
        return;
      }
      if (response.status === 404) {
        stopped = true;
        await response.body?.cancel();
        options.onState("unavailable");
        return;
      }
      if (!response.ok || !response.body ||
          response.headers.get("Content-Type")?.split(";")[0].trim() !== "text/event-stream") {
        await response.body?.cancel();
        throw new Error("Application log stream unavailable");
      }
      reader = response.body.getReader();
      const decoder = new TextDecoder();
      const receive = options.receiver();
      const parse = createEventParser((event) => {
        if (stopped) return;
        const outcome = receive(event);
        if (outcome === "changed") options.onChange();
        if (outcome === "ready") {
          delay = 1000;
          options.onState("live");
          options.onChange();
        }
      });
      watch(45000);
      let yielded = performance.now();
      for (;;) {
        const chunk = await reader.read();
        if (stopped) return;
        if (chunk.done) throw new Error("Application log stream ended");
        watch(45000);
        parse(decoder.decode(chunk.value, { stream: true }));
        // A large replay must not monopolize the microtask queue/rendering.
        if (performance.now() - yielded >= 8) {
          await new Promise<void>((resolve) => setTimeout(resolve, 0));
          yielded = performance.now();
        }
      }
    } catch {
      if (!stopped) options.onState("reconnecting");
    } finally {
      clearTimeout(watchdog);
      attempt.abort();
      try { await reader?.cancel(); } catch { /* The socket may already be closed. */ }
      reader?.releaseLock();
      if (!stopped) {
        const ms = Math.round(delay * (0.8 + Math.random() * 0.4));
        delay = Math.min(delay * 2, 30000);
        retry = setTimeout(() => void connect(), ms);
      }
    }
  };
  options.onState("connecting");
  void connect();
  return () => {
    stopped = true;
    clearTimeout(retry);
    clearTimeout(watchdog);
    controller?.abort();
  };
}
