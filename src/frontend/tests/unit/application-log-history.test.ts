import { LogHistory, parseLogCapture } from "../../src/modules/logs/history";
import { logExportParts, logWindow } from "../../src/modules/logs/log-view";

const a = "a".repeat(32);
const b = "b".repeat(32);
const event = (name: string, data: unknown) => ({ event: name, data: JSON.stringify(data) });
function assert(value: unknown, message = "assertion failed"): asserts value {
  if (!value) throw new Error(message);
}
function throws(fn: () => unknown) {
  let failed = false;
  try { fn(); } catch { failed = true; }
  assert(failed, "expected invalid data to be rejected");
}
const capture = (enabled = true, session = a, through = "0") =>
  event("capture", { enabled, session, through, dropped: "0" });
function frames(id: number, body: string | Uint8Array, session = a, size = 12288) {
  const bytes = typeof body === "string" ? new TextEncoder().encode(body) : body;
  const result = [];
  for (let offset = 0; offset < bytes.length; offset += size) {
    const piece = bytes.slice(offset, offset + size);
    result.push(event("log", {
      session, id: String(id), length: bytes.length, offset, level: 2,
      end: offset + piece.length === bytes.length,
      data: btoa(Array.from(piece, (n) => String.fromCharCode(n)).join("")),
    }));
  }
  return result;
}
const line = (s: string) => `2026-11-11T00:00:00Z INF ${s}\n`;

Deno.test("application logs start empty and a read does not enable recording", () => {
  const history = new LogHistory();
  history.receiver()(capture(false, ""));
  assert(history.capture?.enabled === false && history.count === 0);
  assert(history.cursor().after === "0");
});
Deno.test("history is followed by live records in sequence", () => {
  const history = new LogHistory(); const receive = history.receiver();
  receive(capture(true, a, "2"));
  for (const f of frames(1, line("first"))) receive(f);
  for (const f of frames(2, line("second"))) receive(f);
  assert(receive(event("ready", { session: a, through: "2" })) === "ready");
  for (const f of frames(3, line("live"))) receive(f);
  assert(history.count === 3 && history.entries[1].message === "first");
  assert(history.entries.at(-1)?.message === "live");
});
Deno.test("128 KiB UTF-8 log is reconstructed and exported without truncation", async () => {
  const history = new LogHistory(); const receive = history.receiver();
  receive(capture());
  const message = line("Ж🙂".repeat(30000) + "END");
  for (const f of frames(1, message, a, 113)) receive(f);
  assert(history.count === 1 && history.entries.at(-1)?.message.endsWith("END"));
  const raw = history.entries.at(-1)?.raw;
  assert(raw);
  assert(await new Blob([...raw]).text() === message);
  assert((await new Blob(logExportParts(history.entries)).text()).endsWith(message));
});
Deno.test("there is no 10,000-row client eviction limit", () => {
  const history = new LogHistory(); const receive = history.receiver();
  receive(capture());
  for (let id = 1; id <= 11000; id++) for (const f of frames(id, line(`row ${id}`))) receive(f);
  assert(history.count === 11000 && history.entries[1].message === "row 1");
  assert(history.entries.at(-1)?.message === "row 11000");
  const window = logWindow(history.entries.length, 10000, 320);
  assert(window.end - window.start <= 60);
});
Deno.test("disconnect only discards an incomplete record; resume deduplicates", () => {
  const history = new LogHistory(); let receive = history.receiver();
  receive(capture());
  for (const f of frames(1, line("retained"))) receive(f);
  const parts = frames(2, line("long".repeat(10000)));
  receive(parts[0]);
  assert(history.count === 1 && history.cursor().after === "1");
  receive = history.receiver(); receive(capture(true, a, "2"));
  for (const f of frames(1, line("retained"))) receive(f);
  for (const f of parts) receive(f);
  assert(Number(history.count) === 2 && history.cursor().after === "2");
});
Deno.test("Stop and a new recording preserve the browser's previous session", () => {
  const history = new LogHistory(); const receive = history.receiver();
  receive(capture()); for (const f of frames(1, line("old"))) receive(f);
  receive(capture(false));
  assert(history.count === 1);
  receive(capture(true, b)); for (const f of frames(1, line("new"), b)) receive(f);
  assert(Number(history.count) === 2);
  assert(history.entries.filter((r) => r.kind === "session").length === 2);
  assert(history.entries[1].message === "old" && history.entries.at(-1)?.message === "new");
});
Deno.test("local Clear preserves the resume cursor", () => {
  const history = new LogHistory(); let receive = history.receiver();
  receive(capture()); for (const f of frames(1, line("cleared"))) receive(f);
  history.clear();
  assert(history.count === 0 && history.cursor().after === "1");
  receive = history.receiver(); receive(capture(true, a, "2"));
  for (const f of frames(1, line("cleared"))) receive(f);
  for (const f of frames(2, line("new"))) receive(f);
  assert(Number(history.count) === 1 && history.entries.at(-1)?.message === "new");
});
Deno.test("invalid chunks, sequence gaps and premature ready are rejected", () => {
  const history = new LogHistory(); const receive = history.receiver(); receive(capture());
  throws(() => receive(frames(2, line("gap"))[0]));
  throws(() => receive(event("ready", { session: a, through: "1" })));
  const part = JSON.parse(frames(1, line("hello"))[0].data);
  throws(() => receive(event("log", { ...part, offset: 1 })));
  throws(() => receive(event("log", { ...part, data: "!invalid!" })));
  throws(() => receive(event("log", { ...part, data: "A".repeat(16388) })));
  throws(() => receive(event("log", { ...part, length: Number.MAX_SAFE_INTEGER + 1 })));
});
Deno.test("64-bit IDs are decimal strings, not rounded JS numbers", () => {
  throws(() => parseLogCapture({ enabled: true, session: a, through: 1, dropped: "0" }));
  throws(() => parseLogCapture({ enabled: true, session: a, through: "18446744073709551616", dropped: "0" }));
  throws(() => parseLogCapture({ enabled: true, session: a, through: "01", dropped: "0" }));
  assert(parseLogCapture({ enabled: true, session: a, through: "18446744073709551615", dropped: "0" }).through.endsWith("615"));
});
Deno.test("export preserves binary bytes and embedded newlines independently of display", async () => {
  const history = new LogHistory(); const receive = history.receiver(); receive(capture());
  const bytes = new Uint8Array([0, 255, 10, 13, 239, 187, 191, 65]);
  for (const f of frames(1, bytes, a, 1)) receive(f);
  const row = history.entries.at(-1); assert(row);
  const exported = new Uint8Array(await new Blob(logExportParts([row])).arrayBuffer());
  assert(exported.length === bytes.length && exported.every((byte, i) => byte === bytes[i]));
});
