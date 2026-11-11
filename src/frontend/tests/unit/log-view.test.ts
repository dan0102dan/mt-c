import {
  formatLogs,
  logLevel,
  logText,
  logWindow,
  matchesLog,
  type LogEntry,
} from "../../src/modules/logs/log-view.ts";

function equal(actual: unknown, expected: unknown) {
  if (JSON.stringify(actual) !== JSON.stringify(expected)) {
    throw new Error(`Expected ${JSON.stringify(expected)}, received ${JSON.stringify(actual)}`);
  }
}

const row: LogEntry = {
  id: "boot-a:1",
  level: "info",
  message: "DNS ready",
  fields: { time: "2026-11-11T00:00:00Z", client: "192.168.1.2", domain: "Example.org" },
};

Deno.test("log levels retain upstream order and labels", () => {
  equal(logLevel("trace"), [-1, "TRC"]);
  equal(logLevel("info"), [1, "INF"]);
  equal(logLevel("panic"), [5, "PNC"]);
});
Deno.test("unknown and prototype-named levels cannot crash the viewer", () => {
  for (const level of ["nolevel", "unexpected", "__proto__", "constructor", "toString"]) {
    equal(logLevel(level), [6, "???"]);
    equal(matchesLog({ ...row, level }, "error", ""), true);
  }
});
Deno.test("severity filter is a minimum", () => {
  equal(matchesLog(row, "warn", ""), false);
  equal(matchesLog(row, "debug", ""), true);
});
Deno.test("search covers message, field names and values without case sensitivity", () => {
  for (const filter of [" READY ", "CLIENT=192.168", "example.ORG"]) {
    equal(matchesLog(row, "info", filter), true);
  }
  equal(matchesLog(row, "info", "missing"), false);
});
Deno.test("exports available rows without a server or DOM", () => {
  equal(
    formatLogs([row]),
    '2026-11-11T00:00:00Z INF DNS ready client="192.168.1.2" domain="Example.org"',
  );
  equal(formatLogs([]), "");
});
Deno.test("embedded line breaks and control characters cannot forge exported rows", () => {
  const text = formatLogs([
    {
      ...row,
      message: "first\nsecond\r\t\u001b",
      fields: { time: "now", value: "a\nb" },
    },
  ]);
  equal(text.split("\n").length, 1);
  equal(text, 'now INF first\\nsecond\\r\\t\\u001b value="a\\nb"');
  equal(logText("а\u2028б\u2029в"), "а\\u2028б\\u2029в");
});
Deno.test("export does not mutate retained data", () => {
  const before = JSON.stringify(row);
  formatLogs([row]);
  equal(JSON.stringify(row), before);
});
Deno.test("empty virtual window is valid", () => {
  equal(logWindow(0, 9000, 600), { start: 0, end: 0 });
});
Deno.test("virtual window is bounded for a long journal", () => {
  const range = logWindow(10000, 16000, 320);
  equal(range, { start: 980, end: 1040 });
});
Deno.test("filtering at a stale scroll offset cannot leave a blank window", () => {
  const range = logWindow(3, 160000, 320);
  equal(range, { start: 0, end: 3 });
});
