# Bounded rules reconciliation measurements

namespace desired-list construction + validation + freeing; process CPU; no kernel or network.

Every cached name matches one namespace and owns a distinct IPv4 address. TTL is 60 seconds.
Both variants use the identical C harness and shared matcher/cache implementation; only ruleset.c differs.
Input construction is outside CPU timing. RSS covers the entire process including input setup,
allocator high-water behavior and launcher/runtime effects; it is not an isolated matcher allocation count.
The temporary index is limited to 4096 rules, not a fixed byte budget.

Calibration is separate from the five measured alternating pairs. The selected iteration count stays
fixed within each case. Short candidate intervals are retained when extending them would exceed the
estimated baseline CPU budget; the largest nonempty case always uses one iteration.

These are namespace reconciliation CPU costs on this host, not DNS throughput, real kernel ipset cost,
a direct firc comparison, or physical-router speed predictions. Wildcard/regex and frequent static or
leading-dot boundaries have different costs. All raw rows, calibration and commands are retained.

| Rules × domains | Iterations | Baseline CPU ms median [min–max] | Candidate CPU ms median [min–max] | Baseline/candidate median | RSS KiB baseline → candidate medians |
|---|---:|---:|---:|---:|---:|
| 1000 × 1000 | 474 | 7.438652 [7.306351–7.802732] | 0.470922 [0.444758–0.590550] | 15.80 | 14440 → 14440 |
| 10000 × 1000 | 80 | 49.802377 [47.731508–51.259434] | 2.425224 [2.217574–2.882499] | 20.54 | 14440 → 14440 |
| 10000 × 10000 | 6 | 775.745023 [735.605490–805.497511] | 6.876260 [6.815358–8.041286] | 112.81 | 14440 → 14440 |
| 100000 × 10000 | 1 | 4644.948294 [4425.367600–5028.337101] | 35.633233 [34.999273–38.192688] | 130.35 | 18368 → 19400 |
| 100000 × 0 | 283 | 4.576448 [4.214425–5.734069] | 0.935471 [0.906915–1.051259] | 4.89 | 14440 → 14440 |

CPU values in this table are per reconciliation, after division by the fixed iteration count.
`summary.json` contains every paired value plus median/min/max CPU and peak RSS.
`environment.json` records exact hashes, source provenance and build commands.
`results.jsonl`, `calibration.jsonl` and `commands.jsonl` preserve executed runs.

## Reproduction

Check out the measured source revision, with baseline commit `6a94817a3764dbf9280a9fe657c2afd943725cb4` available locally.
Use GCC and the system PCRE2 development package. The build step creates all transient inputs and
the adjacent source/compiler/command/binary hash manifest; no dated validation log is an input.

```sh
python3 tools/bench/run_rules_reconcile_compare.py --build-only --build-dir .build/rules-reconcile
# After other builds, tests and benchmarks stop; select an available CPU:
python3 tools/bench/run_rules_reconcile_compare.py --build-dir .build/rules-reconcile --output /fresh/results --cpu 7
```
