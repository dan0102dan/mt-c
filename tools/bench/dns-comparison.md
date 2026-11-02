# Paired DNS optimization comparisons

These helpers compare a pristine source checkout with a candidate. They reuse
the existing Go benchmark dependency and Python standard library; they add no
production dependency. Use separate output directories for every experiment.
The runners reject reused raw-result files, invalid DNS replies, dropped load
queries and missing matching/kernel effects. Never run timings concurrently
with builds, unit tests, another benchmark or an active emulator.

## Native transport and pipeline

`dns-proxy-host.c` links the production event loop, DNS transport, parser,
pipeline, cache and matcher. It supplies a counting callback for netfilter.
This isolates that part of the daemon: it does not measure real kernel writes,
HTTP/journal work, router throughput or a complete production daemon.

Build the same fixture programs for both variants:

```sh
go -C tools/bench build -o "$BIN/dnsstub" ./dnsstub
go -C tools/bench build -o "$BIN/dnsload" ./dnsload
go -C tools/bench build -o "$BIN/dnsquery" ./dnsquery
python3 tools/bench/run_dns_host_compare.py --build-only \
  --baseline-source "$BASELINE_SOURCE" --candidate-source "$CANDIDATE_SOURCE" \
  --baseline "$BIN/host-baseline" --candidate "$BIN/host-candidate"
```

Use absolute paths for the variables. Both source builds use the ordinary
Makefile with `-O2` and `-Werror`, in isolated `build/perf-compare` directories.
The helper clears its owned output directory before each build and obtains the
object list from the production Makefile's `LIB_OBJS`; unrelated or stale
objects discovered elsewhere are not linked. It records source and executable
hashes and the exact link command.
An optional `--candidate-loop-object` replaces only the candidate's event-loop
object, making a timer experiment possible without modifying production files.
The analogous `--candidate-proxy-object` replaces the whole DNS proxy object,
including its packing and question-processing logic. Such hybrids are
diagnostic and must be labelled separately.

Overrides require compatible object APIs. The archived initial-candidate
experiments predate the explicit UDP reusable-timer API; substituting the old
baseline loop object into the final implementation alone cannot link that new
API. Replay those diagnostics with their recorded historical sources, not an
arbitrary final checkout.

After all builds finish, run UDP and TCP in separate directories:

```sh
python3 tools/bench/run_dns_host_compare.py \
  --baseline "$BIN/host-baseline" --candidate "$BIN/host-candidate" \
  --dnsstub "$BIN/dnsstub" --dnsload "$BIN/dnsload" \
  --udp-only --repeats 5 --duration 8 --warmup 1 --output "$RESULTS/host-udp"
python3 tools/bench/run_dns_host_compare.py \
  --baseline "$BIN/host-baseline" --candidate "$BIN/host-candidate" \
  --dnsstub "$BIN/dnsstub" --dnsload "$BIN/dnsload" \
  --tcp-only --repeats 5 --duration 12 --warmup 1 --output "$RESULTS/host-tcp"
```

Defaults pin proxy/upstream/loader to different CPUs 0/1/2. Override those flags
to CPUs allowed by the host. The fixture is compressed, matching the emulator
experiment, and Go upstream/loader run with `GOMAXPROCS=1`. Baseline/candidate
order alternates between repetitions. The config has 1000 enabled namespace
rules and the workload has 32 names and 10 clients. Readiness probes use the
scenario's UDP/TCP protocol for both upstream and proxy.
After shutdown, nonzero matching activity, response counts and zero proxy drops
are also checked.

For a separate TCP tail-latency check, `--qps RATE` holds offered load constant.
Choose the rule before inspecting candidate results. The 2026-10-31 experiment
uses 7000 QPS if that is at most 80% of the minimum baseline capacity sample;
otherwise it uses 50% of the baseline median. Use the same rate for both
variants and preserve capacity and fixed-rate results separately.

## Actual OpenWrt packages and kernel behavior

Acquire and SHA256-verify the exact SDKs, router-emulator archive, presets and
official firmware as in `.github/workflows/build.yml`. Build the unchanged
frontend once and use it for both variants. SDK preparation uses only the
normal base/packages/luci feeds and the repository's private cJSON recipe:

