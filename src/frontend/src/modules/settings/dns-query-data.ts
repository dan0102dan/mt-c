/* Shared presentation helpers for the DNS query history in Settings. */
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
  const byDomain = new Map<string, { domain: string; count: number; clients: Set<string>; lastTime: number }>();
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
    .sort((a, b) => b.count - a.count || b.lastTime - a.lastTime || a.domain.localeCompare(b.domain));
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
