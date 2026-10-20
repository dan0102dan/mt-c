import type { DnsQuery } from "./dns-query-data";

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
    ) return address;
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
  const words = [...left, ...Array.from({ length: gap }, () => "0"), ...right].map((part) => parseInt(part, 16));
  if (words.slice(0, 5).some((word) => word !== 0) || words[5] !== 0xffff) return address;
  return [words[6] >> 8, words[6] & 255, words[7] >> 8, words[7] & 255].join(".");
}

export function isDnsClientName(value: unknown): value is string {
  return typeof value === "string" && value.trim().length > 0 &&
    !/[\u0000-\u001f\u007f-\u009f]/.test(value) &&
    new TextEncoder().encode(value).length < 128;
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
