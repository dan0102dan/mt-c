# Application logs: imported design and proposed C backend

Status: design port only. There is no application-log capture/store/endpoint in
this change, and the Logs navigation entry remains disabled until integration.
The production backend and existing DNS capture behaviour are unchanged.

## Provenance

- Original author: shevernitskiy (Kirill).
- Source: https://github.com/MagiTrickle/MagiTrickle/commit/4f94aa69daf726c6547105b2094bbd6aae55870c
- Original PR: https://github.com/MagiTrickle/MagiTrickle/pull/33
- Original component blob: `2d06e4b4b2f917ce6c3893bfa1bab8fdeb8de83c`.
- Target: `src/frontend/src/modules/logs/LogsPanel.svelte`.

This is an adapted port, not a literal cherry-pick of the old Layout/mock paths.
The console design, level colours, 16px virtualized rows, filter and three
controls are retained. Current shared UI components replace `../common/`.
The original unauthenticated EventSource and component-owned history are not
ported: transport and retained records must belong to the application store.
`entries` are validated, bounded presentation records; `labels` and `status`
are localized with `t()` by the eventual owner (add corresponding ru.json keys
when wiring the route). The presentation model is not a frozen wire contract.
Export saves all locally retained rows, independent of filters or connectivity.

## Reuse verified in the current C code

- `src/backend-c/src/api/http_stream.inc`: existing bounded HTTP SSE transport,
  loop-local connection ownership, heartbeat, authorization revalidation,
  send budgets and stalled-client timeout.
- `src/backend-c/src/api/dns_stream.inc`: chunked history followed by live events.
- `src/backend-c/src/dns/query_log.c`: bounded retention pattern, not a generic
  log store. Its DNS interning/page allocator and clear semantics stay separate.
- `src/backend-c/src/logging/log.c`: hook the existing `mt_log()` formatter;
  currently it writes at most 1024 bytes per line to the configured fd.
- `src/backend-c/include/magitrickle/loop.h`: bounded thread-safe `mt_loop_post()`.

Do not read stdout back, shell out to tail/logread, add a Go service, or infer
application records by parsing DNS requests. Keep normal stdout logging and its
level setting. Reading logs must not enable debug/trace or restart DNS.

## Proposed retention and capture (not implemented)

Start with a 256 KiB total preallocated application ring, including its index and
metadata. Derive slot count from the actual struct sizes; 256 slots of 1024 bytes
PLUS metadata would exceed that budget. Preserve the current 1024-byte formatted
line limit and mark truncation. Evict whole oldest records on capacity pressure;
never grow memory, block DNS for a browser, or write an unlimited router file.
Capture from daemon startup even with no viewer, subject to the configured level.
History is from the current process, not a promise to recover prior boots.

Logger calls can come from worker threads, unlike the loop-owned DNS journal.
Use bounded concurrent insertion with short copy-only critical sections (try-lock
and an atomic drop counter on contention is an acceptable first implementation).
Copy owned bytes; do not retain logger stack pointers. Assign a sequence under
synchronization. Readers copy a bounded batch under the lock and serialize after
unlocking. A slow reader must not pin overwritten storage or receive torn rows.

Schedule one coalesced loop wakeup, not a task per line. Never call the loop-local
`mt_http_stream_wake()` directly from workers. Handle a full mt_loop_post queue
and the pending-flag race; a bounded maintenance retry must avoid a permanently
lost wakeup. Stop/detach notifications before loop destruction and join producers
before freeing shared storage. Do not log from the capture/error path recursively.
Existing stdout writes are a separate best-effort sink; the UI must add no waits
for networking or disk. A blocking stdout sink is not made nonblocking by this work.

## Proposed stream contract (not implemented)

An authenticated `GET /api/v1/logs/stream` reuses mt_http_stream, not the old Go API.
Use the existing browser streaming/auth approach (fetch with Bearer headers), not
an EventSource that silently omits Authorization or puts tokens in URLs.

