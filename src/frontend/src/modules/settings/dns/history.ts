import {
  DNS_HISTORY_MEMORY_LIMIT,
  dnsClientNames,
  MAX_DNS_CAPACITY,
  normalizeDnsClient,
  type DnsQuery,
  type DnsQueryResponse,
} from "./data";
import { applyDnsEvent, isDnsQuery, type StreamEvent } from "./stream";

export type CaptureState = {
  enabled: boolean;
  memory_limit: number;
  memory_used: number;
  count: number;
  capacity: number;
  dropped?: number;
};
export function parseCaptureState(value: unknown): CaptureState {
  if (!value || typeof value !== "object") throw new Error("Invalid capture state");
  const state = value as CaptureState;
  if (
    typeof state.enabled !== "boolean" ||
    ![state.memory_limit, state.memory_used, state.count, state.capacity].every(
      (n) => Number.isSafeInteger(n) && n >= 0,
    ) ||
    (state.dropped !== undefined && (!Number.isSafeInteger(state.dropped) || state.dropped < 0)) ||
    state.capacity < 1 ||
    state.capacity > MAX_DNS_CAPACITY ||
    state.count > state.capacity ||
    state.memory_limit > DNS_HISTORY_MEMORY_LIMIT ||
    (state.memory_limit > 0 && state.memory_used > state.memory_limit)
  )
    throw new Error("Invalid capture state");
  return state;
}
export function applyCaptureHistory(state: DnsQueryResponse, event: StreamEvent): DnsQueryResponse {
  const result = applyDnsEvent(state, event);
  if (event.event !== "query") return result;
  const value = JSON.parse(event.data) as { retained_from?: number; id: number };
  if (value.retained_from === undefined) return result;
  if (
    !Number.isSafeInteger(value.retained_from) ||
    value.retained_from < 1 ||
    value.retained_from > value.id
  )
    throw new Error("Invalid DNS retention boundary");
  return { ...result, queries: result.queries.filter((row) => row.id >= value.retained_from!) };
}

export type DnsHistoryView = {
  queries: DnsQuery[];
  capacity: number;
  capture: CaptureState | null;
  clientNames: ReadonlyMap<string, string>;
};

/** Bounded, non-reactive ingest ring. Each network event is O(1), apart from
 * evicting rows that actually leave retention. The component publishes an
 * immutable view once per animation frame, not once per DNS request. No event
 * queue grows while the browser is busy; only the retained history is stored.
 * Snapshots/aliases are committed atomically by the SSE receiver. */
export function createDnsHistory(initial: DnsHistoryView) {
  let capacity = initial.capacity;
  let ring: (DnsQuery | undefined)[] = new Array(capacity);
  let next = 0;
  let count = 0;
  let newest = 0;
  let capture = initial.capture ? { ...initial.capture } : null;
  let labelsKnown = false;
  let names = new Map(initial.clientNames);
  let cachedNames: ReadonlyMap<string, string> = names;
  let namesDirty = true;
  let rowsDirty = true;
  let cachedRows: DnsQuery[] = [];

  const append = (row: DnsQuery) => {
    ring[next] = row;
    next = (next + 1) % capacity;
    count = Math.min(count + 1, capacity);
    newest = row.id;
    rowsDirty = true;
  };
  const reset = (size = capacity) => {
    if (size !== capacity) {
      capacity = size;
      ring = new Array(capacity);
    } else ring.fill(undefined);
    count = next = 0;
    rowsDirty = true;
  };
  const replace = (rows: DnsQuery[], size: number) => {
    reset(size);
    newest = 0;
    for (let i = rows.length - 1; i >= 0; i--) {
      if (rows[i].id <= newest) throw new Error("Unordered DNS snapshot");
      append(rows[i]);
    }
    if (!labelsKnown) {
      names = dnsClientNames(rows);
      namesDirty = true;
    }
  };
  replace(initial.queries, initial.capacity);

  function ingest(event: StreamEvent) {
    if (event.event === "capture") {
      const state = parseCaptureState(JSON.parse(event.data));
      if (!state.enabled || state.capacity !== capacity) reset(state.capacity);
      capture = { ...state };
      return;
    }
    if (event.event === "clients") {
      // Only completed/validated metadata reaches us from createSnapshotReceiver.
      if (!event.clients || event.clients.length > 2048)
        throw new Error("Invalid DNS client inventory");
      names = new Map(event.clients.map((entry) => [entry.ip, entry.name]));
      labelsKnown = true;
      namesDirty = true;
      return;
    }
    if (event.event === "snapshot") {
      const state = applyDnsEvent({ queries: [], capacity }, event);
      replace(state.queries, state.capacity);
      return;
    }
    if (event.event === "clear") {
      const value = JSON.parse(event.data) as Partial<CaptureState>;
      if (typeof value.enabled === "boolean") capture = { ...parseCaptureState(value) };
      reset();
      return;
    }
    if (event.event !== "query" || capture?.enabled === false) return;
    const value: unknown = JSON.parse(event.data);
    if (!isDnsQuery(value)) throw new Error("Invalid DNS query event");
    const extra = value as DnsQuery & { retained_from?: number; memory_used?: number };
    const retained = extra.retained_from ?? Math.max(1, value.id - capacity + 1);
    if (!Number.isSafeInteger(retained) || retained < 1 || retained > value.id)
      throw new Error("Invalid DNS retention boundary");
    if (value.id <= newest) return;
    if (count && value.id !== newest + 1) throw new Error("Gap in DNS stream");
    if (capture && extra.memory_used !== undefined) {
      if (
        !Number.isSafeInteger(extra.memory_used) ||
        extra.memory_used < 0 ||
        (capture.memory_limit && extra.memory_used > capture.memory_limit)
      )
        throw new Error("Invalid DNS memory usage");
      capture.memory_used = extra.memory_used;
    }
    while (count) {
      const oldest = (next + capacity - count) % capacity;
      if (ring[oldest]!.id >= retained) break;
      ring[oldest] = undefined;
      count--;
    }
    const row = normalizeDnsClient(value);
    append(row);
    if (capture) capture.count = count;
  }

  function view(): DnsHistoryView {
    if (rowsDirty) {
      cachedRows = Array.from(
        { length: count },
        (_, i) => ring[(next + capacity - 1 - i) % capacity]!,
      );
      rowsDirty = false;
      // Legacy inline labels are rebuilt only from retained rows, never an
      // unbounded map of every device ever seen. Once/frame, not once/event.
      if (!labelsKnown) {
        names = dnsClientNames(cachedRows);
        namesDirty = true;
      }
    }
    if (namesDirty) {
      cachedNames = new Map(names);
      namesDirty = false;
    }
    return {
      queries: cachedRows,
      capacity,
      capture: capture ? { ...capture } : null,
      clientNames: cachedNames,
    };
  }
  return { ingest, view };
}
