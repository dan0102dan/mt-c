# Opt-in DNS history with a 1 MiB storage budget

Settings → DNS queries displays questions passing through mt-c's DNS proxy.
**Recording is disabled after every daemon/router start.** Reading the API,
opening Settings or subscribing to SSE never enables it. Only an explicit
Start recording action does. The WebUI asks for confirmation that history
will increase RAM use (up to 1 MiB for storage plus streaming/UI overhead).

Capture state is shared by all viewers and is not saved in YAML, configuration
exports or localStorage. Enabled recording continues after closing the page.
Stop recording disables collection, deletes history and frees the shared
byte store; a small control object remains. Existing viewers
receive the state change. Pause only freezes the local view; it does not stop
capture. Clear removes retained rows while leaving capture enabled.

## Memory and eviction

Production history is limited **only by a 1,048,576-byte requested-allocation
budget**, including its control object, page directory, hash buckets, key
metadata, record pages and all stored names/addresses. There is no independent
8192/60000-record retention limit. The `capacity` response field is a conservative
upper bound derived from bytes and the encoding, not an allocation of that many
records and not a second eviction rule. Clients must follow `retained_from`.

Enabling capture reserves the byte store once; appending and eviction make no
heap allocations. While off only the small control object remains. `memory_used`
reports **reserved requested bytes**, including unused pages and size-class
slack, so an enabled empty journal already reports the 1 MiB reservation. Stop
frees the store; Clear keeps the enabled store but resets its contents.

This is not a cap on total daemon RSS. Bookkeeping for the two top-level heap
allocations, JSON/network buffers, device-name discovery and browser memory are
additional. There is no per-record native-allocator overhead.

A normal record is **10 bytes**: a 30-bit millisecond offset and two flags, 16-bit
domain/client handles and the complete 16-bit query type. IDs are reconstructed
from the monotonic sequence, not stored in every row. Each record page has a
64-bit timestamp base. Backwards clock changes or offsets exceeding the encoded
range use a separate full signed 64-bit timestamp, preserving every millisecond
including INT64_MIN/MAX. These exceptional slots also count against the budget.

Wire-format domains and binary IPv4/IPv6 addresses are reference-counted in
shared, nonmoving size-class pages. Metadata is allocated for actual keys, not
for the maximum possible record count. Key reference counts are 32-bit, including
when more than 65535 retained requests share one name. Unknown clients use no
key. Case, escaping, address family and every raw question remain lossless.

Records and keys compete for the same pages. Under byte pressure the oldest
rows are evicted until the necessary slot/page can be reused, possibly several
rows at a time. Unused slab pages immediately return to the shared pool; no
whole-history movement or compaction occurs. Record lookup by ID remains O(1).
A page has 408 records, so freeing a record page never requires scanning the
whole history. The default budget's page count and handle representation are
checked at compile time; smaller-byte test constructors have no row quota either.

Failure to allocate the store leaves recording disabled, without discarding
history or stopping forwarding. After successful enable there is no allocation
failure on append; `dropped` remains a compatible counter (zero in this
allocation-free path). An unavailable control object leaves DNS operational and
journal endpoints return 503 / `DNS_LOG_UNAVAILABLE`. No compression library,
flash logging or persistent archive is added.

Reproduction, workload-dependent retention, exact accounting and timing tradeoffs
are documented in `dns-byte-budget.md`. The earlier review measurements remain in
`dns-review-performance.md` as historical results, not measurements of this layout.

## Related questions in one display row

The default timeline combines A (IPv4), AAAA (IPv6) and HTTPS (type 65,
**HTTPS parameters**) from the same client, case-insensitive domain and transport
within one second of the first question. The window does not slide. Repeated
questions of the same type begin another group, so retries are not swallowed.
Other types and different clients/transports remain separate.

Rows appear immediately and expand as sibling questions arrive. Expanding a row
shows every raw type and millisecond timestamp. The timeline combines
related types automatically, without a separate Combine queries button.
This is presentation only: raw events remain separate in RAM and the stored
request count is not the number of display groups. Global domain grouping,
search, device filters, pagination and router-provided device names are retained.

