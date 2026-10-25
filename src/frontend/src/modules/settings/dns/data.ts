/* DNS query types, bounds and pure transformations. */
export type DnsQuery = {
  id: number;
  time_ms: number;
  client: string;
  client_name?: string; // Optional current router label; IP remains the identity.
  domain: string;
  qtype: number;
  protocol: "udp" | "tcp";
};

export type DnsQueryResponse = { queries: DnsQuery[]; capacity: number };

export type DomainSummary = {
  domain: string;
  count: number;
  clients: string[];
  lastTime: number;
};

export function filterDnsQueries(queries: DnsQuery[], search: string, client: string): DnsQuery[] {
  const text = search.trim().toLocaleLowerCase();
  return queries.filter(
    (row) =>
      (!client || row.client === client) &&
      (!text || row.domain.toLocaleLowerCase().includes(text)),
  );
}

/* Group the ENTIRE filtered history before paginating, not just visible rows. */
export function groupDnsQueries(queries: DnsQuery[]): DomainSummary[] {
  const byDomain = new Map<
    string,
    { domain: string; count: number; clients: Set<string>; lastTime: number }
  >();
  for (const row of queries) {
    const key = row.domain.toLocaleLowerCase();
    const item = byDomain.get(key) ?? {
      domain: row.domain,
      count: 0,
      clients: new Set<string>(),
      lastTime: row.time_ms,
    };
    item.count++;
    item.clients.add(row.client);
    item.lastTime = Math.max(item.lastTime, row.time_ms);
    byDomain.set(key, item);
  }
  return [...byDomain.values()]
    .map((item) => ({
      domain: item.domain,
      count: item.count,
      clients: [...item.clients].sort(),
      lastTime: item.lastTime,
    }))
    .sort(
      (a, b) => b.count - a.count || b.lastTime - a.lastTime || a.domain.localeCompare(b.domain),
    );
}

const typeNames: Record<number, string> = {
  1: "A",
  2: "NS",
  5: "CNAME",
  6: "SOA",
  12: "PTR",
  15: "MX",
  16: "TXT",
  28: "AAAA",
  33: "SRV",
  41: "OPT",
  65: "HTTPS",
};

export function dnsQueryType(qtype: number): string {
  return typeNames[qtype] ?? `TYPE${qtype}`;
}

/** Defensive wire validation, derived from the router byte budget and minimum
 * lossless record size. This is not a configurable/history eviction quota.
 * Actual retention comes from the server's retained_from sequence boundary. */
export const DNS_HISTORY_MEMORY_LIMIT = 1024 * 1024;
export const DNS_HISTORY_RECORD_BYTES = 10;
export const MAX_DNS_CAPACITY = Math.floor(DNS_HISTORY_MEMORY_LIMIT / DNS_HISTORY_RECORD_BYTES);

/** A dual-stack socket may report an IPv4 peer as IPv4-mapped IPv6.
 * This is presentation normalization, not a prefix replacement on arbitrary IPv6. */
export function normalizeDnsClientAddress(address: string): string {
  if (!address.includes(":")) return address;
  let input = address.toLowerCase();
  if (input.includes(".")) {
    const colon = input.lastIndexOf(":");
    const octets = input.slice(colon + 1).split(".");
    if (
      octets.length !== 4 ||
      octets.some((part) => !/^(0|[1-9]\d{0,2})$/.test(part) || Number(part) > 255)
    )
      return address;
    const bytes = octets.map(Number);
    input = `${input.slice(0, colon + 1)}${((bytes[0] << 8) | bytes[1]).toString(16)}:${((bytes[2] << 8) | bytes[3]).toString(16)}`;
  }
  const halves = input.split("::");
  if (halves.length > 2) return address;
  const left = halves[0] ? halves[0].split(":") : [];
  const right = halves.length === 2 && halves[1] ? halves[1].split(":") : [];
  if ([...left, ...right].some((part) => !/^[\da-f]{1,4}$/.test(part))) return address;
  const gap = 8 - left.length - right.length;
  if (halves.length === 1 ? gap !== 0 : gap < 1) return address;
  const words = [...left, ...Array.from({ length: gap }, () => "0"), ...right].map((part) =>
    parseInt(part, 16),
  );
  if (words.slice(0, 5).some((word) => word !== 0) || words[5] !== 0xffff) return address;
  return [words[6] >> 8, words[6] & 255, words[7] >> 8, words[7] & 255].join(".");
}

export function isDnsClientName(value: unknown): value is string {
  return (
    typeof value === "string" &&
    value.trim().length > 0 &&
    !/[\u0000-\u001f\u007f-\u009f]/.test(value) &&
    new TextEncoder().encode(value).length < 128
  );
}

export function normalizeDnsClient(row: DnsQuery): DnsQuery {
  const client = normalizeDnsClientAddress(row.client);
  if (row.client_name !== undefined && !isDnsClientName(row.client_name)) {
    const { client_name: _ignored, ...query } = row;
    return { ...query, client };
  }
  return client === row.client ? row : { ...row, client };
}

/** Keep filter identity as an IP; equal aliases must not merge devices.
 * Snapshots refresh names for all rows, including removing unavailable names. */
export function dnsClientNames(queries: DnsQuery[]): Map<string, string> {
  const names = new Map<string, string>();
  const newest = new Map<string, number>();
  for (const row of queries) {
    const client = normalizeDnsClientAddress(row.client);
    if ((newest.get(client) ?? -1) >= row.id) continue;
    newest.set(client, row.id);
    if (isDnsClientName(row.client_name)) names.set(client, row.client_name);
    else names.delete(client);
  }
  return names;
}

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
    if (
      relatedTypes.has(row.qtype) &&
      previous &&
      elapsed >= 0 &&
      elapsed <= 1000 &&
      !previous.queries.some((query) => query.qtype === row.qtype)
    ) {
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

/** Two states only: newest first by default, oldest first after a click. */
export type DnsTimeSort = "desc" | "asc";

export function nextDnsTimeSort(direction: DnsTimeSort): DnsTimeSort {
  return direction === "desc" ? "asc" : "desc";
}

/** Sort the complete display view before pagination, never the ingest ring.
 * Stable ties retain the source view order. Build related-query bursts before sorting
 * so changing direction cannot change their membership or anchor time. */
export function sortDnsByTime<T>(
  rows: readonly T[],
  direction: DnsTimeSort,
  timestamp: (row: T) => number,
): T[] {
  const sign = direction === "asc" ? 1 : -1;
  return [...rows].sort((a, b) => sign * (timestamp(a) - timestamp(b)));
}

/** Equal counts keep the most recently requested domain first. */
export function sortDnsByCount(
  rows: readonly DomainSummary[],
  direction: DnsTimeSort,
): DomainSummary[] {
  const sign = direction === "asc" ? 1 : -1;
  return [...rows].sort((a, b) => sign * (a.count - b.count) || b.lastTime - a.lastTime);
}
