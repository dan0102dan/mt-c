#!/usr/bin/env python3
"""Paired production transport/pipeline test with a counting netfilter sink.

Build with --build-only first; take timings only after other builds/tests stop.
No host firewall privileges are used. Use router-emulator-compare.py to measure
the actual packaged daemon and real kernel ipset operations.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import signal
import shutil
import subprocess
import time

from dns_compare import config, sha256, summary, validate_load


HERE = Path(__file__).resolve().parent



def clear_host_build(backend, build_name):
    """Clear only the comparison helper's isolated output directory."""
    if build_name != "perf-compare" or (backend / "build").is_symlink():
        raise ValueError("refusing to clear a non-owned host build directory")
    directory = backend / "build" / build_name
    if directory.is_symlink():
        directory.unlink()
    elif directory.exists():
        shutil.rmtree(directory)
    return directory



def production_objects(backend, build_name):
    """Ask the production Makefile for its exact library translation units."""
    target = ".PHONY: print-bench-objs\nprint-bench-objs:\n\t@printf '%s\\n' $(LIB_OBJS)\n"
    output = subprocess.check_output(["make", "-s", "--no-print-directory", f"BUILD={build_name}",
                                      "CFLAGS_EXTRA=-Werror", "--eval", target, "print-bench-objs"],
                                     cwd=backend, text=True)
    root = (backend / "build" / build_name / "src").resolve()
    objects = []
    for name in output.splitlines():
        path = (backend / name).resolve()
        if not path.is_relative_to(root) or path.suffix != ".o" or not path.is_file():
            raise ValueError("Makefile library object is missing or outside the expected build tree")
        if path.relative_to(root).parts[0] in ("main", "tools"):
            raise ValueError("Makefile library objects unexpectedly include an entrypoint")
        objects.append(path)
    if not objects or len(set(objects)) != len(objects):
        raise ValueError("Makefile library object list is empty or duplicated")
    return sorted(objects)


def build(source, binary, build_name, loop_object=None, proxy_object=None):
    backend = source / "src/backend-c"
    clean_directory = clear_host_build(backend, build_name)
    binary.parent.mkdir(parents=True, exist_ok=True)
    with binary.with_suffix(".build.log").open("w") as log:
        subprocess.run(["make", "-j2", f"BUILD={build_name}", "CFLAGS_EXTRA=-Werror",
                        f"build/{build_name}/magitrickled-c"], cwd=backend, stdout=log, stderr=subprocess.STDOUT, check=True)
        objects = production_objects(backend, build_name)
        if loop_object:
            objects = [loop_object if p.name == "loop.o" else p for p in objects]
        if proxy_object:
            objects = [proxy_object if p.name == "proxy.o" and p.parent.name == "dns" else p for p in objects]
        cmd = ["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-D_POSIX_C_SOURCE=200809L",
               "-I" + str(backend / "include"), str(HERE / "dns-proxy-host.c"),
               *map(str, objects), "-lpthread", "-latomic", "-lyaml", "-lpcre2-8", "-lmnl", "-lcjson", "-lm", "-lcurl", "-o", str(binary)]
        log.write(shlex.join(cmd) + "\n"); log.flush()
        subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, check=True)
    manifest = {str(p.relative_to(source)): sha256(p) for p in (backend / "src").rglob("*.c")}
    manifest.update({str(p.relative_to(source)):sha256(p) for p in (backend / "include").rglob("*.h")})
    binary.with_suffix(".metadata.json").write_text(json.dumps(dict(binary_sha256=sha256(binary),
        source_sha256=manifest, object_selection="Makefile LIB_OBJS", clean_build_directory=str(clean_directory), clean_build_verified=True, source_commit=subprocess.check_output(["git","rev-parse","HEAD"],cwd=source,text=True).strip(),
        loop_override_sha256=sha256(loop_object) if loop_object else None,
        proxy_override_sha256=sha256(proxy_object) if proxy_object else None), indent=2) + "\n")


