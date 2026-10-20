import type { DnsQueryResponse } from "./dns-query-data";
import { DNS_HISTORY_MEMORY_LIMIT, MAX_DNS_CAPACITY } from "./dns-query-limits";
import { applyDnsEvent, type StreamEvent } from "./dns-query-stream";

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
