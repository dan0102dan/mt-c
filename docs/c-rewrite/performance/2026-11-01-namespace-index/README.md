# Compact namespace index: implementation and measured scope

Baseline: `d4dd0f05a968f8e9c9e7186be083d56a26648521` (PR #31).
Final production `match.c` Git blob: `71bb109ea935dee598b0d6697970935e340172a2`.

## Change and routing invariants

Only the private namespace index in `src/backend-c/src/rules/match.c` changes.
It keeps the reversed-label trie, but stores nodes in a packed array and label
bytes in one owned buffer. One hash table stores edge IDs, not duplicate label
strings. Up to eight children are searched directly through compact sibling
links; larger fanouts use the hash table, with a maximum 50% load.

Hashes select buckets, never establish equality: parent ID, label length and
all bytes must agree. All growth keeps stable integer IDs/offsets; there are no
borrowed pointers into a mutable config and no query-path allocations. Storage
limits and allocation failures return an error, never truncate/evict rules or
pretend that a partially inserted rule succeeded. Earlier accepted rules
survive a failed insertion. Cleanup does not recurse through the trie.

Case sensitivity, dot boundaries, empty rules, leading/trailing/repeated dots,
and the existing aggregate-matcher quirks are retained. Exact-domain matching,
legacy/imported wildcards, PCRE2, group ordering/priorities, config/API formats,
CNAME/TTL handling and kernel ipset logic are unchanged. No dependency added.
A frozen copy of the old namespace implementation is test-only.

## Local validation of this exact production blob

- GCC: repository warning set with `-Werror`; 9 focused test groups and
  **948,689 checks passed**. Clang ASan/UBSan with leak detection repeats the
  same groups/checks successfully. Clang's production-source analyzer reported
  no diagnostics.
- Exhaustive 364 patterns × 1,093 query strings over `.aA`; randomized sets
  with case, empty labels, punctuation and non-ASCII bytes; both insertion
  orders; every rule in a 20,000-rule growth test; 100,000-rule storage test.
- A real 32-bit hash collision, parent-identity negatives, a 70,000-byte label
  and a 35,000-label deep name, duplicate insertion, and checked size arithmetic.
- Every successful realloc in the fault test is forced to move. All **39**
  allocation sites in the build/growth sequence are failed in turn: old matches
  survive, failed rules do not start matching, retry succeeds, and all owned
  allocations are released.

The managed local environment cannot clone/build the full repository or boot
QEMU. The focused source/header subset was fetched via the GitHub connector;
the production baseline and all fetched dependency files were verified by Git
blob hash. Do not describe the focused runs as full-daemon or kernel routing
validation. The repository's existing full C/sanitizer/golden, packaging and
real OpenWrt smoke workflows are the separate integration gates for this PR.

## Frozen final measurement series

This is a **matcher-only** fixture, not the earlier whole-daemon RSS chart and
not a physical-router benchmark. Both variants compile the actual production
matcher with identical flags and unchanged helper sources. Input strings are
not retained by the fixture. Every inserted rule and a corresponding negative
are checked before timing. Each timed query set is half hits/half misses.

Linux x86_64, GCC `-O2`, same allowed CPU, five alternating baseline/candidate
pairs per case; 100/1,000/10,000/20,000/100,000 rules, three name shapes, two
million timed lookups per sample. **150 samples / 300 million timed lookups**;
all result gates passed. Shared = a common suffix; diverse = independent
second-level domains; deep = long single-child paths around a branching level.

Payload is the requested storage of the matcher (including reserved capacity),
not allocator overhead. CSV also preserves current RSS before/after loading,
after matching and after freeing, plus VmHWM and build/free CPU. RSS includes
the entire harness process/libraries; do not multiply its percentage saving
into production daemon RAM. The new index still scales with rules/label bytes.

| Shape | Rules | Payload bytes before → after | Reduction | CPU ns/query before → after |
|---|---:|---:|---:|---:|
| shared | 100 | 17410 → 5232 | 69.9% | 49.28 → 42.22 |
| shared | 1000 | 153022 → 41072 | 73.2% | 57.98 → 49.89 |
| shared | 10000 | 1987942 → 655472 | 67.0% | 53.71 → 50.37 |
| shared | 20000 | 3997590 → 1310832 | 67.2% | 59.72 → 51.14 |
| shared | 100000 | 17715478 → 5242992 | 70.4% | 62.87 → 60.52 |
| diverse | 100 | 27285 → 10352 | 62.1% | 46.86 → 35.70 |
| diverse | 1000 | 309247 → 82032 | 73.5% | 53.02 → 43.78 |
| diverse | 10000 | 3324175 → 1310832 | 60.6% | 49.58 → 45.56 |
| diverse | 20000 | 6676959 → 2621552 | 60.7% | 51.77 → 45.94 |
| diverse | 100000 | 31213663 → 10485872 | 66.4% | 52.56 → 47.76 |
| deep | 100 | 95310 → 34928 | 63.4% | 99.78 → 77.71 |
| deep | 1000 | 932022 → 278640 | 70.1% | 118.53 → 83.21 |
| deep | 10000 | 9777942 → 4456560 | 54.4% | 145.02 → 86.65 |
| deep | 20000 | 19577590 → 8913008 | 54.5% | 141.49 → 89.93 |
| deep | 100000 | 95615478 → 35651696 | 62.7% | 133.07 → 81.99 |

The table shows medians, not confidence intervals. Raw paired samples are in
[measurements.csv](measurements.csv); exact source/header/binary hashes, compiler,
affinity and executed commands are in [environment.json](environment.json).
The reduction in index payload in these cases is 54–74%; this is **not** a
claim that total mt-c RAM falls by that percentage. Model/YAML/subscription
copies remain separate costs and are intentionally not changed in this PR.

Two exploratory versions were rejected before this frozen series: hashing
every label made deep lookups slower; packed sibling lookup and a 50%-loaded
edge index corrected that in the measured corpus. Do not generalize the timing
result to all workloads or router architectures. No physical-router/QEMU DNS
throughput comparison is claimed here.

## Reproduce

With the baseline commit available in the local git clone and a C compiler plus
PCRE2 development headers/library installed:

```sh
python3 tools/bench/run_namespace_memory.py --output /tmp/ns-memory-new \
  --pairs 5 --queries 2000000 --rules 100 1000 10000 20000 100000
```

For an offline run, `--baseline-file` accepts only a byte-identical copy of the
recorded baseline blob. `--cpu` selects an allowed CPU; otherwise the smallest
allowed CPU is used. The output directory must be new. `make test` and
`make sanitize` automatically discover `test_match_compact.c`.
