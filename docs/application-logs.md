# Optional application logs

The WebUI exposes **Settings → Logs**. Recording is **off after every process
start**. Opening the page or connecting a viewer is read-only; only the explicit
**Start log recording** action enables collection. Regular stdout logging and
its configured `logLevel` are unchanged. Messages emitted before Start cannot
be recovered by this feature.

## Lifetime and storage

The C process owns an append-only RAM journal while recording. There are no
fixed history-row or total-byte quotas and no silent truncation of messages.
The journal is shared between viewers and survives reconstruction of the DNS
and HTTP runtime when settings are applied. It is not saved to configuration or
a file and does not survive a process exit.

Stop is global: it disables capture and frees the router's collected records.
A subsequent Start creates a new random recording-session ID. Repeating Start
while already running is idempotent and does not erase the current session.
A slow/disconnected viewer may not have received every record before Stop frees
it; only completely received records belong to its local history.

The browser keeps its own in-memory history independently of the server. Tab
changes, capture Stop, backend disconnection, and a new recording session do
not erase received entries. New sessions have a separator. Reconnection resumes
after the last complete `(session, id)` without duplicate records. Clear is
local and does not reset the resume cursor. Logout/account changes clear the
browser's sensitive history. Page reload/closure loses it; no localStorage or
IndexedDB is used for log data.

**Save application logs** exports all received entries, including those hidden
by filters, directly from browser memory. The raw bytes (newlines, NUL and
non-UTF-8 bytes included) are preserved; display-only escaping is not exported.
No backend request is needed, so saving works after the process has died.

Unlimited retention is not unlimited physical memory: a long/noisy recording
can exhaust RAM. Allocation failures are counted and surfaced as `dropped`,
not concealed as successful delivery. Stop recording when diagnostics are done.
Messages still in the process/network at a crash cannot be guaranteed in the UI.

## API

All endpoints reuse the existing API authentication middleware.

- `GET /api/v1/logs/capture`: current state; never enables capture.
- `PUT /api/v1/logs/capture`: exactly `{"enabled":true}` or `{"enabled":false}`.
- `GET /api/v1/logs/stream`: a single authenticated SSE response, history then live.
  Reconnect with `?session=<32 lowercase hexadecimal digits>&after=<decimal ID>`.

`capture` events contain `enabled`, `session`, `through` and `dropped`. The last
two fields are decimal uint64 strings. `log` events contain `session`, decimal
`id`, byte `offset`, total byte `length`, numeric `level`, boolean `end`, and
base64 `data`. `ready` marks that the history through its fixed high-water ID
has been delivered; following events are live.

A transport fragment holds up to 12 KiB of original bytes, below the shared
64 KiB SSE-frame limit after encoding. This is a transport/work-size bound,
not a message-length or history limit. A large record spans fragments; clients
commit it only when complete and retry it after interruption. No entire-history
copy or per-viewer log queue is created on the router.

## Concurrency

The disabled capture path is one atomic revision read: no capture allocation,
mutex acquisition or notifications. Enabled producers copy owned records and
append under a mutex. Readers copy only the requested fragment while holding
that mutex; no borrowed pointer can outlive Stop. Generation checks reject a
producer that crosses Stop/Start while allocating.

Each viewer uses a coalesced nonblocking pipe notification to wake the HTTP
loop from logger/worker threads. Pipe bytes are wakeups, never log records.
HTTP watcher removal happens before the stream owner unsubscribes and closes
both pipe ends. Existing send-turn budgets, backpressure, heartbeat, stream
admission and authorization revalidation remain in effect. There is no polling
of the journal and no task allocation for every log line.

## Design provenance

The console layout, colors, filter and 16-pixel virtual rows are adapted from
Kirill (`shevernitskiy`), upstream commit
`MagiTrickle/MagiTrickle@4f94aa69daf726c6547105b2094bbd6aae55870c` (PR #33).
The old unauthenticated EventSource and component-owned history were replaced.
The full-message formatter incorporates the independent logging fix from PR #42.

## Validation

- `test_log_capture.c`: disabled fast path, full 128 KiB records and NUL,
  11,000 records without eviction, stop/start, allocation faults, notifications,
  concurrent producers and stop/start races.
- `test_application_log_stream.c`: read-only controls, input validation,
  complete replay, 140,000-byte messages, live data, resume and recording sessions.
- `application-log-history.test.ts`: fragment integrity/order, independent
  retention, resume/deduplication, multiple sessions, local Clear and exact export.
- `application-logs.spec.ts`: real chunked HTTP fixture for opt-in operation,
  tab switching, disconnect/offline export and resume. This is not a substitute
  for router hardware testing or killing a deployed daemon.

No performance improvement is claimed without measurements.

## Diagnostic message semantics

TRACE preserves individual rtnetlink notifications; an RTM_NEWLINK record
is an **observation**, not proof that the interface transitioned between
states. Each record includes the interface index, administrative flag,
operational state and carrier when present, MTU, notification type and
kernel flags/change mask. No notification is suppressed or aggregated.
These additional fields are *observational only*; link, address, route and
recovery callbacks still run for every notification.

DEBUG DNS responses identify the queried name/type, upstream server, client,
response RCODE, transport, response size and elapsed time (milliseconds,
negative if unavailable). A normal NXDOMAIN is not a routing warning.
Applied routing-address messages identify the address, domain, match, group,
configured interface/profile, effective TTL and actual attempt count.
The logger prefixes records with UTC milliseconds; SSE and the browser retain
and export the full original text.
