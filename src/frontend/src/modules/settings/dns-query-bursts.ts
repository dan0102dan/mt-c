import type { DnsQuery } from "./dns-query-data";

export type DnsBurst = DnsQuery & { queries: DnsQuery[] };
const relatedTypes = new Set([1, 28, 65]);
/** Display-only grouping. Keep the backend's raw records and exact times.
 * A fixed window from the first arrival, not a sliding or debounced window.
 * A repeated type starts another group: retries must not disappear. */
export function groupDnsBursts(queries: readonly DnsQuery[]): DnsBurst[] {
  const groups: DnsBurst[] = [];
  const latest = new Map<string, DnsBurst>();
  for (const row of [...queries].sort((a, b) => a.id - b.id)) {
    const key = JSON.stringify([row.client, row.domain.toLowerCase(), row.protocol]);
    const previous = latest.get(key);
    const elapsed = previous ? row.time_ms - previous.time_ms : -1;
    if (relatedTypes.has(row.qtype) && previous && elapsed >= 0 && elapsed <= 1000 &&
        !previous.queries.some((query) => query.qtype === row.qtype)) {
      previous.queries.push(row);
    } else {
      const group = { ...row, queries: [row] };
      groups.push(group);
      if (relatedTypes.has(row.qtype)) latest.set(key, group);
    }
  }
  // Newest burst first; the anchor ID stays stable when a sibling arrives.
  return groups.reverse();
}
