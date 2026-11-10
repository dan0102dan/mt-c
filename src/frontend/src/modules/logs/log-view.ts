/** Presentation model, not the wire format. Validate and bound incoming data in the store. */
export type LogEntry = {
  /** Stable identity; the future store must include the daemon boot ID. */
  id: string;
  level: string;
  message: string;
  fields: { time: string; [key: string]: string };
};

export const LOG_LEVELS: Readonly<Record<string, readonly [number, string]>> = {
  trace: [-1, "TRC"],
  debug: [0, "DBG"],
  info: [1, "INF"],
  warn: [2, "WRN"],
  error: [3, "ERR"],
  fatal: [4, "FTL"],
  panic: [5, "PNC"],
};

export function logLevel(level: string): readonly [number, string] {
  return Object.hasOwn(LOG_LEVELS, level) ? LOG_LEVELS[level] : [6, "???"];
}

/** Also used by rendering: embedded newlines must not break fixed-height rows. */
export function logText(value: string): string {
  return value.replace(/[\\\r\n\t\u0000-\u001f\u007f\u2028\u2029]/g, (char) => {
    if (char === "\\") return "\\\\";
    if (char === "\r") return "\\r";
    if (char === "\n") return "\\n";
    if (char === "\t") return "\\t";
    return `\\u${char.charCodeAt(0).toString(16).padStart(4, "0")}`;
  });
}

export function matchesLog(entry: LogEntry, minimum: string, filter: string): boolean {
  if (logLevel(entry.level)[0] < logLevel(minimum)[0]) return false;
  const needle = filter.trim().toLocaleLowerCase();
  return (
    !needle ||
    entry.message.toLocaleLowerCase().includes(needle) ||
    Object.entries(entry.fields).some(([key, value]) =>
      `${key}=${value}`.toLocaleLowerCase().includes(needle),
    )
  );
}

/** Entirely local: export must still work when the daemon is unavailable. */
export function formatLogs(entries: readonly LogEntry[]): string {
  return entries
    .map(({ fields, level, message }) => {
      const extra = Object.entries(fields)
        .filter(([key]) => key !== "time")
        .map(([key, value]) => `${logText(key)}=${JSON.stringify(value)}`)
        .join(" ");
      return `${logText(fields.time)} ${logLevel(level)[1]} ${logText(message)}${extra ? ` ${extra}` : ""}`;
    })
    .join("\n");
}

export function logWindow(count: number, scrollTop: number, height: number) {
  const lineHeight = 16;
  const overscan = 20;
  const first = Math.min(
    Math.max(0, count - 1),
    Math.max(0, Math.floor(scrollTop / lineHeight)),
  );
  return {
    start: Math.max(0, first - overscan),
    end: Math.min(count, first + Math.ceil(Math.max(0, height) / lineHeight) + overscan),
  };
}
