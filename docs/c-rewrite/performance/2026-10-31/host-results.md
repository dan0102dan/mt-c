# Final native DNS comparison, 2026-10-31

The final candidate improves throughput for the two multi-address UDP fixtures
in all five pairs. **There is no universal UDP throughput win:** the single-A fixture has
lower marginal median QPS and higher marginal median p99. TCP capacity is nearly
flat in the marginal medians with mixed paired tail changes. These are results
for the final restricted implementation, not the broader initial experiment.

The baseline is pristine `6a94817a3764dbf9280a9fe657c2afd943725cb4`. The measured
candidate production sources match `1763aff5a780343f1c1673586a046b30d99b9168`.
Only UDP opts into reusable deadline timerfds and forwarding an unchanged raw
response. TCP retains ordinary fresh timers and historical repacking behavior.
The cache allocation reduction, action coalescing and rule reconciliation work
remain shared. Initial experiments and their unfavorable TCP tails are retained
in `experimental-results.md` and `experimental-host-results.md`.

## Method and gates

The native executable links production transport, loop, parser, pipeline, cache
and matcher objects with a counting netfilter callback. It does not run the
full daemon's API/journal or write host kernel ipsets. Both binaries were built
from empty, isolated `build/perf-compare` directories using the same `-O2
-Werror` recipe. The linker consumes the production Makefile's exact `LIB_OBJS`
list, not every object found recursively. Build logs, source hashes and final
executable identities are in `final-emulator-build/final-host-*.metadata.json`.

Both variants use the same compressed upstream fixture, 1000 enabled namespace
rules, 32 query names and 10 clients. Proxy, upstream and loader are pinned to
CPUs 0, 1 and 2; the Go programs use `GOMAXPROCS=1`. Known builds, tests, emulators
and other benchmark jobs were stopped during timing. Each sample starts a
fresh proxy, warms for one second and alternates baseline/candidate order
between repetitions. Readiness probes use the measured scenario's protocol for
both the upstream and proxy on both variants.

Raw DNS validity, query totals, proxy response counts, nonzero matching activity
and zero proxy drops must pass before a sample is accepted. UDP uses five pairs
of eight-second samples for each of three shapes. TCP capacity and paced tests
each use five pairs of twelve-second samples. No sample was replaced or rerun
because its result was unfavorable.

The complete final series contains **50 measured samples and 18,513,722 valid
queries**, with zero errors, timeouts, invalid replies or reported proxy drops.
Warm-up requests are excluded from that query total. `host-summary.json`
contains marginal summaries, per-pair differences, callback ratios and references
to every raw run. The predeclared experiment is in `final-experiment-plan.json`.

## UDP: gains depend on the answer shape

Percentages below compare the medians of each variant's five samples. They are
not medians of paired percentage changes. Ranges are observed minima/maxima,
not confidence intervals.

| Workload | Baseline QPS median [range] | Candidate QPS median [range] | QPS change | Proxy CPU/query change |
|---|---:|---:|---:|---:|
| UDP, 1 A | 69452.70 [62649.86, 74543.75] | 67308.55 [62379.82, 82050.61] | **−3.09%** | −13.12% |
| UDP, 4 A | 68942.90 [62717.25, 71851.74] | 76882.10 [75738.78, 82985.69] | +11.52% | −16.83% |
| UDP, 2 CNAME + 4 A | 54360.08 [53109.81, 55616.90] | 60349.21 [54070.61, 66837.64] | +11.02% | −17.09% |

| Workload | p95 median baseline → candidate, ms | p99 median baseline → candidate, ms | p99 marginal change |
|---|---:|---:|---:|
| UDP, 1 A | 0.281 → 0.279 | 0.494 → 0.530 | **+7.29%** |
| UDP, 4 A | 0.263 → 0.249 | 0.426 → 0.398 | −6.57% |
| UDP, 2 CNAME + 4 A | 0.309 → 0.282 | 0.495 → 0.448 | −9.49% |

Single-A throughput has mixed paired directions: three of five candidate runs
are faster, with a paired median QPS change of +4.93%, despite the negative
ratio of marginal medians. Its candidate p99 ranges from 0.389 to 1.286 ms,
compared with baseline 0.419 to 0.560 ms. One retained pair has a +0.832 ms p99
increase; two of five are worse. The broad variation and conflicting summaries
do not support a throughput or tail improvement for this shape.

For four A records, QPS improves in all five pairs, with a paired median change
of +12.35%; the measured QPS ranges do not overlap. Both p95 and p99 improve in
all five pairs. For two CNAMEs plus four A records, QPS also improves in all five
pairs, with paired median +12.81%; the ranges overlap slightly. Its p95 and p99
improve in four pairs and worsen in one. Proxy CPU per query falls in 14 of the
15 UDP pairs: four of five single-A pairs and all ten multi-address pairs.

With two CNAMEs and four A records, counting-sink calls per response are about
11.9995 on the baseline and exactly 4 on the candidate across the complete
warm-up plus load session. With no CNAMEs, both variants emit exactly 1 or 4.
This verifies less duplicate logical action work in the fixture. It does not
measure netlink syscall latency or real routing throughput.

