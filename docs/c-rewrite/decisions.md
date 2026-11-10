# Decision log (C rewrite)

Format: ID, status (proposed/accepted/superseded), context, decision,
consequences. Phase 0 entries are **proposed** unless marked otherwise —
final acceptance happens when the implementing phase starts.

## D-93 — DNS-safe WebUI toggles and immediate, quiet settings saves (2026-11-11)

Status: **accepted at the user's request**. This entry supersedes D-92's
original browser-preview workflow and its earlier full-runtime treatment of
HTTP enable changes. Historical decisions below remain for context.

Changing only `httpWeb.enabled` creates or removes the TCP WebUI listener;
it does not recreate the DNS proxy, DNS pipeline, cache, routing components,
event loop or Unix administrative API. The operation waits for the entire
HTTP acknowledgement to flush, and listener removal runs only after that
response callback has returned. Enabling first binds the new HTTP listener,
then persists the configuration; a persistence failure disposes the new
listener and retains the previous configuration. Disabling persists first,
then closes the HTTP listener. The browser warns about losing web access,
not a DNS interruption. Changing `httpWeb.host.address` or
`httpWeb.host.port` still follows full-runtime reconstruction and therefore
retains the DNS-interruption warning; mixed DNS changes do too.

Saving no longer calls `POST /system/settings/preview`, nor replaces that
request with background or debounced previews. The already loaded normalized
settings and edited values determine the necessary warning immediately.
The frontend comparison covers the same restart-requiring fields as the
native classifier, with regression tests for every editable field and for
reverted values. WebUI enable/disable, authentication, logging, interface-list
visibility, PTR/AAAA flags and additional TTL do not by themselves require a
DNS warning. Save-all still confirms before any writes and saves General
last, after the other changed settings sections succeed.

The existing preview API remains available for API clients. The actual PUT
retains strict validation, revision conflict detection and
`allow_dns_restart` enforcement. A changed runtime must not silently turn an
unapproved live save into a DNS restart. Refusal is an ordinary save error
with the draft preserved, not an automatic retry or an extra confirmation
flow. A 202 response means accepted, not applied: bounded operation-result
reconciliation after PUT remains, including lost-acknowledgement and
reauthentication handling, without automatically resending the mutation.

Normal progress and success do not create text rows, banners or success
toasts in Settings. The fixed-size Save button indicates work in progress;
its dimensions and the settings header/content positions remain stable.
Actionable validation, save errors and reconnection instructions remain
available, and failures never discard the draft. Browser tests cover the
absence of preflight requests and normal status captions, stable geometry
while saving and after completion, conditional warnings, preserved drafts
and safe Save-all ordering.

## D-01 — Source layout `src/backend-c/`

Status: proposed. Adopt the spec's suggested tree (include/magitrickle/ +
src/<module> + tests/{unit,integration,differential,fuzz,benchmarks}).
Go backend stays at `src/backend/` untouched until Phase 9. Makefile gains
parallel `build_backend_c` until switchover.

## D-02 — Event-driven core, epoll, small fixed thread count

Status: proposed. Single epoll loop thread for DNS UDP/TCP + timers +
signals; a small worker pool (N=2 default, configurable) only for
blocking/slow work: iptables fork+exec, libcurl transfers, shadow-file
crypt (SHA-crypt with high rounds is CPU-heavy — must not stall DNS).
Netfilter ipset adds via netlink are non-blocking-fast; measure first
(Phase 5) before moving them off-loop. Justification vs alternatives to be
re-validated by Phase 1 spike per spec §12 (not chosen "because the spec
said epoll": the workload is thousands of tiny independent exchanges —
thread-per-request is the Go model we're explicitly replacing to cut RSS).
Every queue bounded with drop counters; overflow policy per queue
documented in code and in `current` docs.

## D-03 — DNS parser: own minimal implementation

Status: proposed. See dependencies.md. Raw pass-through preserved for the
common path (bytes in → bytes out), parse only what hooks need — mirrors
Go behaviour incl. "hooks force parse" semantics. Full fuzz coverage
mandatory before Phase 3 exit.

## D-04 — YAML via libyaml with Go-yaml-v2 compatibility shims

Status: proposed. Duration strings + bare-int-nanoseconds parsing, legacy
ms/s normalization preserved (contract §1), canonical save order fixed by
explicit emitter sequence, unknown keys ignored. Differential tests are
the gate.

## D-05 — JSON via cJSON

Status: proposed. Payloads are small; availability in both feeds decides.

## D-06 — HTTP server: own bounded HTTP/1.1 on the shared loop

Status: proposed, pending Phase 1 spike vs libmicrohttpd. If the spike
shows >2 weeks of hardening effort, fall back to libmicrohttpd (LGPL,
in feeds). Hard limits (header 8 KB, body 1 MB, conns 64, timeouts) —
documented hardening, covered by tests.

## D-07 — Regex: PCRE2 with corpus-proven parity + explicit failure mode

Status: proposed. regexp2 (.NET) parity proven for the known corpus;
patterns that fail to compile in PCRE2 are rejected at load with a clear
log/API error (rule matches nothing — same net effect as Go's invalid-regex
behaviour), never silently rewritten. Add match/depth limits (regex DoS
hardening — Go version has none). Divergences documented in release notes.

## D-08 — ipset/route/link via libmnl; iptables via save/restore exec

Status: proposed. Keeps the proven Go architecture (contract §7); fake
executable seam retained for tests; transcripts from
`utils/iptables/iptables_test.go` become shared fixtures.

## D-09 — HTTP client via libcurl

Status: proposed. https subscriptions need real TLS; manual redirect loop
to preserve exact 301/302-only, ≤5, loop-detect semantics. Response size
bound added (hardening).

## D-10 — Config snapshot swap instead of Go's unlocked config mutation

Status: proposed. Go mutates `App.config` fields during SIGHUP reload with
no lock (data race, benign today). C uses immutable config snapshot +
atomic pointer swap; observable behaviour unchanged.

## D-11 — Process exit codes

Status: proposed. Go exits 0 even when `Start` fails (error only logged).
Init systems (rc.func, procd respawn) treat death as restartable either
way. C will exit non-zero on startup failure — divergence flagged for
review in Phase 1 (procd respawn behaviour must be re-checked; if it causes
respawn storms on permanent config errors, keep exit 0 for parity).

## D-12 — Rule-set snapshots (RCU-light) for the match path

Status: proposed. Match path reads an immutable snapshot (refcounted);
writers build+swap. No lock-free structures beyond atomic pointer + rc.

## D-13 — UPX decision deferred to measurements

Status: accepted (spec). Not applied automatically to C builds; Phase 8
measures startup/RSS impact.

## D-14 — Language/base: C11 + documented GNU/Linux extensions in platform layer

Status: accepted (spec). Extensions (epoll, timerfd, signalfd, pktinfo,
accept4) live under `src/platform/` and are listed in its header docs.

## D-15 — `subnet`/`subnet6` stay out of DNS matching

Status: accepted (verified in code). They are materialized into ipset by
sync only; `IsMatch` returns false. The C rule engine keeps this split
(contract §6).

## D-16 — Benchmark scope on shared CI host

Status: accepted. 1000-concurrency and on-device cells deferred (noise /
no hardware); documented in benchmark-methodology.md; device runs are a
Phase 5/8 obligation, scripts already parameterized (`GROUP_ENABLE=1`).