The clickable **Time** header uses the same sort indicators as Groups → Pattern,
but has only two states: **newest first** (default) and **oldest first**. In
aggregate domain mode, **Last request** controls the same direction using the
latest timestamp for each domain, rather than request count. Sorting is applied
to the complete filtered display before pagination. Switching it returns to the
first page; filters, related-query membership and the live subscription are
preserved. Equal timestamps keep stable order, and new SSE records and
reconnected snapshots obey the selected direction. This is local view state;
no router configuration or retained/streamed order is changed.

## Authenticated API and SSE

- `GET /api/v1/system/dns/capture` reads runtime state.
- `PUT /api/v1/system/dns/capture` accepts exactly `{"enabled":true}` or
  `{"enabled":false}`. Invalid/trailing input and extra fields are rejected.
- State contains `enabled`, `memory_limit`, `memory_used`, `count`, `capacity`, `dropped`.
- `GET /api/v1/system/dns/stream` is one long-lived authenticated SSE response.
- `GET /api/v1/system/dns/queries?limit=128&before=1234` is a bounded page for
  explicit consumers, newest first. `limit` is 1..128 (default 128); `before`
  is an optional exclusive positive ID. `next_before` is the next cursor or
  zero when no older retained rows remain. `retained_from` signals eviction.
  It never constructs the entire journal. Use SSE for a complete initial view.
- `DELETE /api/v1/system/dns/queries` clears retained history without changing
  enabled state; the response includes updated capture state and `cleared`.

Streams begin with a `capture` state event, then `snapshot_begin`, batches of at
most 32 rows in `snapshot_chunk`, and `snapshot_end`. New queries are pushed
immediately. Enable/disable changes are broadcast with `capture`; clearing uses
`clear`. Live rows carry `retained_from` so the WebUI drops byte-budget evictions
independently of the byte-derived admission bound, and `memory_used` updates the usage display.

Snapshots may exceed 2 MiB in total, but each event is bounded to 64 KiB. The
browser stages chunks and atomically replaces history after complete validation.
Reconnects, clearing or eviction during a snapshot restart staging. Bearer tokens
remain in headers, never URLs. No polling or automatic capture-start fallback
exists. Hidden/inactive/paused views close their local subscriptions; background
capture continues if explicitly enabled. Read-only viewers do not start client
inventory lookup while capture is off.

The eight-subscriber limit and bounded writable-callback work remain. One
persistent timer per connection checks the last write progress, idle heartbeat
and authorization. Burst notifications only arm EPOLLOUT once. Every successful
send updates a timestamp, never creates a timerfd. The same admission middleware
revalidates credentials at most once/second during streaming, including expiry,
password changes and auth becoming enabled. HTTP mutations/config reload also
recheck streams; external credential changes have at most a one-second recheck
interval, subject to event-loop scheduling. Client-side logout is not the
security boundary.

Slow snapshots have a total 30-second deadline and at most three eviction
restarts. A lagging subscriber is closed rather than endlessly replaying
history. Browser reconnect remains exponentially backed off.

Current client labels use a separate atomic `clients_begin {count}` →
`clients_chunk {offset,clients:[{ip,name}]}` → `clients_end {}` transfer, at
most 32 labels per event / 2048 total. Alias changes do not restart a DNS
snapshot or replay request rows. They describe current router labels, not
historical device ownership of an IP.

The WebUI ingests queries into a bounded, non-reactive ring and publishes
immutable row snapshots at most once per animation frame. It does not queue
unbounded events or copy/filter the full history for every received request.
Pause, clear, capture consent and raw-event preservation remain unchanged. History is volatile DNS metadata,
not visited URLs; cached queries and encrypted DNS bypassing the proxy are not
captured. The address seen by the proxy may be a forwarding/NAT device.

## Tests

Core tests cover disabled no-op behavior, explicit start, idempotent toggles,
freed buffers, restart defaults, subscriber neutrality, and 24,000 maximum-name
requests under the shared byte cap. HTTP tests cover strict authorized control
and state broadcasts, in addition to existing chunking/auth/clear tests.
Additional regressions cover more than 100000 retained records, slab reuse, all
wire-name size classes, arbitrary IPv4/IPv6 clients, full-range timestamps,
clear/re-enable sequence continuity and zero heap/moving operations on append.
Frontend tests cover 100000-row snapshot staging and byte-pressure eviction,
anchored-window grouping, raw-event preservation, retention
boundaries, RAM confirmation cancellation, page reload and multi-viewer stop.
