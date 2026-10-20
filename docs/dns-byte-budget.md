# DNS history: byte-only retention and compact pages

This layout supersedes the storage implementation described in decision D-83.
Its forwarding, opt-in consent, SSE authorization, event bounds and optional
failure policy are unchanged. The comparison baseline is `4c3e468` (40-byte
records, a separate 8192-row ceiling and native allocations for unique names).
All numbers below are local x86-64 results, not router RSS/throughput promises.

## Allocation and encoding

One small control allocation and one byte-store allocation together request
exactly 1048576 bytes while recording. The byte store contains a page directory,
4096 hash buckets, slab free lists and shared 4096-byte pages. Pages are used by
records or key/timestamp size classes according to actual content. There is no
capacity-sized key array, separate record quota, moving arena or native allocation
per append. Stop frees the byte store; Clear resets it without disabling capture.
The reported allocation includes free pages and internal fragmentation. Native
allocator bookkeeping for the two allocations and all non-journal memory are
outside this requested-byte budget.

Each page of records contains an 8-byte timestamp base and 408 ten-byte records:

| Field | Bytes |
| --- | ---: |
| Millisecond offset (30 bits), TCP flag and absolute-time flag | 4 |
| Exact domain handle | 2 |
| Exact binary client handle (zero for unknown) | 2 |
| DNS query type, full 0..65535 range | 2 |

No per-record ID is necessary: the sequence and oldest/newest retained positions
reconstruct it exactly. No ID is reused after Clear/Stop. A timestamp outside the
page base's unsigned 30-bit offset range uses an extra slab slot containing its
full int64 value. Backwards clocks, sparse traffic and signed extrema therefore
remain valid, at a workload-dependent memory cost. Explicit byte encoding and
memcpy avoid unaligned integer loads on MIPS/ARM and host-endianness assumptions.

Shared key headers are 12 bytes, followed by actual wire-name/address bytes in a
size-class slot. Headers include 32-bit reference counts: >65535 references to
one domain must not wrap. Domain case and binary label bytes are not normalized
or combined. IPv4, IPv6 and IPv4-mapped IPv6 remain distinct stored identities;
the existing API display normalization is unchanged.

Keys are pinned before allocation may evict their old references. Pages can be
reassigned between record and key storage. Recycling may remove multiple oldest
records to release one page; the SSE `retained_from` field communicates the exact
boundary. The fixed-size page directory gives O(1) lookup by ID. Hash lookup is
expected constant-time, not a worst-case collision guarantee. Tests cover tiny
budgets so page pressure and every size class are exercised repeatedly.

`capacity` is retained for protocol validation, computed from the byte budget and
minimum record layout. It does not allocate a row array or govern eviction. The
frontend defensive limit is similarly derived from 1 MiB / 10 bytes, rather than
the former unrelated 60000 constant. Real history is always smaller because keys,
page headers, directory and partial pages share the budget.

## Retention measurements

`tools/bench/dns_query_log_memory.c` fills the actual production journal with
300000 questions per scenario. Short names have 33 wire bytes; maximum names have
255. The measured final counts vary as oldest pages are recycled, so these are
workload examples, not guaranteed quotas.

| Scenario | Old retained | New retained | New requested bytes |
| --- | ---: | ---: | ---: |
| 200 short domains, 20 IPv4 clients | 8192 | 101304 | 1048576 |
| Unique short domains, 20 IPv4 clients | 8192 | 17664 | 1048576 |
| Unique maximum names, 20 IPv4 clients | 1925 | 3630 | 1048576 |
| 200 short domains, 20 IPv6 clients | 8192 | 101304 | 1048576 |
| 200 short domains, continuously unique IPv6 clients | 8192 | 24192 | 1048576 |
| Unique maximum names and unique IPv6 clients | 1925 | 3270 | 1048576 |

The one-name/one-client regression retains 102752 rows at its final checkpoint.
Every raw request still exists separately: this is representation compression,
not DNS-event aggregation, sampling or lossy timestamp truncation.

## CPU and regression boundary

Five alternating repetitions of `tools/bench/dns_query_cost.c`, 300000 appends
after warm-up, production 1 MiB enabled, CLOCK_PROCESS_CPUTIME_ID. Values are
medians of each repetition's average nanoseconds/append, not latency percentiles.
The current benchmark's short name is 30 wire bytes. It excludes socket I/O,
SSE serialization, auth, browser rendering and complete routing workloads.

| Workload | Old ns | New ns |
| --- | ---: | ---: |
| Short repeated names | 61.147 | 70.479 |
| Short unique names | 90.195 | 180.146 |
| Maximum repeated names | 362.098 | 380.911 |
| Maximum unique names | 398.002 | 440.542 |

This change maximizes retention within the budget; it is **not faster on every
workload**. In particular, the denser key/record layout adds approximately 90 ns
per short unique append on this host while keeping more history. No universal
zero-overhead or target-router performance claim follows from these values.

Deterministic allocator-wrapping tests require zero malloc/calloc/realloc/memmove
calls during warmed and pressure-driven append/eviction, including forced heap
failure after enable. The previous whole-arena compaction cannot reappear in this
layout: live pages never move. The C model test validates retained IDs and exact
fields across names of every size class, IPv4/IPv6/unknown clients, clock jumps,
INT64_MIN/MAX and clear/re-enable cycles; ASan/UBSan covers the same code.

Frontend tests stage 100000 rows atomically and follow a multi-row byte-pressure
eviction without applying the old row cap. Time sorting, filtering and SSE
lifecycle remain unchanged. CI results must be read for the exact commit; local
browser execution may be restricted by the environment and is not inferred from
TypeScript or C tests. Production-router A/B and prolonged mixed workloads remain
necessary before making a router-wide p95/p99 or throughput guarantee.

## Reproduction

Compile either bench with that version's `src/dns/query_log.c`, `src/dns/wire.c`
and include directory, using `-std=c11 -O2 -D_POSIX_C_SOURCE=200809L`.

```sh
./dns_query_log_memory
# budget, maximum names, continuously unique names, measured appends
./dns_query_cost 1048576 0 1 300000
```

The cost helper also compiles against the old header and uses that version's
production constructor for an honest 1 MiB/8192 baseline. It does not compare an
unbounded test-only history to bounded production storage.
