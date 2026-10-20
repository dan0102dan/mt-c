# Opt-in DNS history with a 1 MiB storage budget

Settings → DNS queries displays questions passing through mt-c's DNS proxy.
**Recording is disabled after every daemon/router start.** Reading the API,
opening Settings or subscribing to SSE never enables it. Only an explicit
Start recording action does. The WebUI asks for confirmation that history
will increase RAM use (up to 1 MiB for storage plus streaming/UI overhead).

Capture state is shared by all viewers and is not saved in YAML, configuration
exports or localStorage. Enabled recording continues after closing the page.
Stop recording disables collection, deletes history and frees the ring,
dictionaries and string pools; a small control object remains. Existing viewers
receive the state change. Pause only freezes the local view; it does not stop
capture. Clear removes retained rows while leaving capture enabled.

## Memory and eviction

Production storage has a **1,048,576-byte requested-allocation budget** including
24-byte ring records, dictionary metadata, hash buckets, reserved string pools
and the control object. Pools share the remaining byte budget instead of each
reserving a megabyte. Unused reserve is reclaimed before oldest records are
evicted. No history buffers are allocated while off.

This is not a cap on total daemon RSS: allocator bookkeeping, temporary realloc
copies, JSON/network buffers, client-name discovery and browser memory are
additional. The UI warns about this and displays history usage separately.

The production ring has at most **8192 individual records**. The byte budget may
evict earlier depending on unique domain lengths and clients. In a local
synthetic maximum-length-name/IPv6 stress test, 1844 rows used 1,048,448 bytes;
short repeated names retain more. This is not a traffic prediction or router
RSS measurement. The explicitly enabled, row-bounded `create_sized` constructor
remains available for tests/embedders; its `memory_limit=0` does not describe
production behavior.

Each raw question retains ID, timestamp, type, protocol and exact domain/client
identity. Reference-counted dictionaries store decompressed wire names and
binary IPs once. Eviction releases unused keys, and pools compact/shrink. Stop
releases all history buffers. No compression dependency, flash logging or
background persistent archive is added.

## Related questions in one display row

The default timeline combines A (IPv4), AAAA (IPv6) and HTTPS (type 65,
**HTTPS parameters**) from the same client, case-insensitive domain and transport
within one second of the first question. The window does not slide. Repeated
questions of the same type begin another group, so retries are not swallowed.
Other types and different clients/transports remain separate.

Rows appear immediately and expand as sibling questions arrive. Expanding a row
shows every raw type and millisecond timestamp. Combining can be toggled off.
This is presentation only: raw events remain separate in RAM and the stored
request count is not the number of display groups. Global domain grouping,
search, device filters, pagination and router-provided device names are retained.

## Authenticated API and SSE

- `GET /api/v1/system/dns/capture` reads runtime state.
- `PUT /api/v1/system/dns/capture` accepts exactly `{"enabled":true}` or
  `{"enabled":false}`. Invalid/trailing input and extra fields are rejected.
- State contains `enabled`, `memory_limit`, `memory_used`, `count`, `capacity`.
- `GET /api/v1/system/dns/stream` is one long-lived authenticated SSE response.
- `GET /api/v1/system/dns/queries` is a finite snapshot for explicit consumers.
- `DELETE /api/v1/system/dns/queries` clears retained history without changing
  enabled state; the response includes updated capture state and `cleared`.

Streams begin with a `capture` state event, then `snapshot_begin`, batches of at
most 32 rows in `snapshot_chunk`, and `snapshot_end`. New queries are pushed
immediately. Enable/disable changes are broadcast with `capture`; clearing uses
`clear`. Live rows carry `retained_from` so the WebUI drops byte-budget evictions
even below the ring's slot count, and `memory_used` updates the usage display.

Snapshots may exceed 2 MiB in total, but each event is bounded to 64 KiB. The
browser stages chunks and atomically replaces history after complete validation.
Reconnects, clearing or eviction during a snapshot restart staging. Bearer tokens
remain in headers, never URLs. No polling or automatic capture-start fallback
exists. Hidden/inactive/paused views close their local subscriptions; background
capture continues if explicitly enabled. Read-only viewers do not start client
inventory lookup while capture is off.

The existing eight-subscriber limit, bounded writable-callback work, idle
heartbeat and stalled-writer timeout remain. History is volatile DNS metadata,
not visited URLs; cached queries and encrypted DNS bypassing the proxy are not
captured. The address seen by the proxy may be a forwarding/NAT device.

## Tests

Core tests cover disabled no-op behavior, explicit start, idempotent toggles,
freed buffers, restart defaults, subscriber neutrality, and 24,000 maximum-name
requests under the shared byte cap. HTTP tests cover strict authorized control
and state broadcasts, in addition to existing chunking/auth/clear tests.
Frontend tests cover anchored-window grouping, raw-event preservation, retention
boundaries, RAM confirmation cancellation, page reload and multi-viewer stop.
