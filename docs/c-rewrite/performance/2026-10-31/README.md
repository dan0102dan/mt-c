# DNS and rules reconciliation optimization

This change removes repeated work in mt-c's UDP DNS path and rules
reconciliation. The comparison baseline is
`6a94817a3764dbf9280a9fe657c2afd943725cb4`. The final production implementation
is in `1763aff5a780343f1c1673586a046b30d99b9168`; later commits add the
measurement tooling, tests of that tooling, and this evidence. Per-file hashes
identify the actual sources and executables used by each experiment.

## Implementation and behavior

| Change | Work removed | Bounds and preserved behavior |
|---|---|---|
| UDP response forwarding | Packing an already validated reply when AAAA filtering changed nothing | Parsing and the cache/routing callback still run. TCP retains its previous packing path. Header-only normalization remains. |
| UDP deadline reuse | Recreating a timerfd, watch allocation, and epoll registration for every completed request | Explicit opt-in API; at most 32 idle descriptors. Each active deadline still has its own monotonic kernel timer. TCP and all existing timer callers use fresh timers. |
| Response-local routing actions | Repeated successful writes of the same group/family/address during a CNAME response | At most 32 original actions and 4096 bytes of owned names per batch; lossless chunked flush. Last TTL wins, including a lower or zero TTL. No suppression across responses or reloads. |
| Alias traversal | A second queue allocation and second copy of every alias name | The owned BFS result also serves as the queue; ordering, cycles, lifetime, and allocation-failure cleanup remain covered. |
| Rules reconciliation | Scanning all cached names separately for every ordinary rule | Reuse the existing matcher in blocks of at most 4096 rules. Empty caches skip matcher construction. Static subnet ordering and leading-dot namespace semantics are retained. |

The routing action sink retries the final desired write after an error, up to
the number of original writes represented by that action, and stops on success.
This preserves later opportunities after a transient failure. Successful final
routing state is compared against the previous per-record stream. Arbitrary
kernel failures do not produce an identical sequence of intermediate TTLs and
partial states: retries use the final TTL. Duplicate successful log entries
are collapsed as well. DNS forwarding keeps its existing best-effort behavior
when a routing write fails.

UDP forwarding now preserves upstream compression and parser-accepted trailing
bytes when no Answer AAAA was removed. This is an intentional wire
representation change, not removal of validation. Authority/Additional records
and response-hook ordering remain unchanged. PTR eligibility reads the parsed
question instead of traversing its name again; the fake-PTR response builder
is unchanged.

The timer reserve counts both idle watches and reusable watches retired in the
current epoll batch. A cancelled watch cannot be reused until that batch ends,
so an old ready event cannot invoke a new timer's callback. Disarm/rearm failures
close the affected descriptor; idle descriptors close on loop return or
destruction. Ordinary timers neither consume nor populate the reserve.

The 4096-rule bound is a rule-count limit, not a constant byte budget. Pattern
lengths and kinds still determine matcher memory, and wildcard/regex evaluation
can remain linear within a block. Reconciliation retains maximum remaining TTL
and permanent-entry priority; this is deliberately distinct from the DNS
response's last-write rule. Allocation failure while building desired lists
returns before any partial lists are applied to the kernel.

These intentional details are recorded in [D-86 through D-89](../../decisions.md).
No production dependency, configuration field, API route, netfilter naming
contract, or supported build target was removed or added by the optimization.

## Measurements and scope

The final OpenWrt guest shows a substantial UDP gain. TCP does not have a
general performance win, and the unfavorable guest TCP tail is retained rather
than averaged into the UDP result. These are ratios of the variants' sample
medians; the detailed reports also give every paired change and observed range.

| OpenWrt 25.12.5 / QEMU workload | Baseline QPS | Candidate QPS | QPS change | Daemon CPU/query change | p99, ms, baseline → candidate |
|---|---:|---:|---:|---:|---:|
| UDP, 1 A | 1216.24 | 1954.02 | +60.66% | −46.41% | 17.654 → 12.991 |
| UDP, 2 CNAME + 4 A | 902.59 | 1429.67 | +58.40% | −35.39% | 19.276 → 16.167 |
| TCP, 2 CNAME + 4 A | 335.55 | 331.19 | **−1.30%** | **+2.77%** | **56.925 → 62.797** |

Both UDP workloads improve throughput, p95/p99, and daemon CPU/query in all
three paired runs; their observed QPS ranges do not overlap. Guest TCP has
higher p99 in two of three pairs, with changes of +1.425, −0.029, and +9.776 ms.
Its marginal p99 increase is **10.32%**; the paired median increase is 1.425 ms
(2.32%). Ranges overlap and directions are mixed. This does not establish a
TCP improvement or equivalence, and the unfavorable observations remain a
performance limitation of this submission.

