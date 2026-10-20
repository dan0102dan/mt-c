# DNS journal review: performance and failure checks

Measured on 2026-10-20. These are host experiments, not measurements on a Keenetic/OpenWrt router. The implementation measured is commit `1bb5eaa730228d26a97f54c353a8a3b21206ab04`.

## Compared versions and environment

- `base`: `05c9ff8c6e3b0df7435f250cba36dc5e5d73e818`, before DNS history.
- `reviewed`: `7436bc90d4c29efcdead2477a3e211b81f56c422`, the originally reviewed arena implementation; used in the microbenchmark.
- `before`: `63bbbf1784b5111831757c005325dacb9e6425df`, including opt-in capture, the 1 MiB budget and the combined-question timeline; this is the direct predecessor for behavioral comparisons.
- `work`: the fixes in `1bb5eaa`, preserving those opt-in/budget/UI changes.

Linux x86-64 container, AMD EPYC 9V74 virtual CPU, cgroup CPU quota equivalent to four cores. Native GCC `-O2` builds. DNS load generator, a local UDP echo upstream and SSE readers share the host. Netfilter is disabled and routing groups are empty; these tests do **not** measure a complete routing workload, real WAN/upstream latency, TCP throughput, browser FPS or target-router performance. Some initial repetitions overlapped frontend build work; tail-latency differences should not be treated as precise isolated-machine estimates.

## Ring hot-path microbenchmark

`tools/bench/dns_query_cost.c` calls the actual binary-name append function against a prefilled ring. Five repetitions per scenario; median of each repetition's average time. No SSE, DNS parsing or HTTP serialization is included. Short names are 33 wire bytes, maximum names 255 bytes. Repeated-name scenarios use 200 distinct names.

| Capacity | Names/workload | Before, ns/append | Fixed, ns/append |
| --- | --- | ---: | ---: |
| 1000 | Short, repeated | 64.6 | 53.7 |
| 1000 | Short, constantly unique | 83.0 | 71.4 |
| 1000 | Maximum length, repeated | 347.8 | 349.6 |
| 1000 | Maximum length, constantly unique | 9654.4 | 415.7 |
| 10000 | Maximum length, constantly unique | 96782.5 | 409.8 |

The pathological case is approximately 23x faster at 1000 rows and 236x at 10000. More importantly, it no longer scales with the number of retained names: the moving arena is gone. Repeated long names remain essentially unchanged; this is not a claim that every workload becomes faster.

The CI regression is an operation-count test, not a noisy timing threshold. `test_dns_query_alloc` wraps `memmove` and `realloc`, churns prefilled 1000/10000-row rings with maximum-length names and requires **zero calls to either**. Distinct names use exact-sized allocations; repeated names allocate nothing. Inline binary IPs remove the second dictionary. Allocator internals/fragmentation are not included in requested-byte accounting.

## Full daemon UDP + SSE A/B

`tools/bench/dns_journal_load.py` launches the actual daemon, uses high loopback ports, warms up for two seconds at 5000 queries/s, then measures 9000 requests at a target of 3000 queries/s. Three repetitions per case, alternating candidate order. Cases include capture off, capture on with 0/1/4/8 active readers, and one active plus one non-consuming reader. Both short repeated and constantly unique maximum-length names are tested.

All **78 runs / 702000 measured requests** completed with zero DNS response loss. Values below are medians across three runs; CPU is daemon CPU as a percentage of one core, p99 is end-to-end loopback DNS latency. Reader/generator CPU is excluded from daemon CPU but can affect scheduling latency.

