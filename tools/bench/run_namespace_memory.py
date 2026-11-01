#!/usr/bin/env python3
"""Paired matcher-only memory/CPU comparison; no networking or kernel ipsets.

The frozen baseline is PR #31's matcher, independently checked by Git blob SHA.
No whole-daemon RSS or physical-router improvement is implied by these results.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile

BASE_REF = "d4dd0f05a968f8e9c9e7186be083d56a26648521"
BASE_BLOB = "9437f3e4a34c2647db904b74b2bb1c1b9d185886"
ROOT = Path(__file__).resolve().parents[2]
BACKEND = ROOT / "src/backend-c"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--baseline-file", type=Path, help="offline copy of the exact baseline blob")
    parser.add_argument("--rules", nargs="+", type=int, default=[1000, 10000, 100000])
    parser.add_argument("--shapes", nargs="+", choices=["shared", "diverse", "deep"], default=["shared", "diverse", "deep"])
    parser.add_argument("--pairs", type=int, default=5)
    parser.add_argument("--queries", type=int, default=1000000)
    parser.add_argument("--cpu", type=int)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    if not args.rules or min(args.rules) <= 0 or max(args.rules) > 1000000 or args.pairs < 1 or not 1 <= args.queries <= 100000000:
        parser.error("invalid measurement sizes")
    data = args.baseline_file.read_bytes() if args.baseline_file else subprocess.check_output(
        ["git", "show", f"{BASE_REF}:src/backend-c/src/rules/match.c"], cwd=ROOT)
    blob = hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()
    if blob != BASE_BLOB:
        raise RuntimeError("baseline is not the recorded production matcher")
    allowed = sorted(os.sched_getaffinity(0))
    cpu = min(allowed) if args.cpu is None else args.cpu
    if cpu not in allowed:
        parser.error("selected CPU is not allowed")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    candidate = BACKEND / "src/rules/match.c"
    sources = [ROOT / "tools/bench/namespace_memory.c", candidate,
               BACKEND / "src/rules/wildcard.c", BACKEND / "src/util/lookup.c", BACKEND / "src/logging/log.c"]
    metadata = dict(started_at=datetime.now(timezone.utc).isoformat(), baseline_ref=BASE_REF, baseline_git_blob=blob, cpu=cpu, platform=platform.platform(),
        compiler=subprocess.check_output([args.cc, "--version"], text=True).splitlines()[0],
        parameters={k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
        source_sha256={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
        scope="matcher only; requested payload excludes allocator overhead; RSS is whole harness process",
        header_sha256={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                       for p in sorted((BACKEND / "include/magitrickle").glob("*.h"))}, commands=[])
    rows = []
    with tempfile.TemporaryDirectory(prefix="mtc-ns-memory-") as directory:
        temp = Path(directory)
        baseline = temp / "baseline-match.c"
        baseline.write_bytes(data)
        binaries = {}
        for variant, source in (("baseline", baseline), ("candidate", candidate)):
            binary = temp / variant
            command = [args.cc, "-std=c11", "-O2", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
                       "-I" + str(BACKEND / "include"), '-DMATCH_SOURCE="' + str(source) + '"']
            if variant == "candidate": command.append("-DCOMPACT_NAMESPACE")
            command += [str(sources[0]), str(sources[2]), str(sources[3]), str(sources[4]), "-lpcre2-8", "-o", str(binary)]
            metadata["commands"].append(command)
            subprocess.run(command, check=True)
            binaries[variant] = binary
        os.sched_setaffinity(0, {cpu})
        with (out / "results.jsonl").open("x") as raw:
            for shape in args.shapes:
                for rules in args.rules:
                    for pair in range(args.pairs):
                        for variant in (("baseline", "candidate") if pair % 2 == 0 else ("candidate", "baseline")):
                            command = [str(binaries[variant]), str(rules), shape, str(args.queries)]
                            row = json.loads(subprocess.check_output(command, text=True, timeout=90))
                            if (row["rules"] != rules or row["shape"] != shape or row["queries"] != args.queries
                                    or row["hits"] != (args.queries + 1) // 2 or row["payload_bytes"] <= 0
                                    or row["query_cpu_ns"] <= 0 or row["rss_rules_kib"] <= 0):
                                raise RuntimeError("benchmark result failed correctness gate")
                            row.update(variant=variant, pair=pair)
                            raw.write(json.dumps(row) + "\n"); raw.flush(); rows.append(row)
        metadata["binary_sha256"] = {v: hashlib.sha256(p.read_bytes()).hexdigest() for v, p in binaries.items()}
    os.sched_setaffinity(0, allowed)
    summaries = []
    for shape in args.shapes:
        for rules in args.rules:
            item = dict(shape=shape, rules=rules)
            for variant in ("baseline", "candidate"):
                selected = [r for r in rows if r["shape"] == shape and r["rules"] == rules and r["variant"] == variant]
                item[variant] = {key: dict(median=statistics.median(r[key] for r in selected),
                    min=min(r[key] for r in selected), max=max(r[key] for r in selected))
                    for key in ("payload_bytes", "rss_rules_kib", "rss_queries_kib", "peak_kib", "build_cpu_ms", "query_cpu_ns")}
            summaries.append(item)
    (out / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
    (out / "summary.json").write_text(json.dumps(summaries, indent=2) + "\n")
    for r in summaries:
        b, c = r["baseline"], r["candidate"]
        print(f"{r['shape']:8} {r['rules']:6} payload {b['payload_bytes']['median']:.0f} -> {c['payload_bytes']['median']:.0f}, "
              f"RSS KiB {b['rss_rules_kib']['median']:.0f} -> {c['rss_rules_kib']['median']:.0f}, "
              f"CPU ns/query {b['query_cpu_ns']['median']:.1f} -> {c['query_cpu_ns']['median']:.1f}")


if __name__ == "__main__":
    main()
