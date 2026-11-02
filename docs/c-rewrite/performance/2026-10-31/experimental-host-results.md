# Native DNS comparison, 2026-10-31

These are fresh measurements against pristine `6a94817a`, not the earlier
2026-10-29 experiment. Full daemon/package/kernel checks are reported separately.
The native executable links production transport, loop, parser, pipeline, cache
and matcher objects, with a counting netfilter callback. There is no host kernel
write, API/journal traffic or physical-router measurement in these numbers.

## Method and gates

Both variants use the same `-O2 -Werror` build recipe, compressed upstream fixture,
1000 enabled namespace rules, 32 names and 10 clients. Proxy, upstream and loader
are pinned to CPUs 0, 1 and 2; Go programs use `GOMAXPROCS=1`. Builds and other
known test/benchmark jobs were stopped during the measurements. Each sample
starts a fresh proxy, warms for one second and alternates baseline/candidate
order between repetitions. Raw DNS replies, query totals, proxy response counts,
nonzero matching activity and zero proxy drops must pass before saving a row.

UDP uses five pairs of eight-second samples per case. TCP capacity and paced
runs each use five pairs of twelve-second samples. The fixed-rate rule was
predeclared: use 7000 QPS if that is no more than 80% of the slowest baseline
capacity sample; otherwise use half the baseline median. The baseline minimum
was 13434.75 QPS, selecting 7000. The paced loader measures request latency after
its schedule wait; it does not include schedule lateness and is not a fully
open-loop arrival model.

## Main observations

The table compares medians across the five runs of each variant. Percentages
are ratios of those medians, not medians of paired percentage changes.

| Workload | Baseline QPS | Candidate QPS | QPS change | Proxy CPU/query change |
|---|---:|---:|---:|---:|
| UDP, 1 A | 73434.90 | 79476.19 | +8.23% | -14.43% |
| UDP, 4 A | 67387.36 | 72322.38 | +7.32% | -16.46% |
| UDP, 2 CNAME + 4 A | 53628.89 | 56664.12 | +5.66% | -11.42% |
| TCP capacity, 1 A | 13619.98 | 13927.78 | +2.26% | -4.04% |
| TCP paced, 1 A | 6999.92 | 6999.92 | unchanged offered rate | -5.22% |

All main experiments together contain **50 valid samples and 18,487,046 valid
measured queries**, with zero reported errors, timeouts, invalid replies or
proxy drops. Warm-up requests are excluded from this query total. Proxy CPU per
query is lower in every paired main sample. UDP QPS is higher in 14 of 15 pairs;
some observed QPS ranges overlap. These are short samples on one host, so the
numbers are descriptive, not confidence intervals or universal speedup bounds.

With two CNAMEs and four A records, the observed counting-sink callback ratio is
about 12 per response on the warmed baseline and exactly 4 on the candidate.
For the no-CNAME cases it remains 1 or 4 on both. This proves the logical action
reduction in this fixture; it does not measure netlink syscall latency.

## TCP tail remains a qualification

| Capacity TCP metric | Baseline median [min, max], ms | Candidate median [min, max], ms |
|---|---:|---:|
| p95 | 1.397 [1.371, 1.414] | 1.403 [1.370, 1.419] |
| p99 | 3.221 [2.706, 3.257] | 3.493 [3.432, 3.574] |

Capacity p95 is nearly flat with mixed pair directions. Capacity p99 increased
in all five pairs: **+0.516, +0.795, +0.261, +0.317, +0.246 ms**. Its marginal
median increased 8.44%, and the observed ranges do not overlap. This must not be
dismissed as a general TCP latency win or as proven noise.

At 7000 QPS the marginal p99 medians are 2.538 and 2.291 ms, but the paired
candidate-minus-baseline differences are **+0.353, +0.719, -0.023, +0.078,
-0.609 ms**. Three of five are worse and the paired median is +0.078 ms.
The broad overlapping ranges and mixed direction establish neither a repeatable
fixed-rate tail improvement nor a consistent fixed-rate tail regression.

A separate five-pair capacity diagnostic links the candidate's unchanged
objects with only the pristine baseline `loop.o`, comparing that hybrid with
the full candidate. It contains 1,676,394 additional valid queries and zero
errors. Full-pool p99 is worse in four of five pairs (paired median +0.151 ms),
with overlapping ranges; p95 is mixed, QPS medians differ by +0.16%, and CPU/query
marginal medians by -1.44%. This suggests a possible adverse pool contribution,
but does not establish it as the sole cause of the full baseline/candidate
capacity-tail change. The diagnostic is not a pristine baseline comparison.

A further diagnostic compares pristine baseline against the candidate with
only the original `dns/proxy.o` restored, preserving the new pool, cache and
pipeline. Five pairs of twelve-second capacity samples produced 1,640,938 valid
queries and zero errors. QPS marginal medians are 13872.23→13931.66 (+0.43%),
p95 is 1.342→1.439 ms (+7.23%), and p99 is 3.556→3.773 ms (+6.10%). Both p95 and p99
are worse in four of five pairs; p99 differences are −0.233,+0.240,+0.908,+0.320,
+0.044 ms (paired median +0.240 ms), with overlapping ranges. Restoring the old
proxy did not resolve the adverse tail direction. It restores both the old
uncompressed packing and historical question processing, so it is not a
wire-format-only toggle. Raw data and per-pair values are in
`host-tcp-proxy-isolation/`; exact object/source hashes and link command are in
`emulator-build/host-old-proxy*`. This result motivates narrowing the final
transport/timer optimizations to UDP; it is evidence about the initial 4fefbaf
candidate, not validation of a later restricted implementation.

The compressed response is another real difference: the original packer emits
an uncompressed single-A message (84 DNS bytes for this query), while the
candidate can forward the original compressed message (59 DNS bytes). The
no-pool diagnostic keeps response format equal. The measurements do not
identify kernel scheduling, Go allocation/GC or wire parsing as the cause.

## Reproduction and raw evidence

- `host-summary.json` contains the combined marginal and paired summaries,
  exact callback ratios and raw/metadata references.
- `host-udp/`, `host-tcp/`, `host-tcp-paced/` each contain `results.jsonl`,
  `summary.json`, `metadata.json`, exact executed harness and per-run logs.
- `host-tcp-timer-isolation/` contains the separately labelled diagnostic.
- `tcp-paced-selection.json` records the rate-selection rule and baseline inputs.
- `emulator-build/host-*.metadata.json` records production source and executable
  hashes, build/link argv and the diagnostic loop object.
- `tools/bench/dns-comparison.md` documents commands and scope.

No physical router, direct comparison against firc or Internet packet-forwarding
bandwidth was measured by this native experiment.