| Workload | Before CPU | Fixed CPU | Before p99, ms | Fixed p99, ms |
| --- | ---: | ---: | ---: | ---: |
| Capture off, short names | 4.34% | 3.67% | 1.370 | 0.685 |
| Capture on, short names, no readers | 4.01% | 4.34% | 1.573 | 0.868 |
| Capture on, short names, 1 reader | 6.34% | 5.68% | 1.652 | 0.826 |
| Capture on, short names, 4 readers | 13.35% | 10.35% | 1.717 | 2.215 |
| Capture on, short names, 8 readers | 23.36% | 18.36% | 20.619 | 12.656 |
| Capture on, maximum unique names, no readers | 26.04% | 3.67% | 3.098 | 0.650 |
| Capture on, maximum unique names, 1 reader | 30.38% | 5.68% | 3.457 | 1.444 |
| Capture on, maximum unique names, 4 readers | 37.05% | 12.02% | 6.532 | 2.893 |
| Capture on, maximum unique names, 8 readers | 47.40% | 18.69% | 6.162 | 3.602 |
| Maximum unique names, 1 reader + 1 stalled | 30.38% | 7.34% | 2.266 | 1.349 |

The no-journal base measured 3.34% CPU / 0.643 ms p99 for short names and 4.67% / 2.402 ms for maximum names. These values illustrate host variability, not an expectation that instrumentation is faster than no instrumentation. Short-name/4-reader p99 did not improve in this experiment even though CPU did. Streaming still has serialization/network/reader costs, and eight readers are intentionally a stress case. The evidence establishes removal of the identified compaction and timer churn regressions; it does not establish zero monitoring overhead or universal p99 improvement.

Reproduction (isolated machine only; the daemon uses its normal Unix socket path):

```sh
python3 tools/bench/dns_journal_load.py \
  --binary base=/path/to/base/magitrickled-c \
  --binary before=/path/to/pre-fix/magitrickled-c \
  --binary work=/path/to/fixed/magitrickled-c \
  --rate 3000 --duration 3 --repeats 3 --out dns-load.jsonl
```

For ring benchmarking, compile `tools/bench/dns_query_cost.c` with the corresponding version's `src/dns/query_log.c` and `src/dns/wire.c`, using that version's headers and `-std=c11 -O2 -D_POSIX_C_SOURCE=200809L`. The source documents its positional arguments. Store each repetition, not just the best result.

## Resource and failure checks

A separate actual-daemon test linked only `mt_dns_query_log_create` to a failure stub. The daemon stayed alive, forwarded **1000/1000 DNS requests**, and all five journal read/control routes returned 503 with `DNS_LOG_UNAVAILABLE` / `X-DNS-Log-Available: 0`. No core DNS shutdown occurred. Unit tests additionally inject allocation failure during enable/append and require retained rows/IDs and consent state to remain intact; dropped-record counts are explicit.

Five connect/load/disconnect cycles, each with seven consuming SSE clients plus one stalled client, forwarded another **20000/20000 requests**. After each idle drain, descriptors returned to the same baseline of **66**. Journal requested bytes stayed at **1048235** with 1925 maximum-length unique rows, below the 1048576-byte budget. RSS rose from 7432 KiB before the first streaming cycle to 7972 KiB and stabilized in the final cycles; this short test is not a proof of absence of every possible leak. Stopping capture returned journal allocation to its 256-byte control object and zero rows.

Other deterministic regressions cover:

- Actual signed JWT plus temporary shadow-file password replacement: the established stream closes on revalidation and the old token cannot reconnect; auth becoming enabled is also covered.
- 128 live events without creating timerfds per event and without redundant wakeups per notification.
- A maximum of three eviction-driven snapshot restarts and an absolute 30-second transfer deadline, independent of partial-write progress.
- Bounded 128-row finite GET pages, valid cursor boundaries and invalid-query rejection.
- Client metadata updates without replaying DNS history.
- Frontend bounded ring ingestion of 100000 events, immutable published views, retention and atomic label updates; rendering is published at most once per animation frame.

Full local C suites passed for generic, Keenetic and OpenWrt; the generic suite also passed ASan/UBSan. Frontend type checking and DNS unit tests passed locally. Full browser suites and static analysis are checked by GitHub Actions, not inferred from these benchmarks. The existing MIPSle job is a core/tool cross-build, **not** a full target daemon build.

## Remaining verification boundary

The known algorithmic degradation is removed and protected by deterministic tests. Before making a router-wide performance guarantee, repeat A/B on the actual target CPU/libc with production routing rules, authentication, realistic clients and prolonged churn. Neither these measurements nor a green CI run justify saying that DNS history/SSE has no cost.
