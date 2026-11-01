#!/usr/bin/env python3
"""Build and compare the rules-reconciliation harness reproducibly.

Use --build-only first; no timing takes place in that mode. The timing mode
verifies the adjacent build manifest and never compiles. Calibration is saved
separately; five alternating pairs use one fixed iteration count per case.
Take measurements only after other builds/tests/benchmarks have stopped.

From the repository root (GCC and the system PCRE2 development package):
  python3 tools/bench/run_rules_reconcile_compare.py --build-only --build-dir .build/rules-reconcile
  python3 tools/bench/run_rules_reconcile_compare.py --build-dir .build/rules-reconcile --output /fresh/results --cpu 7
The baseline commit must be present in the local git object database. The
build creates both binaries, the extracted baseline source and their hash/
compiler/command manifest; no dated report or earlier workspace is required.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[2]
CASES = ((1000, 1000), (10000, 1000), (10000, 10000), (100000, 10000), (100000, 0))
REPEATS = 5
TARGET_CANDIDATE_NS = 250_000_000
BASELINE_BUDGET_NS = 5_000_000_000
BASELINE_COMMIT = "6a94817a3764dbf9280a9fe657c2afd943725cb4"
RULESET = "src/backend-c/src/netfilter/ruleset.c"
HARNESS = "tools/bench/rules_reconcile_bench.c"
COMMON_SOURCES = (
    "src/backend-c/src/rules/match.c", "src/backend-c/src/rules/wildcard.c",
    "src/backend-c/src/dns_cache/cache.c", "src/backend-c/src/config/models.c",
    "src/backend-c/src/util/lookup.c", "src/backend-c/src/util/err.c",
    "src/backend-c/src/logging/log.c",
)
SCOPE = "namespace desired-list construction + validation + freeing; process CPU; no kernel or network"


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def now():
    return datetime.now(timezone.utc).isoformat()


def build_binaries(directory):
    directory.mkdir(parents=True, exist_ok=True)
    baseline = directory / "baseline-ruleset.c"
    baseline.write_bytes(subprocess.check_output(["git", "show", BASELINE_COMMIT + ":" + RULESET], cwd=ROOT))
    inputs = {ROOT / RULESET, ROOT / HARNESS, baseline}
    inputs.update(ROOT / name for name in COMMON_SOURCES)
    inputs.update((ROOT / "src/backend-c/include").rglob("*.h"))
    hashes = {str(path): sha256(path) for path in sorted(inputs)}
    common = ["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Wconversion", "-Wshadow",
              "-Wformat=2", "-Wundef", "-Wstrict-prototypes", "-Wmissing-prototypes", "-Werror",
              "-D_POSIX_C_SOURCE=200809L", "-ffunction-sections", "-fdata-sections", "-Isrc/backend-c/include",
              HARNESS, *COMMON_SOURCES, "-Wl,--gc-sections", "-lpcre2-8"]
    commands, binaries = {}, {}
    for variant in ("baseline", "candidate"):
        binary = directory / ("rules-reconcile-" + variant)
        command = list(common)
        if variant == "baseline":
            command.insert(1, '-DMT_BENCH_RULESET_SOURCE="' + str(baseline) + '"')
        command += ["-o", str(binary)]
        subprocess.run(command, cwd=ROOT, check=True)
        commands[variant] = command
        binaries[variant] = dict(file=binary.name, sha256=sha256(binary))
    if any(sha256(Path(name)) != digest for name, digest in hashes.items()):
        raise RuntimeError("source changed during benchmark build")
    manifest = dict(schema=1, built_utc=now(), baseline_commit=BASELINE_COMMIT,
                    git_head_at_build=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                    compiler=subprocess.check_output(["gcc", "--version"], text=True).splitlines()[0],
                    source_sha256=hashes, build_commands=commands, binaries=binaries)
    (directory / "benchmark-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("Built both harness binaries and benchmark-manifest.json; no measurements executed.", flush=True)


def validate_build_manifest(directory):
    manifest = json.loads((directory / "benchmark-manifest.json").read_text())
    if manifest.get("schema") != 1 or manifest.get("baseline_commit") != BASELINE_COMMIT:
        raise ValueError("unsupported benchmark build provenance")
    if not manifest.get("source_sha256"):
        raise ValueError("missing benchmark source provenance")
    for name, digest in manifest["source_sha256"].items():
        if sha256(Path(name)) != digest:
            raise ValueError("benchmark source changed; rerun --build-only: " + name)
    binaries = {}
    for variant in ("baseline", "candidate"):
        item = manifest["binaries"][variant]
        if Path(item["file"]).name != item["file"]:
            raise ValueError("benchmark binary must be beside its manifest")
        binary = directory / item["file"]
        if sha256(binary) != item["sha256"]:
            raise ValueError("benchmark binary changed; rerun --build-only: " + variant)
        binaries[variant] = binary
    if manifest["binaries"]["baseline"]["sha256"] == manifest["binaries"]["candidate"]["sha256"]:
        raise ValueError("baseline and candidate binaries are identical")
    return manifest, binaries


def choose_iterations(rules, domains, baseline_ns, candidate_ns):
    if baseline_ns <= 0 or candidate_ns <= 0:
        raise ValueError("calibration CPU time must be positive")
    requested = max(1, math.ceil(TARGET_CANDIDATE_NS / candidate_ns))
    affordable = max(1, BASELINE_BUDGET_NS // baseline_ns)
    if (rules, domains) == (100000, 10000):
        count, reason = 1, "one iteration for the billion-comparison baseline"
    else:
        count = min(requested, affordable, 4096)
        reason = "candidate target" if count == requested else "baseline CPU budget or iteration cap"
    return dict(rules=rules, domains=domains, iterations=count, reason=reason,
                target_candidate_ns=TARGET_CANDIDATE_NS, estimated_baseline_budget_ns=BASELINE_BUDGET_NS,
                requested_iterations=requested, baseline_affordable_iterations=affordable,
                estimated_candidate_ns=candidate_ns * count, estimated_baseline_ns=baseline_ns * count)


def validate_result(row, rules, domains, iterations):
    expected = dict(rules=rules, domains=domains, iterations=iterations,
                    validated_ipv4_entries=domains * iterations,
                    checksum=(domains * (domains + 1) // 2 if iterations % 2 else 0))
    if row.get("fixture") != "namespace_all_hit_v4":
        raise ValueError("unexpected reconciliation fixture")
    for key, value in expected.items():
        if type(row.get(key)) is not int or row[key] != value:
            raise ValueError("invalid result field: " + key)
    for key in ("build_verify_clear_cpu_ns", "peak_rss_kib"):
        if type(row.get(key)) is not int or row[key] <= 0:
            raise ValueError("invalid measurement field: " + key)


def describe(values):
    return dict(median=statistics.median(values), min=min(values), max=max(values))


def summarize(rows, plans):
    cases = []
    for plan in plans:
        selected = [r for r in rows if (r["rules"], r["domains"]) == (plan["rules"], plan["domains"])]
        pairs = []
        variants = {}
        for variant in ("baseline", "candidate"):
            sample = [r for r in selected if r["variant"] == variant]
            if sorted(r["rep"] for r in sample) != list(range(REPEATS)):
                raise ValueError("missing or duplicate paired repeat")
            if any(r["iterations"] != plan["iterations"] for r in sample):
                raise ValueError("iteration count changed within a case")
            variants[variant] = dict(
                cpu_ms_per_reconcile=describe([r["build_verify_clear_cpu_ns"] / r["iterations"] / 1e6 for r in sample]),
                cpu_ms_per_process_interval=describe([r["build_verify_clear_cpu_ns"] / 1e6 for r in sample]),
                process_peak_rss_kib=describe([r["peak_rss_kib"] for r in sample]),
            )
        for rep in range(REPEATS):
            pair = {r["variant"]: r for r in selected if r["rep"] == rep}
            b, c = pair["baseline"], pair["candidate"]
            pairs.append(dict(rep=rep, order=[r["variant"] for r in sorted(pair.values(), key=lambda r: r["sequence"])],
                              baseline_cpu_ms=b["build_verify_clear_cpu_ns"] / b["iterations"] / 1e6,
                              candidate_cpu_ms=c["build_verify_clear_cpu_ns"] / c["iterations"] / 1e6,
                              baseline_peak_rss_kib=b["peak_rss_kib"], candidate_peak_rss_kib=c["peak_rss_kib"],
                              baseline_over_candidate_cpu=b["build_verify_clear_cpu_ns"] / c["build_verify_clear_cpu_ns"]))
        cases.append(dict(plan=plan, variants=variants, pairs=pairs,
                          baseline_over_candidate_median_cpu=variants["baseline"]["cpu_ms_per_reconcile"]["median"] /
                          variants["candidate"]["cpu_ms_per_reconcile"]["median"],
                          candidate_minus_baseline_median_peak_rss_kib=variants["candidate"]["process_peak_rss_kib"]["median"] -
                          variants["baseline"]["process_peak_rss_kib"]["median"]))
    if len(rows) != len(plans) * 2 * REPEATS:
        raise ValueError("unexpected measurement count")
    return dict(scope=SCOPE, measured_runs=len(rows), repeats=REPEATS, cases=cases)


def write_readme(output, summary):
    text = ["# Bounded rules reconciliation measurements", "", SCOPE + ".", "",
            "Every cached name matches one namespace and owns a distinct IPv4 address. TTL is 60 seconds.",
            "Both variants use the identical C harness and shared matcher/cache implementation; only ruleset.c differs.",
            "Input construction is outside CPU timing. RSS covers the entire process including input setup,",
            "allocator high-water behavior and launcher/runtime effects; it is not an isolated matcher allocation count.",
            "The temporary index is limited to 4096 rules, not a fixed byte budget.", "",
            "Calibration is separate from the five measured alternating pairs. The selected iteration count stays",
            "fixed within each case. Short candidate intervals are retained when extending them would exceed the",
            "estimated baseline CPU budget; the largest nonempty case always uses one iteration.", "",
            "These are namespace reconciliation CPU costs on this host, not DNS throughput, real kernel ipset cost,",
            "a direct firc comparison, or physical-router speed predictions. Wildcard/regex and frequent static or",
            "leading-dot boundaries have different costs. All raw rows, calibration and commands are retained.", "",
            "| Rules × domains | Iterations | Baseline CPU ms median [min–max] | Candidate CPU ms median [min–max] | Baseline/candidate median | RSS KiB baseline → candidate medians |",
            "|---|---:|---:|---:|---:|---:|"]
    for case in summary["cases"]:
        p, v = case["plan"], case["variants"]
        b, c = v["baseline"]["cpu_ms_per_reconcile"], v["candidate"]["cpu_ms_per_reconcile"]
        text.append(f"| {p['rules']} × {p['domains']} | {p['iterations']} | {b['median']:.6f} [{b['min']:.6f}–{b['max']:.6f}] | "
                    f"{c['median']:.6f} [{c['min']:.6f}–{c['max']:.6f}] | {case['baseline_over_candidate_median_cpu']:.2f} | "
                    f"{v['baseline']['process_peak_rss_kib']['median']} → {v['candidate']['process_peak_rss_kib']['median']} |")
    text += ["", "CPU values in this table are per reconciliation, after division by the fixed iteration count.",
             "`summary.json` contains every paired value plus median/min/max CPU and peak RSS.",
             "`environment.json` records exact hashes, source provenance and build commands.",
             "`results.jsonl`, `calibration.jsonl` and `commands.jsonl` preserve executed runs.", "",
             "## Reproduction", "",
             "Check out the measured source revision, with baseline commit `" + BASELINE_COMMIT + "` available locally.",
             "Use GCC and the system PCRE2 development package. The build step creates all transient inputs and",
             "the adjacent source/compiler/command/binary hash manifest; no dated validation log is an input.", "",
             "```sh", "python3 tools/bench/run_rules_reconcile_compare.py --build-only --build-dir .build/rules-reconcile",
             "# After other builds, tests and benchmarks stop; select an available CPU:",
             "python3 tools/bench/run_rules_reconcile_compare.py --build-dir .build/rules-reconcile --output /fresh/results --cpu 7",
             "```", ""]
    (output / "README.md").write_text("\n".join(text))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / ".build/recovery-rules")
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--cpu", type=int, default=7)
    args = parser.parse_args()
    directory = args.build_dir.resolve()
    if args.build_only:
        build_binaries(directory)
        return
    if args.output is None:
        parser.error("timing requires --output; use --build-only to prepare binaries")
    if args.cpu not in os.sched_getaffinity(0):
        parser.error("selected CPU is outside current affinity")
    manifest, sources = validate_build_manifest(directory)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    metadata = dict(started_utc=now(), completed=False, scope=SCOPE, cpu=args.cpu,
                    invocation=sys.argv, baseline_commit=BASELINE_COMMIT,
                    git_head_at_run=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                    uname=list(os.uname()), python=platform.python_version(), load_average_before=list(os.getloadavg()),
                    matrix=CASES, repeats=REPEATS, calibration_policy=dict(candidate_target_ns=TARGET_CANDIDATE_NS,
                    estimated_baseline_budget_ns=BASELINE_BUDGET_NS, iteration_cap=4096, largest_nonempty_iterations=1),
                    source_sha256=manifest["source_sha256"], build_commands=manifest["build_commands"],
                    binary_sha256={variant: sha256(path) for variant, path in sources.items()},
                    build_manifest_sha256=sha256(directory / "benchmark-manifest.json"),
                    driver_sha256=sha256(Path(__file__)), compiler=manifest["compiler"],
                    rss_scope="whole process high-water RSS, including setup; not an allocator measurement")
    with (output / "environment.json").open("x") as file:
        json.dump(metadata, file, indent=2); file.write("\n")
    shutil.copyfile(__file__, output / "driver-executed.py")
    shutil.copyfile(directory / "benchmark-manifest.json", output / "build-manifest.json")
    rows, plans = [], []
    with tempfile.TemporaryDirectory(prefix="mtc-rules-sync-", dir="/tmp") as directory, \
            (output / "results.jsonl").open("x") as raw, \
            (output / "calibration.jsonl").open("x") as calibrations, \
            (output / "commands.jsonl").open("x") as commands:
        binaries = {}
        for variant, source in sources.items():
            dest = Path(directory) / variant
            shutil.copyfile(source, dest); dest.chmod(0o700)
            if sha256(dest) != metadata["binary_sha256"][variant]:
                raise RuntimeError("binary changed while copying")
            binaries[variant] = dest
        sequence = 0

        def execute(variant, rules, domains, iterations, phase, rep=None):
            nonlocal sequence
            sequence += 1
            command = ["taskset", "-c", str(args.cpu), str(binaries[variant]), str(rules), str(domains), str(iterations)]
            started = now()
            record = dict(sequence=sequence, phase=phase, variant=variant, rep=rep,
                          command=command, started_utc=started)
            commands.write(json.dumps(record) + "\n"); commands.flush()
            before = time.monotonic()
            process = subprocess.run(command, text=True, capture_output=True, timeout=90, check=True)
            wall = time.monotonic() - before
            if process.stderr:
                raise RuntimeError("benchmark wrote unexpected stderr: " + process.stderr)
            result = json.loads(process.stdout)
            validate_result(result, rules, domains, iterations)
            result.update(record, full_process_wall_seconds=wall,
                          cpu_ns_per_reconcile=result["build_verify_clear_cpu_ns"] / iterations)
            return result

        for rules, domains in CASES:
            calibration = {}
            for variant in ("baseline", "candidate"):
                result = execute(variant, rules, domains, 1, "calibration")
                calibrations.write(json.dumps(result) + "\n"); calibrations.flush()
                calibration[variant] = result["build_verify_clear_cpu_ns"]
            plan = choose_iterations(rules, domains, calibration["baseline"], calibration["candidate"])
            plans.append(plan)
            (output / "plans.json").write_text(json.dumps(plans, indent=2) + "\n")
            print(f"case {rules} x {domains}: {plan['iterations']} iterations ({plan['reason']})", flush=True)
            for rep in range(REPEATS):
                for variant in (("baseline", "candidate") if rep % 2 == 0 else ("candidate", "baseline")):
                    result = execute(variant, rules, domains, plan["iterations"], "measured", rep)
                    raw.write(json.dumps(result) + "\n"); raw.flush(); rows.append(result)
                    print(f"  pair {rep} {variant}: {result['cpu_ns_per_reconcile'] / 1e6:.6f} ms, "
                          f"peak RSS {result['peak_rss_kib']} KiB", flush=True)
        result = summarize(rows, plans)
        (output / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
        write_readme(output, result)
    metadata.update(completed=True, finished_utc=now(), load_average_after=list(os.getloadavg()))
    (output / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")


if __name__ == "__main__":
    main()
