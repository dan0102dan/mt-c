# Final OpenWrt emulator validation, 2026-10-31

Both final package formats pass the functional checks. In the separate QEMU
performance series, the final candidate improves the two UDP workloads in all
three pairs. **TCP has no demonstrated win:** marginal QPS falls 1.30%, p95 rises
8.11%, p99 rises 10.32%, and CPU/query rises 2.77%; paired directions and ranges
are reported below. These results are retained without replacement runs.

This report covers pristine baseline `6a94817a` and the final restricted
candidate whose production source matches `1763aff5a780343f1c1673586a046b30d99b9168`.
Reusable deadline timerfds and unchanged raw-response forwarding are enabled
only for UDP; TCP retains ordinary fresh timers and historical repacking.
The earlier broader candidate, its setup attempts and measurements are
preserved separately in `experimental-results.md` and
`experimental-emulator-results.md`.

## Environment and fresh build provenance

The tests use pinned Router Emulator 3.7 and the official firmware family used
by repository CI. Bundled QEMU reports `10.1.0 (v10.1.0-53-g5fe1322)`. Both
presets have **512 MiB of emulated RAM**. No physical router or direct firc
comparison is involved.

| Package | Firmware | Preset | SDK / libc | Final result |
|---|---|---|---|---|
| APK | OpenWrt 25.12.5 | Cudy WR3000P v1 | GCC 14.3.0, AArch64 Cortex-A53, musl | Both variants PASS |
| IPK | OpenWrt 24.10.4 | Cudy TR3000 v1 | GCC 13.3.0, AArch64 Cortex-A53, musl | Both variants PASS |

Each pair uses the same SDK, normal package recipe, staged dependencies,
compiler/security flags and frontend payload. All four final builds stage and
hash the fresh source archive after SDK clean, verify C/H/Makefile hashes in the
unpacked source, remove that verified backend's build directory, then compile.
The builder independently extracts and hashes the stripped executable from the
actual APK/IPK; the runner checks the installed path and running `/proc/PID/exe`
against that payload. All 144 frozen production-manifest paths match both
candidate package builds. `final-emulator-build/package-summary.json` and the
per-version metadata/logs record these checks.

| Firmware | Variant | Actual package executable SHA256 |
|---|---|---|
| 25.12.5 | baseline | `acfeacf8d88965cb148bb864bfc9dcdb99441a592c110bdc8f64b5592b6550d0` |
| 25.12.5 | candidate | `fea57b6b3e7bff4c9e0cdeb8b34bf824578074212a037b2a5e97e4c3f75cb68b` |
| 24.10.4 | baseline | `42778da919a1735d8a70043b7a822a9c24ba829a7d9c5b1bd24c5b8a28ec0a3e` |
| 24.10.4 | candidate | `85a9f3fbcb2cf8037d671cffc823341a32a6012a69321eefff1b426091ecf39c` |

## Functional results and explicit reuse

`final-apk-validation/` completed full baseline and candidate DHCP/SSE and LAN
routing smoke tests, followed by fresh detailed DNS/kernel checks for both.
`final-ipk-validation/` completed fresh package identity and detailed DNS/kernel
checks for both variants plus the candidate's full DHCP/SSE and LAN smoke.

Only the IPK baseline's LAN/DHCP smoke was reused from the completed
`ipk-validation-final/` experiment. The final helper required identical actual
baseline executable identity, QEMU, firmware, preset, launcher and fixture
hashes, plus complete passing logs. It recorded those exact proof hashes in
`final-ipk-validation/metadata.json`. The baseline APK executable changed after
the fresh-build correction, so its full smoke was rerun. No candidate smoke or
detailed kernel check was reused.

The verified behavior covers:

- Package installation, procd startup and host-forwarded HTTP API access.
- DHCP rename propagation to live SSE and an existing journal row without a
  restart; name rediscovery after closing and reopening the journal.
- Actual IPv4/IPv6 client HTTP traffic through domain, namespace, wildcard,
  regex, subnet and subnet6 rules. The LAN fixture checks blocked traffic before
  creation, same-IP nonmatching DNS controls where relevant, successful policy
  routing after matching, and blocked traffic again after deletion.
