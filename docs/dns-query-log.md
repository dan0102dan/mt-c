# Compact DNS history and live SSE

Settings → DNS queries displays validated incoming UDP/TCP DNS questions passing through mt-c's proxy. Rows contain `id`, `time_ms`, `client`, `domain`, numeric `qtype` and `protocol`. DNS metadata does not prove a page was visited; URLs, paths and response payloads are not recorded. Client caches, encrypted DNS bypassing this proxy, and addresses hidden by NAT/forwarders cannot be reconstructed.

## Bounded RAM storage

The default remains **1000 individual questions**. Closing/reloading the browser does not stop server-side collection. Restarting the daemon/router or explicit clearing discards history; nothing is written to flash or config exports.

The ring uses **24-byte records**, not the public materialized `mt_dns_query_entry_t` structure. Records retain ID, full timestamp, query type and transport, referencing two bounded dictionaries. Exact decompressed DNS wire names (up to 255 bytes, including root) and binary IPs are stored once per distinct key. Case and binary label bytes are preserved; duplicate requests are NOT merged. IPv4 and IPv4-mapped IPv6 remain distinct.

Each dictionary has fixed-capacity handles/hash buckets and a lazily growing byte pool with length-prefixed, aligned records. Zero-reference keys are released on eviction. Pools compact in place and shrink under low utilization; clearing releases the pools. No per-repeat-key allocation, unbounded string cache, or second full-sized compaction arena is used. One spare key slot permits acquiring replacement keys before evicting a row. Allocation failure leaves existing rows and IDs intact. Access is loop-local; no packed/unaligned C structure loads are required on MIPS.

`mt_dns_query_log_stats` reports requested allocation bytes including unused pool reserve, but **not allocator bookkeeping, RSS, temporary JSON/HTTP buffers or browser memory**. `tools/bench/dns_query_log_memory.c` reproduces these synthetic x86-64 measurements:

| Rows | Unique names | Clients | Name length | Allocated bytes | KiB |
|---:|---:|---:|---|---:|---:|
| 1000 | 200 | 20 IPv4 | 31 text characters | 73,868 | 72.1 |
| 1000 | 1000 | 20 IPv4 | 31 text characters | 109,406 | 106.8 |
| 1000 | 1000 | 1000 IPv6 | 255 wire bytes | 348,892 | 340.7 |
| 10000 | 2000 | 20 IPv4 | 31 text characters | 791,403 | 772.9 |
| 10000 | 10000 | 20 IPv4 | 31 text characters | 1,196,203 | 1168.2 |

The old 1000-row array alone required 1,096,000 bytes on x86-64. Actual traffic mix and ABI affect totals; the table is not a router RSS benchmark. No compression library or additional runtime dependency is needed.

The sized constructor accepts 1..60000 rows for embedding/tests; `MT_DNS_QUERY_LOG_CAPACITY` is a build-time default. This change does not add a YAML setting or silently increase the production retention limit.

## Authenticated HTTP API

- `GET /api/v1/system/dns/stream`: one long-lived `text/event-stream` response.
- `GET /api/v1/system/dns/queries`: finite JSON snapshot for explicit/manual reads; no periodic polling.
- `DELETE /api/v1/system/dns/queries`: clear the shared journal and notify viewers.

The stream starts with **multiple bounded events**, not one giant JSON snapshot:

```text
event: snapshot_begin
data: {"capacity":1000,"count":2,"through":12}

event: snapshot_chunk
data: {"offset":0,"queries":[{"id":11,"time_ms":123,"client":"192.168.1.10","domain":"example.org","qtype":1,"protocol":"udp"},{"id":12,"time_ms":124,"client":"192.168.1.11","domain":"example.org","qtype":28,"protocol":"tcp"}]}

event: snapshot_end
data: {"through":12}

```

Chunks contain at most 32 rows in ascending ID order. `through` is a fixed high-water mark; new requests follow the completed snapshot as `query` events. The browser validates count, capacity, offsets and contiguous IDs, stages at most the declared capacity, then replaces the displayed history atomically on `snapshot_end`. It does not serialize the completed history into another giant JSON string. Filters/grouping/pagination stay local and unchanged.

Clearing during a snapshot or evicting its unread rows starts a new `snapshot_begin`, abandoning previous staging. Clearing during live streaming emits `clear` with `{}`. Reconnection always starts a fresh snapshot, and discards any unfinished old one. There is no `Last-Event-ID` replay archive; recovery is limited to retained history. A very slow reader may need a fresh snapshot rather than receiving every event.

**The total history/connection is not limited to 2 MiB.** Each event has a 64 KiB safety limit on server and client, including escaped longest legal names. The C server materializes at most 32 rows for a snapshot event, not the full journal. Temporary event/HTTP buffers remain bounded independently of history length. The finite manual JSON endpoint still materializes its explicitly requested snapshot.

At most eight subscribers share the same ring. Per-writable-callback work remains bounded to 64 KiB/32 events. Idle heartbeat comments every 15 seconds maintain connectivity; DNS events do not wait for a timer. Nonprogressing writers time out. Responses set `Cache-Control: no-store, no-cache, no-transform` and `X-Accel-Buffering: no`; reverse proxies must preserve streaming.

The frontend uses fetch streaming with the existing Bearer header, never a token in the URL. Pause, leaving Settings, hidden tab, logout and unmount abort the subscription. Resume reconnects; transport failures use bounded exponential backoff. Unauthorized responses stop retries. While paused, Refresh performs one finite read without resuming SSE.

## Verification

Core tests cover reference-count reclamation, 20,000-row churn with model comparisons, pool compaction/shrink, clear, reused handles, case, maximum binary names, IPv4/IPv6 and invalid inputs. Real C HTTP tests exercise authorization, multi-viewer push/clear, a >2 MiB snapshot in bounded events, interrupted snapshots and subscriber teardown/limits. Frontend unit tests exercise framing/UTF-8, >2 MiB cumulative transfer, malformed/incomplete chunks and authenticated lifecycle. Browser tests use a genuinely open response, including partial-snapshot staging and reconnection without polling.
