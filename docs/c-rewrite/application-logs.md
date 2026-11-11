# Decision: opt-in application journal

Application-log streaming is an additive diagnostic API. The existing console
log level, stdout output, DNS capture, persisted configuration and routing
behavior are not changed by opening the WebUI.

Per the user's revised requirement, collection defaults to off and starts only
through an explicit WebUI/API action. This supersedes the earlier draft proposing
always-on startup capture, a 256 KiB ring and a 10,000-row browser limit. None of
those draft policies is implemented. Server and browser history are RAM-only and
have no arbitrary row/total-byte eviction quota. Complete logger messages are
preserved; resource failures are explicit and the normal finite-memory caveat
still applies.

A global Stop frees server history, but cannot erase already received browser
records. Browser Clear is local; reconnect/new sessions preserve prior records.
The process-local journal survives runtime reconstruction and never automatically
enables again after a process restart. Startup messages preceding Start are not
available. This choice makes the unused feature essentially dormant rather than
silently accumulating a diagnostic history for everyone.

The existing bounded HTTP streaming transport is reused. Bounded frames and
per-turn output budgets are independent of history retention. A borrowed pipe
watch safely bridges worker-thread loggers to the event loop, without per-message
HTTP work or unbounded subscriber queues.

See `../application-logs.md` for the protocol, ownership, user-visible behavior,
provenance and regression coverage. No golden snapshots of existing APIs need
changing for these additional routes.
