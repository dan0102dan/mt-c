import type { DomainSummary } from "./dns-query-data";

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