The native counting-sink fixture has a different scope and smaller gains:

| Native workload | Baseline QPS | Candidate QPS | QPS change | Proxy CPU/query change |
|---|---:|---:|---:|---:|
| UDP, 1 A | 69452.70 | 67308.55 | **−3.09%** | −13.12% |
| UDP, 4 A | 68942.90 | 76882.10 | +11.52% | −16.83% |
| UDP, 2 CNAME + 4 A | 54360.08 | 60349.21 | +11.02% | −17.09% |
| TCP, 1 A, capacity | 13985.23 | 14015.97 | +0.22% | +0.49% |

Native multi-address UDP throughput improves in all five pairs. Single-A
directions are mixed and include a retained candidate p99 of 1.286 ms; no
throughput/tail improvement is claimed for that shape. Native TCP capacity has
mixed paired tails and no established capacity gain. At the separately selected
7000-QPS offered rate, TCP p99 improves in four of five pairs; its marginal
medians are 1.273 → 1.052 ms. Paired CPU changes do not support a repeatable CPU
gain at that rate. CPU figures above concern the proxy/daemon process only,
excluding the loader and upstream.

The final DNS series comprises **68 samples and 18,698,796 valid queries**,
excluding warm-up, with zero measured errors, timeouts, or invalid replies.
This combines 50 native samples with 18 guest samples; the environments are
reported separately and their QPS values are not pooled.

Final native measurements are in [host-results.md](host-results.md). The
fixture links the production transport, event loop, parser, cache, pipeline,
and matcher, with a counting sink in place of kernel writes. It measures that
path rather than the complete daemon. UDP, TCP capacity, and TCP at a fixed
offered load are separate experiments.

[emulator-results.md](emulator-results.md) records actual APK/IPK installation,
the packaged daemon, real guest-kernel ipsets, LAN routing checks, and the final
OpenWrt 25.12.5 timing comparison. Guest-loopback measurements include competition
with the guest load generator and QEMU's CPU/kernel costs.

The [rules reconciliation comparison](rules-sync/README.md) measures desired-list
construction, result validation, and freeing. It excludes input setup from CPU
time and includes setup in process peak RSS. Both variants use an identical
harness and shared matcher/cache; only `ruleset.c` changes. These values are not
complete reload times or DNS throughput.

| Namespace rules × cached names | Baseline CPU per reconciliation | Candidate CPU | Ratio of CPU medians |
|---|---:|---:|---:|
| 1000 × 1000 | 7.439 ms | 0.471 ms | 15.80× |
| 10000 × 1000 | 49.802 ms | 2.425 ms | 20.54× |
| 10000 × 10000 | 775.745 ms | 6.876 ms | 112.81× |
| 100000 × 10000 | 4644.948 ms | 35.633 ms | 130.35× |
| 100000 × 0 | 4.576 ms | 0.935 ms | 4.89× |

These are 50 measured runs plus separately retained calibration. At
100000 × 10000, whole-process peak RSS medians are 18368 → 19400 KiB, an
increase of 1032 KiB. Other cases hit the launcher/runtime high-water floor;
equal RSS there does not prove zero additional matcher allocation. The two
largest nonempty cases retain short candidate CPU intervals under the declared
baseline-budget/one-iteration policy, with no selectively extended runs.

The [experiment plan](final-experiment-plan.json) fixes workloads, repetition
counts, CPU affinity, readiness protocol, and the TCP fixed-rate selection rule
before final measurements. Order alternates between baseline and candidate.
Timings run after known builds, tests, and other benchmark processes stop.
Each measured DNS run requires valid replies and zero errors/timeouts; guest
runs also verify actual kernel-set effects before measurement. Reported
median/min/max ranges describe these samples and are not confidence intervals.

### Rejected broader variant

The initial variant reused timers and forwarded unchanged wire bytes for both
UDP and TCP. Its native TCP capacity test had a higher p99 in all five pairs
(median p99 3.221 → 3.493 ms). Restoring just the original timer or proxy object
did not establish a neutral tail. The final implementation therefore restricts
both optimizations to UDP and repeats the complete measurement plan.

All initial and diagnostic samples remain in the evidence. They are labelled
experimental and must not be quoted as results for the final code. See
[experimental-results.md](experimental-results.md) for their locations and the
readiness-probe difference between experiments. The diagnostic results do not
prove one individual change caused the entire TCP effect.

## Regression coverage

The final local matrix passed all **496 test-binary executions**:

| Variant | Normal | ASan + UBSan + leak checking |
|---|---:|---:|
| Generic | 81/81 | 81/81 |
| Keenetic | 86/86 | 86/86 |
| Native OpenWrt, including real ubusd | 81/81 | 81/81 |

