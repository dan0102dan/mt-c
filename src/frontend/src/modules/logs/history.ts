import type { LogEntry } from "./log-view";

export type LogCapture = {
  enabled: boolean;
  session: string;
  through: string;
  dropped: string;
};
export type LogCursor = { session: string; after: string };
const levels = ["trace", "debug", "info", "warn", "error", "fatal", "panic", "nolevel"];
const isSession = (s: unknown): s is string => typeof s === "string" && /^[a-f0-9]{32}$/.test(s);
const isSequence = (s: unknown): s is string =>
  typeof s === "string" && /^(0|[1-9][0-9]{0,19})$/.test(s) && BigInt(s) <= 18446744073709551615n;
function object(value: unknown): Record<string, unknown> {
  if (!value || typeof value !== "object" || Array.isArray(value)) throw new Error("Invalid log data");
  return value as Record<string, unknown>;
}
export function parseLogCapture(value: unknown): LogCapture {
  const v = object(value);
  if (
    typeof v.enabled !== "boolean" ||
    !(isSession(v.session) || (!v.enabled && v.session === "")) ||
    !isSequence(v.through) || !isSequence(v.dropped) ||
    (!v.enabled && v.through !== "0")
  ) throw new Error("Invalid log capture state");
  return { enabled: v.enabled, session: v.session, through: v.through, dropped: v.dropped };
}

/** No count/byte eviction policy. Only completely received records are committed.
 * A receiver is connection-local, so an interrupted large record is retried
 * from its beginning without replacing the already committed browser history. */
export class LogHistory {
  entries: LogEntry[] = [];
  count = 0;
  capture: LogCapture | null = null;
  private session = "";
  private after = 0n;
  private shownSession = "";

  cursor(): LogCursor {
    return { session: this.session, after: this.after.toString() };
  }
  clear() {
    this.entries = [];
    this.count = 0;
    this.shownSession = "";
    // Keep the accepted cursor: reconnect must not undo a local Clear.
  }
  receiver() {
    let partial: {
      id: bigint; length: number; offset: number; level: number;
      parts: Uint8Array<ArrayBuffer>[];
    } | undefined;
    return (event: { event: string; data: string }): "changed" | "ready" | "none" => {
      const value = object(JSON.parse(event.data));
      if (event.event === "capture") {
        const capture = parseLogCapture(value);
        if (!capture.enabled || capture.session !== this.capture?.session) partial = undefined;
        if (capture.enabled && capture.session !== this.session) {
          this.session = capture.session;
          this.after = 0n;
        }
        this.capture = capture;
        return "changed";
      }
      if (event.event === "ready") {
        if (!this.capture || value.session !== this.capture.session ||
            !isSequence(value.through) || (this.capture.enabled && BigInt(value.through) > this.after)) {
          throw new Error("Incomplete log snapshot");
        }
        return "ready";
      }
      if (event.event !== "log") throw new Error("Unknown log event");
      if (!this.capture?.enabled || value.session !== this.session ||
          !isSequence(value.id) || value.id === "0" ||
          typeof value.offset !== "number" || !Number.isSafeInteger(value.offset) || value.offset < 0 ||
          typeof value.length !== "number" || !Number.isSafeInteger(value.length) || value.length < 1 ||
          typeof value.level !== "number" || !Number.isInteger(value.level) || value.level < 0 || value.level > 7 ||
          typeof value.end !== "boolean" || typeof value.data !== "string" ||
          value.data.length > 16384 || !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(value.data)) {
        throw new Error("Invalid log record");
      }
      const id = BigInt(value.id);
      if (id <= this.after) return "none";
      if (id !== this.after + 1n) throw new Error("Gap in log stream");
      if (!partial) {
        if (value.offset !== 0) throw new Error("Missing log fragment");
        partial = { id, length: value.length, level: value.level, offset: 0, parts: [] };
      }
      if (partial.id !== id || partial.length !== value.length || partial.level !== value.level ||
          partial.offset !== value.offset) throw new Error("Out-of-order log fragment");
      const binary = atob(value.data);
      if (!binary.length || binary.length > 12288 || binary.length > partial.length - partial.offset ||
          value.end !== (partial.offset + binary.length === partial.length)) {
        throw new Error("Invalid log fragment length");
      }
      const bytes = new Uint8Array(binary.length);
      for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
      partial.parts.push(bytes);
      partial.offset += bytes.length;
      if (!value.end) return "none";

      const decoder = new TextDecoder();
      const text = partial.parts.map((part) => decoder.decode(part, { stream: true })).join("") + decoder.decode();
      const match = /^(\S+) [A-Z?]{3} ([\s\S]*)$/.exec(text);
      const time = match?.[1] ?? "";
      if (this.shownSession !== this.session) {
        this.entries.push({
          id: `session:${this.session}`, kind: "session", level: "session", message: "Recording session",
          fields: { time, session: this.session },
        });
        this.shownSession = this.session;
      }
      this.entries.push({
        id: `${this.session}:${id}`, level: levels[partial.level],
        message: (match?.[2] ?? text).replace(/\n$/, ""), fields: { time }, raw: partial.parts,
      });
      this.count++;
      this.after = id;
      partial = undefined;
      return "changed";
    };
  }
}