- Exact decoded owners, CNAME targets, record types/classes/order, addresses
  and TTL from compressed upstream responses, on cold/warm UDP and TCP paths.
- Two overlapping groups receiving four A and four AAAA addresses in real
  kernel ipsets. An unmatched query resolves without adding addresses.
- Upstream TTL 300→60 replacing kernel timeouts of about 3900→3660 seconds,
  with a ±30-second observation tolerance.
- SIGHUP enabling AAAA forwarding while preserving daemon PID and binary hash.
- A dropped UDP reply followed by successful recovery; this does not measure
  the exact time of exchange reclamation.
- A dropped TCP reply closing near the configured one-second proxy deadline,
  followed by successful TCP recovery.

There are **24 fresh detailed check groups**, six per variant/firmware pair.
Raw wire values and kernel entries remain in each run's DNS records and
`correctness.json`; `emulator-correctness-summary.json` indexes them and the
exact smoke proof used.

| TCP deadline observation | Baseline, seconds | Candidate, seconds |
|---|---:|---:|
| APK 25.12.5 | 1.007873 | 1.008479 |
| IPK 24.10.4 | 1.007972 | 1.007583 |

Final ARM64 musl event-loop tests also ran inside the 25.12.5 guest: **21 cases,
4943 assertions, all PASS**, including fresh ordinary timers, explicit reusable
timers, cap 32, stale cancellation and unread expiry. Binaries were checked
against their independently recorded SHA256 before execution. Exact runtime
proof is in `validation/udp-timer-final/arm64-runtime.json`.

The detailed DNS client and performance loader use guest loopback. The separate
LAN fixture proves actual forwarded client traffic; this does not measure
host-forwarded UDP through QEMU user networking. All final smoke fixtures use
`.252`, outside the emulator's reserved `.248`–`.250` range. This correction
is not asserted to explain any earlier unrelated host-forwarded UDP timeout.

## Paired QEMU performance

`final-apk-benchmark/` is a separate completed 25.12.5 session with the same final
APKs, real daemon and real kernel ipsets. QEMU is pinned to host CPUs 4/5 and the
compressed upstream stub to CPU 6. The Go loader runs inside the guest over
loopback with `GOMAXPROCS=1`, competing for emulated CPU resources. Both variants
use 1000 enabled namespace rules, 32 names and 10 clients. Each case has three
pairs of ten-second samples after two seconds of warm-up, a fresh daemon per
sample and alternating variant order. Before every measurement the helper
checks exact DNS records, expected real ipset addresses and remaining TTL.
Known builds, tests, native loads and other emulators were stopped throughout.

The **18 samples contain 185,074 valid measured queries**, with zero errors,
timeouts or invalid replies. Warm-up requests are excluded. Raw rows are in
`final-apk-benchmark/results.jsonl`; the complete marginal and paired summary is
`emulator-performance-summary.json`. No unfavorable sample was replaced and no
additional timing experiment was selected after these results.

The following percentages compare medians of each variant's three samples.
Observed ranges are not confidence intervals.

| Workload | Baseline QPS median [range] | Candidate QPS median [range] | QPS change | Daemon CPU/query change |
|---|---:|---:|---:|---:|
| UDP, 1 A | 1216.24 [1140.46, 1357.51] | 1954.02 [1893.00, 2036.40] | +60.66% | −46.41% |
| UDP, 2 CNAME + 4 A | 902.59 [895.31, 995.45] | 1429.67 [1164.59, 1533.13] | +58.40% | −35.39% |
| TCP, 2 CNAME + 4 A | 335.55 [320.40, 339.45] | 331.19 [300.00, 349.71] | **−1.30%** | **+2.77%** |

| Workload | p95 median baseline → candidate, ms | p99 median baseline → candidate, ms |
|---|---:|---:|
| UDP, 1 A | 12.532 → 10.036 | 17.654 → 12.991 |
| UDP, 2 CNAME + 4 A | 14.603 → 11.857 | 19.276 → 16.167 |
| TCP, 2 CNAME + 4 A | 46.960 → 50.769 | 56.925 → 62.797 |