Each process has a fresh boot_id and each record a monotonic sequence. Send 64-bit
sequences as decimal strings so JavaScript does not lose precision. Identity is
(boot_id, sequence), independent of wall-clock time. A suggested envelope includes
time, level, message, optional fields and truncation; normalize it into LogEntry.

Initial connection: capture a high-water sequence, replay at most the latest 200
records AND 64 KiB of encoded snapshot data, then continue strictly after that
high-water mark. Generate bounded chunks on demand. Never copy the whole ring for
each viewer or retry a moving snapshot indefinitely. If overwrite wins, emit a
visible gap with a reason/range and continue from the earliest retained ID.
Distinguish initial history omitted by the chosen tail limit from records dropped
after subscription, overwrite gaps, contention loss and client-local eviction.

Reconnect: resume after the last accepted (boot_id, sequence), deduplicate replay,
and report a new session on changed boot_id. A reconnect/snapshot must NEVER clear
records already owned by the browser. Keep one stream per authenticated app,
reuse the existing aggregate stream/client/write-buffer limits, and reserve
bounded per-client cursor/frame state only. Validate frame bytes before parsing;
cap unfinished SSE buffers as well as parsed records. Oversized/malformed input
must not grow a string indefinitely. Retain existing heartbeat/auth expiry rules.

## UI lifetime and crash behaviour (not implemented)

Store history outside LogsPanel, at authenticated-application scope. Start the
subscription with the authenticated UI, not only when the Logs tab is visible;
otherwise a crash while viewing Groups leaves nothing in the browser. Switching
tabs or temporarily losing the backend must not destroy the retained data.

Proposed client cap: both 10,000 rows and an 8 MiB accounted text budget, plus
bounded parser/batch buffers. This is NOT a promise of an 8 MiB JavaScript heap:
objects, strings, indexes, filtered references and export blobs have overhead.
Do not retain duplicate unbounded arrays or pending animation-frame batches.
Update display in batches and keep virtualized rows. Stop autoscroll when the
user reads older records; pause-display and pause-capture are different actions.

On connection loss, retain/freeze the current diagnostic tail and show one inline
connection status, not repeated toasts or a page reload. The default first version
should require Resume to append a new session, so an automatically restarted,
noisy daemon cannot immediately overwrite the crash evidence. While frozen,
export and search continue locally. Manual clear affects this browser only and
must not rewind the resume cursor or clear the router/other viewers.

No persistence to localStorage/IndexedDB by default. Closing/reloading the browser
loses its in-memory history; .log export is the explicit durable copy. Logout
clears sensitive history, and different routers/accounts must never share a store.
No raw HTML/ANSI interpretation or secrets in diagnostic records. Future log
fields must not include Authorization, credentials or full sensitive configs.

Only records fully received and accepted before a disconnect can be guaranteed
in the UI. SIGKILL, power loss, a crash inside formatting, buffered network data
and a disconnected browser can lose the final records. A RAM ring plus SSE is
not a durable audit log or a crash-signal unwinder.

## Implementation acceptance checks

C: small/full ring, byte budget including metadata, wraparound, UTF-8 truncation,
allocation failure, concurrent writers/readers, sequence consistency, full post
queue, notification teardown, stopped readers and aggregate SSE limits. Snapshot
races must produce bounded work and explicit gaps, not hangs or silent resets.
Run the project unit/sanitizer suites and router emulator; measure DNS load with
logging idle, active, flooding and with a stalled viewer before claiming overhead.

UI/integration: history+live ordering, dedup/reconnect/new boot, malformed/oversized
frames, parser/batch caps, memory plateau, search/virtualization, tab retention,
local clear cursor, expired auth, logout and router change. Kill the actual daemon
while the UI is open: accepted records must remain searchable/exportable. Restart
it and verify the frozen tail is not overwritten until Resume.

The design port has 10 dependency-free presentation tests for level/filter,
export/escaping and virtual-window behaviour. They were executed via a Node
adapter for the Deno.test calls, and log-view.ts passed standalone strict tsc.
The full Deno/Svelte build and browser/router tests have NOT been run in this
environment; the branch is a preparatory design port, not a completed feature.