```sh
python3 tools/ci/build-emulator-package.py --prepare-only \
  --sdk "$SDK" --source "$BASELINE_SOURCE" --output "$BUILD_LOGS"
python3 tools/ci/build-emulator-package.py \
  --sdk "$SDK" --source "$BASELINE_SOURCE" --frontend "$FRONTEND_DIST" \
  --output "$PACKAGES/baseline"
python3 tools/ci/build-emulator-package.py \
  --sdk "$SDK" --source "$CANDIDATE_SOURCE" --frontend "$FRONTEND_DIST" \
  --output "$PACKAGES/candidate"
CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go -C tools/bench build -o "$BIN/dnsload-arm64" ./dnsload
CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go -C tools/bench build -o "$BIN/dnsquery-arm64" ./dnsquery
```

Preparation is for a disposable SDK: it restores pinned feed checkouts, builds
compile-time dependencies and records logs. Later package builds reuse staged
dependencies with the SDK's `NO_DEPS=1` option. They keep the ordinary recipe,
architecture, security flags and runtime dependency metadata. C/H/Makefile
hashes are verified against the SDK's unpacked sources. After SDK clean, the
fresh source archive is staged and checksummed directly in its download cache
before preparation. After the unpacked-source hash check, the helper removes
only that disposable backend's build directory before compilation; source
hashes alone cannot establish that old object files were rebuilt. Containment
checks reject escaped source/build parents, and a build-directory symlink is
unlinked without recursing into its target. Each package's actual
stripped executable is independently extracted and hashed; the guest must
execute that exact file. Build metadata must remain next to each package.

```sh
python3 tools/ci/router-emulator-compare.py \
  --emulator-dir "$EMULATOR" --openwrt-version 25.12.5 --preset cudy-wr3000p-v1 \
  --baseline "$BASELINE_APK" --candidate "$CANDIDATE_APK" \
  --dnsstub "$BIN/dnsstub" --host-dnsquery "$BIN/dnsquery" \
  --dnsload "$BIN/dnsload-arm64" --dnsquery "$BIN/dnsquery-arm64" \
  --cache "$DOWNLOAD_CACHE" --output "$RESULTS/apk-correctness" --correctness-only
```

Run again with `--benchmark-only` and a new output directory during a quiet
window. Defaults are three pairs of ten-second samples, two-second warm-up,
1000 enabled rules, 32 names and 10 clients. QEMU uses host CPUs 4/5 and the
upstream uses CPU 6; override the affinity flags where necessary. For IPK use
the 24.10.4 SDK/packages and `--preset cudy-tr3000-v1`. Optional repeatable
`--guest-test PATH` arguments checksum and run cross-built unit executables
inside the same guest. `--skip-lan-smoke` requires separately retained passing
LAN evidence and must be disclosed in the report. Alternatively,
`--reuse-baseline-smoke PRIOR_RUN` checks completed prior baseline identity,
QEMU/firmware/preset/fixture hashes and both LAN/DHCP pass logs, then reuses only
that baseline smoke. Candidate smoke and both variants' detailed kernel/DNS
checks still run, and the exact reused proof is recorded in metadata.

The suite checks actual package installation, procd/API, DHCP/SSE and all six
routing rule types using the existing LAN fixtures. Additional gates compare
exact CNAME/A/AAAA records; unmatched names must leave kernel sets empty. Two
overlapping groups must receive all addresses, a shorter TTL must replace the
old timeout, and live AAAA-filter reload must preserve daemon identity. A TCP
drop must close near the configured one-second proxy deadline. A UDP drop plus
successful recovery does not establish the exact UDP exchange reclamation time.

Guest dependency downloads use a read-only local mirror restricted to the two
official OpenWrt release trees. Host HTTPS verification and guest package-feed
signature verification remain enabled. No host credentials enter the guest.
Emulator ownership is protected by a lock; serial sockets are unique, NAND is
synced on exit, and cleanup targets only the launched process group.

Each run saves config/helper/package hashes, exact executed harness, binary
identity checks, QEMU/preset/firmware/NAND provenance, raw measurements and
serial logs. Medians and observed min/max ranges are descriptive; they are not
confidence intervals. Kernel sets are verified before every measured run.
Guest-loopback timing includes guest loader competition and emulated CPU/kernel
costs. It cannot establish a physical-router speedup, Internet data-forwarding
bandwidth, host-forwarded UDP behavior or a win over a different project.