Each variant has two explicitly skipped privileged route cases, as described
below. [validation-summary.json](validation-summary.json) links the complete
per-variant logs, commands, source manifests, and executable hashes. Final
focused DNS parse/PTR fuzzing adds 400,000 inputs under ASan/UBSan/LSan without
a crash or timeout; the transport/API selection cases are covered by their
separate regression tests, not by these wire-helper fuzz targets.

The [C backend CI run for the final production commit](https://github.com/dan0102dan/mt-c/actions/runs/41224220405)
also passed: generic/Keenetic build, unit tests, sanitizers, static analysis,
fuzz smoke, native OpenWrt/ubus, complete golden/HTTP contracts, real-kernel
route failover, and the dependency-free mipsel cross-build. The separate
[netfilter recovery regression](https://github.com/dan0102dan/mt-c/actions/runs/41224220399)
passed as well. [ci-source-validation.json](ci-source-validation.json) retains
that exact source-commit status. Current checks for the eventual evidence/tooling
commit are available on [PR #31](https://github.com/dan0102dan/mt-c/pull/31).

The new production regression tests exercise the actual proxy handlers and
timer APIs, the real daemon routing-action sink, and a frozen reference rules
builder. They cover:

- Cold and warm CNAME chains, multiple groups, IPv4/IPv6, lower/zero TTL,
  reloads, ignored/malformed records, and action/name-buffer boundaries.
- Owned alias output after cache destruction and every allocation failure in
  an expanding traversal.
- Seeded mixed rules, both 4096-rule boundaries, static subnet ordering,
  leading-dot namespaces, expiry, and failures while constructing a later block.
- Cancelled ready events, mixed ordinary/reusable timers, callback userdata
  lifetime, zero-delay and periodic timers, ID wrap, syscall failures, and
  complete descriptor cleanup.
- TCP read/processing deadlines and failure cleanup, UDP timer selection,
  filtering versus forwarding bytes, PTR handling, and packing failures.

OpenWrt correctness checks independently hash the package's extracted executable,
the installed file, and `/proc/PID/exe`. Both release lines run detailed DNS/kernel
checks. Final candidate LAN/DHCP smoke is rerun; reuse of a prior baseline smoke
is allowed only after validating its executable, firmware, QEMU, preset, and
fixture identity, and is recorded explicitly. The suite checks all six routing
rule types, IPv4/IPv6 and unmatched controls, overlapping groups, a shorter TTL,
AAAA-filter reload with unchanged PID/binary, drop/recovery, and a proxy-side
TCP timeout distinct from the client's own timeout.

Both final OpenWrt validations passed. APK reran full baseline and candidate
LAN/DHCP smoke; IPK reran candidate smoke and reused the separately verified
baseline smoke. Both versions freshly installed and checked both executables
and ran all detailed DNS/kernel gates. Final ARM64 timer executables also
passed inside OpenWrt 25.12.5: **21 cases, 4943 assertions**, with matching
served, guest, and local binary checksums. Seventy Python helper tests and the
Go fixture suites passed; the new helper regression suites are added to CI.

The existing LAN fixture's client address changes from `.250` to `.252` because
router-emulator v3.7 reserves `.248`–`.250`. This corrects a fixture collision;
it does not establish anything about unrelated host-forwarded UDP behavior.

## Reproduction and limitations

See [the DNS comparison guide](../../../../tools/bench/dns-comparison.md) for
host/package/guest build and run commands. Executed commands, source and binary
hashes, raw measurements, test logs, and failed setup attempts are retained.
There is no physical-router measurement or direct firc head-to-head comparison
in this report. DNS query rates are not Internet packet-forwarding bandwidth.

The local managed host cannot create a network namespace. Two privileged route
cases per local C test variant therefore skip; GitHub CI runs the real-kernel
route gate, and the OpenWrt guest provides the additional kernel/LAN checks.
The local HTTP golden gate has the same namespace restriction; its complete
GitHub CI run is tracked separately from the local pre-HTTP comparisons.

## Evidence layout

The readable reports, final summaries, and raw final measurement rows are kept
beside this README. Complete build/test/fuzz/emulator logs, executed helper
snapshots, source provenance, previous experiments, and failed setup attempts
are stored in `evidence.tar.xz`, with paths relative to this directory. Extract
it here to resolve every detailed path mentioned by a report:

```sh
tar -xJf evidence.tar.xz
```

The evidence manifest records the archive checksum and checksums of its files.
The archive contains generated fixtures and logs, not SDKs or executable
packages. The measured package/executable hashes and reproduction commands
are retained.
