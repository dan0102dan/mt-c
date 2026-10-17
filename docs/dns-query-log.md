# DNS query history and live SSE stream

Available in **Settings → DNS queries** in mt-c's C daemon.

The proxy captures each validated incoming UDP/TCP DNS question before forwarding or generating a local fake-PTR answer. A row contains `id`, `time_ms` (Unix milliseconds), `client` (IP without source port), `domain`, numeric `qtype`, and `protocol` (`udp` or `tcp`). The most recent **1000 questions** are retained in a bounded RAM ring (approximately 1.1 MiB), never written to flash or configuration exports. A daemon restart clears this history.

## API

All endpoints use the existing system API authentication. Never place the Bearer token in the URL.

- `GET /api/v1/system/dns/stream`: open SSE subscription (`Content-Type: text/event-stream`).
- `GET /api/v1/system/dns/queries`: finite JSON snapshot, retained for explicit manual/API reads, not periodic polling.
- `DELETE /api/v1/system/dns/queries`: clear the shared history and notify all connected viewers.

The SSE connection starts with a **replacement** `snapshot`:

```text
event: snapshot
data: {"queries":[{"id":1,"time_ms":123456,"client":"192.168.1.10","domain":"example.com","qtype":1,"protocol":"udp"}],"capacity":1000}

```

New requests are pushed immediately after capture:

```text
event: query
data: {"id":2,"time_ms":123457,"client":"192.168.1.11","domain":"example.org","qtype":28,"protocol":"tcp"}

```

Clearing history broadcasts `event: clear` with `data: {}`. Subsequent queries follow normally. Every connection/reconnection starts with a new snapshot, not an append to the previous UI state. This deliberately does not use `Last-Event-ID` replay: process restarts, clears and history eviction cannot leave stale rows in the browser. Disconnected clients can recover only the current bounded history, not an unlimited event archive.

## Subscription lifecycle

The Settings view opens **one HTTP connection**, using fetch streaming so the existing `Authorization` header can be sent. There is no three-second GET timer and no polling fallback. Domain search, device filter, grouping and pagination operate locally over the bounded history.

Pause, leaving Settings, hiding the browser tab, logout and unmount close the connection. Resume opens a fresh subscription and replaces the snapshot. Explicit Refresh reconnects when live; while paused it makes one finite snapshot request without resuming the subscription.

After a transport failure/EOF, reconnect uses exponential backoff (about 1–30 seconds with jitter). An invalid/expired token stops retries and resets authentication. The UI shows connecting, live, paused or reconnecting state and preserves its filters.

## Router resource limits

Journal notifications only wake subscribed sockets; JSON serialization and nonblocking writes happen on the existing epoll loop when writable. There are no per-request threads or independent per-client event queues.

At most eight subscribers share the same ring. Each HTTP stream retains at most one frame, capped at 2 MiB. A writable callback has a 64 KiB/32-frame budget so replaying history cannot monopolize one loop iteration. An evicted reader receives a replacement snapshot. A connection that cannot make write progress times out using the existing 30-second idle timeout.

A `: heartbeat` comment is sent every 15 seconds when idle. This is only a keep-alive, **not a data refresh interval**; DNS events do not wait for it. The frontend detects a connection silent for 45 seconds. Responses disable caches and send `X-Accel-Buffering: no`; a reverse proxy must preserve streaming and permit long-lived responses.

## Visibility and privacy

This is DNS metadata, not proof that a page was visited. Full URLs, page paths and response content are not stored. Requests bypassing the proxy (including many DoH/DoT clients) are invisible. Client-side DNS caching may eliminate requests entirely. If an intermediate forwarder/NAT hides client addresses, the journal shows the source address actually visible at the proxy, not an inferred device.

## Tests

`test_dns_stream` exercises the real C HTTP transport: authorization, initial snapshot, two-viewer push/clear, post-clear delivery, ring-eviction resynchronization, subscriber bounds and teardown with open streams. Frontend unit tests cover incremental framing, CR/LF boundaries, split UTF-8, bounds, replacement/deduplication and authenticated subscription cleanup. Browser tests use an actual open local HTTP response to check live updates, absence of periodic GETs, pause/resume, reconnect, filtering and mobile layout.
