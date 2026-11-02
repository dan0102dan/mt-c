# Real-package OpenWrt emulator validation, 2026-10-31

## Environment and binary identity

These tests boot the same pinned Router Emulator 3.7 and official firmware
family used by repository CI. Its bundled QEMU reports
`10.1.0 (v10.1.0-53-g5fe1322)`. Both presets have **512 MiB emulated RAM**.
No physical router or direct firc comparison is involved.

| Package | Firmware | Preset | SDK / libc | Both variants |
|---|---|---|---|---|
| APK | OpenWrt 25.12.5 | Cudy WR3000P v1 | GCC 14.3.0, AArch64 Cortex-A53, musl | PASS |
| IPK | OpenWrt 24.10.4 | Cudy TR3000 v1 | GCC 13.3.0, AArch64 Cortex-A53, musl | PASS |

Each baseline/candidate pair uses the same SDK, normal package recipe, runtime
dependency list, compiler/security flags and frontend payload. The builder
checks every C/H/Makefile hash against the SDK-unpacked sources. It independently
extracts the stripped executable from the actual APK/IPK; the runner checks the
installed path and running `/proc/PID/exe` against that payload. All four source
sets still matched after the production changes were committed as `4fefbaf`.
See `emulator-build/package-summary.json` and per-version build logs/metadata.

The SDK 25 setup needed a repair to generated curl libtool auxiliaries after host
libtool 2.4.7 files mixed with SDK 2.5.4 macros. Re-running the SDK's libtoolize
and existing config.status fixed that tool mismatch. Source, configure flags
and the package recipe were unchanged; both variants use the same staging.
Exact setup notes and commands are in `emulator-build/sdk-setup.json` and the
accompanying libtool/config-status logs.

## Behavioral results

`apk-validation/` and `ipk-validation-final/` each completed successfully for
both pristine baseline `6a94817a` and candidate. Every variant passed:

- Package installation, procd startup and the host-forwarded HTTP API.
- DHCP rename propagation to live SSE and an existing journal row without
  restarting; name rediscovery after closing/reopening the journal.
- Actual IPv4/IPv6 client HTTP traffic through all six rule types: domain,
  namespace, wildcard, regex, subnet and subnet6. The LAN fixture checks traffic
  blocked before creation, a same-IP nonmatching DNS control where relevant,
  successful policy routing after matching, and blocked again after deletion.
- Exact decoded CNAME/A/AAAA response owners, targets, classes, addresses,
  record order and TTL from a compressed upstream fixture; both cold/warm UDP
  and TCP paths.
- Two overlapping groups with four A and four AAAA addresses in real kernel
  ipsets; an unmatched query resolves without adding to those sets.
- Replacing upstream TTL 300 with TTL 60 updates kernel timeouts from about 3900
  to about 3660 seconds, with a ±30-second observation tolerance.
- SIGHUP reload enables forwarding of AAAA answers while keeping the same
  daemon PID and executable hash.
- Dropped UDP reply followed by successful recovery. This is not a measurement
  of the exact UDP exchange-reclamation time.
- A dropped TCP reply closes near the configured one-second proxy deadline,
  followed by successful TCP recovery.

| TCP deadline observation | Baseline, seconds | Candidate, seconds |
|---|---:|---:|
| APK 25.12.5 | 1.007127 | 1.008033 |
| IPK 24.10.4 | 1.005974 | 1.009773 |

The ARM64 musl event-loop binaries also ran inside the 25.12.5 guest: **18 cases,
4403 assertions, all pass**, including timer pool cap, stale cancellation and
unread expiry. Their checksummed binaries and raw runtime proof are recorded in
`validation/timer-pool/arm64-runtime.json`.

The detailed client and benchmark client run inside the guest over loopback.
The separate LAN fixture proves actual forwarded client traffic; these runs do
not establish the behavior or speed of host-forwarded UDP through QEMU's user
networking. Both final smoke runs use the fixture's corrected `.252` temporary
client address, outside the emulator's reserved `.248`–`.250` range. No claim
is made that this address change explains any earlier unrelated UDP timeout.

## Harness corrections and retained attempts

Three setup attempts are retained with their exact executed scripts and notes:
`apk-correctness/` stopped on unsupported BusyBox fractional sleep after passing
the baseline LAN/DHCP and timer checks; `apk-correctness-final/` stopped before
daemon tests on a stale ephemeral mirror port in reused NAND; `ipk-validation/`
stopped before daemon tests because the mirror initially rejected `~` in an
official package filename. Integer waits, known local-mirror URL refresh and
the standard URI character were corrected. The full **17 Python helper tests**
pass, including real shell/sed regressions. All final behavioral tests were
rerun from the corrected helper for the relevant format. Failed setup attempts
are not counted as daemon failures or as successful complete runs.