def snapshot(pid):
    root = Path(f"/proc/{pid}")
    stat = (root / "stat").read_text().split(")", 1)[1].split()
    info = dict(cpu_ticks=int(stat[11])+int(stat[12]))
    for line in (root / "status").read_text().splitlines():
        if line.startswith(("VmRSS:","VmHWM:")):
            key, value = line.split(":",1); info[key] = int(value.split()[0])
    info["timerfds"] = 0
    for path in (root / "fd").iterdir():
        try:
            info["timerfds"] += os.readlink(path) == "anon_inode:[timerfd]"
        except FileNotFoundError:
            pass  # The event loop may close a descriptor during this sample.
    return info


def stop(process):
    if process is None:
        return
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
        try: process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait(timeout=5)


def run(a):
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    # Refuse to accidentally overwrite raw results from a previous run.
    rows = []
    with (out / "results.jsonl").open("x") as raw:
        config_path = out / "config.yaml"
        config_path.write_text(config(upstream_port=a.upstream_port, port=a.port, rules=a.rules))
        (out / "harness-executed.py").write_bytes(Path(__file__).read_bytes())
        metadata = dict(scope="host production transport/pipeline with counting sink; no kernel writes",
                        parameters={k:str(v) if isinstance(v,Path) else v for k,v in vars(a).items()},
                        sha256={str(p):sha256(p) for p in (a.baseline,a.candidate,a.dnsstub,a.dnsload,config_path,HERE/"dns_compare.py",HERE/"dns-proxy-host.c",Path(__file__))},
                        start_unix=time.time(), clock_ticks_per_second=os.sysconf("SC_CLK_TCK"), uname=list(os.uname()))
        (out / "metadata.json").write_text(json.dumps(metadata,indent=2)+"\n")
        cases = [("tcp",0,1)] if a.tcp_only else [("udp",0,1),("udp",0,4),("udp",2,4)] + ([] if a.udp_only else [("tcp",0,1)])
        for proto, cnames, addresses in cases:
            stub = None
            with (out / f"upstream-{proto}-{cnames}-{addresses}.log").open("w") as stub_log:
                try:
                    stub = subprocess.Popen(["taskset","-c",str(a.stub_cpu),str(a.dnsstub),"-listen",f"127.0.0.1:{a.upstream_port}",
                                             "-cname",str(cnames),"-answer-count",str(addresses),"-compress"],env={**os.environ,"GOMAXPROCS":"1"},stdout=stub_log,stderr=subprocess.STDOUT)
                    probe = [str(a.dnsload),"-server",f"127.0.0.1:{a.upstream_port}","-proto",proto,"-mode","probe","-probe-limit","5s"]
                    subprocess.run(probe,check=True,stdout=subprocess.DEVNULL,timeout=8)
                    for rep in range(a.repeats):
                        for variant in (("baseline","candidate") if rep%2 == 0 else ("candidate","baseline")):
                            proxy = None
                            with (out / f"{proto}-{cnames}-{addresses}-{rep}-{variant}.log").open("w") as proxy_log:
                                try:
                                    proxy = subprocess.Popen(["taskset","-c",str(a.proxy_cpu),str(getattr(a,variant)),str(config_path)],stdout=proxy_log,stderr=subprocess.STDOUT)
                                    subprocess.run([str(a.dnsload),"-server",f"127.0.0.1:{a.port}","-proto",proto,"-mode","probe","-probe-limit","5s"],check=True,stdout=subprocess.DEVNULL,timeout=8)
                                    command = ["taskset","-c",str(a.loader_cpu),str(a.dnsload),"-server",f"127.0.0.1:{a.port}","-proto",proto,
                                               "-ndomains",str(a.names),"-concurrency",str(a.concurrency),"-qps",str(a.qps)]
                                    env = {**os.environ,"GOMAXPROCS":"1"}
                                    warm = json.loads(subprocess.check_output(command+["-duration",f"{a.warmup}s"],env=env,text=True,timeout=a.warmup+10))
                                    validate_load(warm)
                                    before = snapshot(proxy.pid)
                                    row = json.loads(subprocess.check_output(command+["-duration",f"{a.duration}s"],env=env,text=True,timeout=a.duration+10))
                                    after = snapshot(proxy.pid)
                                    validate_load(row)
                                    if proxy.poll() is not None or stub.poll() is not None:
                                        raise RuntimeError("test process exited during measured interval")
                                    stop(proxy)
                                    if proxy.returncode:
                                        raise RuntimeError("proxy shutdown failed")
                                    proxy_log.flush()
                                    stats = [json.loads(line) for line in Path(proxy_log.name).read_text().splitlines() if line.startswith("{")]
                                    if len(stats) != 1 or stats[0]["dropped"] or stats[0]["responses"] < warm["ok"] + row["ok"] or stats[0]["sink_calls"] < stats[0]["responses"]:
                                        raise RuntimeError(f"production pipeline/sink gate failed: {stats}")
                                    row.update(variant=variant,rep=rep,cnames=cnames,addresses=addresses,before=before,after=after,
                                               cpu_ticks_per_query=(after["cpu_ticks"]-before["cpu_ticks"])/row["ok"],pipeline_stats=stats[0])
                                    raw.write(json.dumps(row)+"\n"); raw.flush(); rows.append(row)
                                    print(f"{proto} CNAME={cnames} A={addresses} rep={rep} {variant}: {row['rps']:.1f} QPS p95={row['p95_ms']}ms",flush=True)
                                finally: stop(proxy)
                finally: stop(stub)
    (out / "summary.json").write_text(json.dumps(summary(rows),indent=2)+"\n")


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--baseline",type=Path,required=True); p.add_argument("--candidate",type=Path,required=True)
    p.add_argument("--baseline-source",type=Path); p.add_argument("--candidate-source",type=Path)
    p.add_argument("--candidate-loop-object",type=Path)
    p.add_argument("--candidate-proxy-object",type=Path)
    p.add_argument("--build-only",action="store_true")
    p.add_argument("--dnsstub",type=Path); p.add_argument("--dnsload",type=Path); p.add_argument("--output",type=Path)
    p.add_argument("--repeats",type=int,default=5); p.add_argument("--duration",type=float,default=8); p.add_argument("--warmup",type=float,default=1)
    p.add_argument("--rules",type=int,default=1000); p.add_argument("--names",type=int,default=32); p.add_argument("--concurrency",type=int,default=10)
    p.add_argument("--proxy-cpu",type=int,default=0); p.add_argument("--stub-cpu",type=int,default=1); p.add_argument("--loader-cpu",type=int,default=2)
    p.add_argument("--port",type=int,default=13553); p.add_argument("--upstream-port",type=int,default=15399); p.add_argument("--tcp-only",action="store_true")
    p.add_argument("--udp-only",action="store_true"); p.add_argument("--qps",type=float,default=0)
    a=p.parse_args()
    for name in ("baseline","candidate","baseline_source","candidate_source","candidate_loop_object","candidate_proxy_object","dnsstub","dnsload"):
        if getattr(a,name) is not None: setattr(a,name,getattr(a,name).resolve())
    if a.build_only:
        if a.baseline_source is None or a.candidate_source is None: p.error("build needs both source directories")
        build(a.baseline_source,a.baseline,"perf-compare")
        build(a.candidate_source,a.candidate,"perf-compare",a.candidate_loop_object,a.candidate_proxy_object)
        return
    if a.output is None or a.dnsstub is None or a.dnsload is None: p.error("run needs --output --dnsstub --dnsload")
    if min(a.repeats,a.duration,a.warmup,a.rules,a.names,a.concurrency) <= 0 or a.names > a.rules or a.qps < 0 or (a.tcp_only and a.udp_only):
        p.error("positive parameters and names <= rules required")
    if len({a.proxy_cpu,a.stub_cpu,a.loader_cpu}) != 3: p.error("pin proxy, stub and loader to separate CPUs")
    run(a)


if __name__ == "__main__":
    main()