## TCP capacity: mixed paired tails, no capacity win claimed

| Metric | Baseline median [range] | Candidate median [range] | Marginal change |
|---|---:|---:|---:|
| QPS | 13985.23 [13886.48, 14052.63] | 14015.97 [13768.94, 14054.84] | +0.22% |
| p95, ms | 1.362 [1.274, 1.483] | 1.327 [1.308, 1.345] | −2.57% |
| p99, ms | 3.544 [2.743, 3.646] | 3.519 [3.337, 3.708] | −0.71% |
| CPU ticks/query | 0.005847 [0.005787, 0.005935] | 0.005876 [0.005787, 0.006136] | +0.49% |

Candidate-minus-baseline p99 changes in repetition order are **−0.129, +0.838,
−0.168, −0.025, +0.133 ms**. Two of five are worse; the paired median difference
is −0.025 ms (paired median percentage −0.71%). The observed ranges overlap,
and the +0.838 ms pair is retained. QPS paired median change is +0.016%; CPU/query
paired median change is +0.72%, with three of five candidate values higher.
This series does not establish a TCP capacity gain or statistical equivalence.
It also does not reproduce the uniformly adverse capacity-p99 direction that
motivated narrowing the initial implementation.

The initial all-transport optimization had capacity p99 worse in all five pairs
(+8.44% marginal median). Its two diagnostic arms did not demonstrate that the
response format alone explained the cost. The final implementation therefore
opts TCP out of the new timer and raw-forwarding paths. Those experimental
samples remain available and are not mixed into this final sample set. The
final series also uses protocol-matched readiness, so comparing absolute
latencies between historical and final sessions would confound the code change
with fixture warm-up and time-of-run effects.

## TCP at the predeclared 7000-QPS offered rate

The declared rule selects 7000 QPS if it is at most 80% of the slowest final
baseline capacity sample; otherwise it selects integer half of the baseline
median. The final baseline minimum was 13886.48 QPS, so 7000 was selected. The
inputs and rule are saved in `final-tcp-paced-selection.json`.

| Metric | Baseline median [range] | Candidate median [range] | Marginal change |
|---|---:|---:|---:|
| Completed QPS | 6999.95 [6999.34, 6999.97] | 6999.87 [6999.41, 6999.92] | −0.001% |
| p95, ms | 0.777 [0.718, 0.813] | 0.712 [0.685, 0.838] | −8.37% |
| p99, ms | 1.273 [1.136, 1.487] | 1.052 [0.984, 1.393] | −17.36% |
| CPU ticks/query | 0.005750 [0.005262, 0.005940] | 0.005321 [0.005190, 0.006119] | −7.45% |

Paced p99 differences are **−0.286, −0.152, +0.120, −0.147, −0.334 ms**: four of
five improve, paired median −0.152 ms (−13.38%). p95 likewise improves in four
pairs. This is a favorable observed tail direction for this offered-rate
fixture, with overlapping ranges and only five pairs; it is not a general TCP
latency guarantee. Despite the lower marginal CPU median, CPU/query is higher
in three of five pairs and has paired median change +0.62%, so no repeatable
paced CPU saving is claimed.

The paced loader measures request latency after waiting for its schedule. It
does not include schedule lateness and is not a fully open-loop arrival model.
Fixed completed QPS is an offered-rate control, not a capacity measurement.

## Resource observations and scope

After-load snapshots show zero timerfds on both TCP variants and on the UDP
baseline. The UDP candidate retains 18–19 reusable timerfds, within its tested
cap of 32. Across final samples, process RSS is 7464–7656 KiB on the baseline
and 7524–7700 KiB on the candidate. These are snapshots, not peak memory or
kernel-memory measurements. CPU/query is derived from process user+system
CPU ticks (`CLK_TCK=100` here); it excludes upstream and loader CPU.

The results are short paired experiments on one shared host with known local
workloads stopped. They are descriptive, not confidence bounds. The separate
OpenWrt report validates actual packages, daemon behavior and real kernel
routing/ipsets. Neither report measures a physical router, Internet forwarding
bandwidth or a direct comparison against firc.

## Reproduction and raw evidence

- `host-summary.json`: combined final marginal and paired values, exact callback
  ratios, snapshots and raw/metadata references.
- `final-host-udp/`, `final-host-tcp/`, `final-host-tcp-paced/`: every raw row,
  per-run logs, metadata, generated config and exact executed harness.
- `final-emulator-build/final-host-{baseline,candidate}.metadata.json`: fresh
  build/source/object/executable provenance; adjacent build logs show commands.
- `final-experiment-plan.json` and `final-tcp-paced-selection.json`: declared
  scenarios and offered-rate selection.
- `experimental-results.md`: retained initial/diagnostic experiments and why
  the final transport optimization was narrowed.
- `tools/bench/dns-comparison.md`: reusable commands and measurement limits.