The local mirror is restricted to the official 24.10.4/25.12.5 release paths;
host HTTPS and guest feed-signature verification remain enabled. It does not
copy credentials into the guest. Unique sockets, a NAND lock, explicit process
shutdown, binary hash checks and owned-process-group cleanup guard the fixture.

## Evidence

`emulator-correctness-summary.json` indexes all 24 detailed check groups,
installed executable identities, raw wire records, LAN/DHCP logs and timer
proof. Each run's `metadata.json` records archive, QEMU binary, preset,
official firmware and pre-boot NAND hashes, every served helper/config hash,
exact parameters and package build metadata. `harness-executed.py` and the
executed LAN/DHCP shell scripts preserve the actual code used for each run.

## Paired QEMU performance experiment

`apk-benchmark/` is a separate completed 25.12.5 session with the same two APKs,
real daemon and real kernel ipsets. QEMU is pinned to host CPUs 4/5; the compressed
upstream stub to CPU 6. The Go loader runs **inside the guest** over loopback with
`GOMAXPROCS=1`, competing for emulated CPU resources. Configuration is the same
on both variants: 1000 enabled namespace rules, 32 query names, 10 clients. Each
case has three pairs of ten-second samples after two seconds of warm-up, fresh
daemon per sample and alternating variant order. Before every measurement the
runner checks exact DNS records, real expected ipset addresses and remaining
TTL. Known builds/tests and native loads were stopped during this session.

The **18 measured samples contain 170,855 valid queries**, with zero reported
errors, timeouts or invalid replies. Warm-up queries are excluded from this
count. Every paired sample uses a verified running executable from its actual
package. Raw per-run values are in `apk-benchmark/results.jsonl`; the combined
marginal and paired summaries are in `emulator-performance-summary.json`.

| Workload | Baseline QPS median [range] | Candidate QPS median [range] | QPS change | Daemon CPU/query change |
|---|---:|---:|---:|---:|
| UDP, 1 A |1215.38 [1214.07,1344.83]|1813.51 [1736.10,2088.93]|+49.21%|-41.68%|
| UDP, 2 CNAME + 4 A |712.85 [643.24,796.54]|1224.78 [1218.89,1327.47]|+71.81%|-48.37%|
| TCP, 2 CNAME + 4 A |270.99 [262.18,303.57]|281.71 [279.51,340.34]|+3.96%|-13.06%|

Percentages compare marginal medians. UDP QPS is higher and daemon CPU/query
lower in all three pairs for both UDP shapes. TCP QPS is higher in two of three
pairs with overlapping ranges; its observed speed difference is inconclusive.
Daemon CPU/query is lower in all nine pairs, but this excludes loader, upstream
and the remaining QEMU process CPU.

After-load snapshots show two timerfds on the baseline and 20–22 on the
candidate. The pool deliberately retains reusable watches; its cap of 32 is
covered by the unit tests. Observed daemon RSS spans 2660–2776 KiB on the
baseline and 2676–2764 KiB on the candidate across these samples. These snapshots
are not peak-RAM or kernel-memory measurements.

| Workload | p95 median baseline → candidate, ms | p99 median baseline → candidate, ms |
|---|---:|---:|
| UDP, 1 A |12.324 →10.837|16.625 →14.208|
| UDP, 2 CNAME + 4 A |18.435 →12.987|23.928 →16.698|
| TCP, 2 CNAME + 4 A |57.883 →56.163|68.265 →68.013|

The short-run tail samples vary: UDP CNAME baseline p99 spans 23.207–88.063 ms
and candidate 16.604–45.286 ms; TCP p99 ranges overlap. These are observed ranges,
not confidence intervals. Guest TCP uses a different answer shape from native
TCP and does not invalidate the native capacity p99 regression reported in
`host-results.md`.

The larger UDP gains under QEMU must not be projected onto physical hardware:
TCG changes instruction and syscall costs, and the guest client competes with
the daemon. These measurements establish a speedup for these emulated UDP
fixtures. They do not measure real-router throughput, host-forwarded UDP,
Internet data bandwidth or superiority to firc. All emulator and helper
processes exited after the completed session; no additional timing run was
selected to replace an unfavorable sample.
