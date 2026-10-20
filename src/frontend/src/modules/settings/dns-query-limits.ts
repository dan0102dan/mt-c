/** Defensive wire validation, derived from the router byte budget and minimum
 * lossless record size. This is not a configurable/history eviction quota.
 * Actual retention comes from the server's retained_from sequence boundary. */
export const DNS_HISTORY_MEMORY_LIMIT = 1024 * 1024;
export const DNS_HISTORY_RECORD_BYTES = 10;
export const MAX_DNS_CAPACITY = Math.floor(DNS_HISTORY_MEMORY_LIMIT / DNS_HISTORY_RECORD_BYTES);