For both UDP shapes, QPS is higher, p95/p99 lower and daemon CPU/query lower in
all three pairs. QPS ranges do not overlap. Paired median QPS changes are
+55.64% for single A and +59.68% for two CNAMEs plus four A records. The UDP
single-A baseline p99 varies from 15.096 to 48.842 ms versus 12.792–13.773 ms
for the candidate; that slow baseline sample is retained and visible.

TCP's p99 paired changes are **+1.425, −0.029, +9.776 ms**: two of three worsen,
paired median +1.425 ms (+2.32%), while marginal medians differ by +10.32%.
Baseline p99 ranges 56.134–61.372 ms and candidate 56.105–66.701 ms. p95 changes
are +0.649, −1.137, +7.592 ms. CPU/query is also higher in two of three pairs.
QPS improves in two pairs but falls 10.59% in the last, producing the negative
marginal median change. These mixed short runs with overlapping ranges do not
establish a repeatable TCP capacity win or equivalence. The unfavorable tail
observations remain a limitation; they are not dismissed as proved noise.

Native TCP uses a different answer shape and reports its own mixed capacity
tails in `host-results.md`; its paced fixture is favorable in four of five
pairs. Neither series cancels the other. Likewise, the large emulated single-A
UDP improvement does not erase the final native single-A QPS −3.09% and p99
+7.29% marginal changes.

After-load snapshots show two timerfds on every baseline and final TCP sample.
The UDP candidate retains 20–21, consistent with its explicit reusable pool;
the cap is unit-tested. Observed daemon RSS is 2640–2768 KiB on the baseline
and 2648–2760 KiB on the candidate. These snapshots are not peak-RAM or
kernel-memory measurements. Daemon CPU/query excludes loader/upstream CPU and
the rest of QEMU.

QEMU TCG changes instruction and syscall costs, and the guest loader shares
emulated resources with the daemon. The results establish improvements for
these emulated UDP fixtures, with the TCP costs above. They do not establish
physical-router throughput, Internet forwarding bandwidth, host-forwarded UDP
performance or superiority to firc.

## Retained setup attempts and helper checks

The initial environment setup needed SDK 25's generated curl libtool auxiliaries
repaired using the SDK's own libtoolize and existing config.status; both
variants use the same resulting dependency staging. Commands are preserved in
`emulator-build/sdk-setup.json` and adjacent logs.

Later source-cache hash guards rejected a stale SDK source archive. A candidate
link then exposed stale objects despite verified source files. The final
builder now stages the exact fresh archive after clean and explicitly clears
only the verified unpacked backend build directory before compile. All four
final packages were rebuilt after those fixes. The host linker likewise uses
the production Makefile's exact object whitelist after a clean isolated build.
Failures are retained in `final-emulator-build/source-cache-guard/` and
`final-emulator-build/rejected-preclean/`; they are not counted as successful
validation or final timing evidence.

Earlier emulator setup attempts exposed unsupported BusyBox fractional sleep,
a stale ephemeral mirror port in reused NAND, and a mirror whitelist rejecting
`~` in an official dependency filename. Exact executed scripts and notes remain
in the experimental directories. The corrected helpers use integer waits,
refresh only known local-mirror URLs and accept the standard filename character.

Saved final helper regressions pass: 23 emulator-comparison tests, 12 package/
native-build preparation tests, six existing emulator tests, 24 existing SDK
tests, five rules-driver tests, and `go test -count=1 -v ./...` for the benchmark
helpers. The index is `validation/final-bench-helpers/summary.json`. Guards cover
wrong binary identity, malformed replies, permanent unmatched ipset entries,
mirror traversal, stale archives/objects, escaped symlinks, exact object lists,
process shutdown and smoke-reuse provenance.

The local dependency mirror is restricted to the official release trees; host
HTTPS and guest feed-signature verification remain enabled. Each run retains
package/build identities, firmware/QEMU/preset/pre-boot NAND hashes, config and
helper hashes, exact executed harness and shell fixtures, serial logs and raw
results. Unique sockets, an emulator lock and owned-process-group cleanup keep
control endpoints and shutdown confined to the launched fixture. All final
emulator and helper processes exited after the completed series.
