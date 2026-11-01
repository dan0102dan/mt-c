#!/usr/bin/env python3
"""Paired real-package, real-kernel OpenWrt emulator checks and DNS timings.

Uses CI's existing DHCP/LAN-routing fixtures. Detailed DNS and load clients run
inside the guest. QEMU measurements are not physical-router speed estimates.
"""
import argparse
import configparser
from contextlib import contextmanager
import fcntl
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
import ipaddress
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "bench"))
from dns_compare import config, sha256, summary, validate_load

spec = importlib.util.spec_from_file_location("mtc_smoke", HERE / "router-emulator-smoke.py")
SMOKE = importlib.util.module_from_spec(spec)
spec.loader.exec_module(SMOKE)


def mirror_key(path):
    parsed = urllib.parse.urlsplit(path)
    if parsed.query or parsed.fragment or parsed.netloc or not parsed.path.startswith("/openwrt/"):
        return None
    key = urllib.parse.unquote(parsed.path[len("/openwrt/"):])
    if not re.fullmatch(r"[A-Za-z0-9._+~/-]+", key) or any(p in ("", ".", "..") for p in key.split("/")):
        return None
    return key if key.startswith(("releases/24.10.4/", "releases/25.12.5/")) else None


@contextmanager
def serve_assets(assets, cache):
    """Whitelist local files; mirror only public signed OpenWrt release feeds."""
    locks = {}
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            path = assets.get(self.path)
            if path is None:
                key = mirror_key(self.path)
                if key is None:
                    self.send_error(404)
                    return
                path = cache / key
                with locks.setdefault(key, threading.Lock()):
                    if not path.is_file():
                        path.parent.mkdir(parents=True, exist_ok=True)
                        temporary = None
                        try:
                            # Keep inherited proxy/CA trust. Guest verifies feed signatures.
                            with urllib.request.urlopen("https://downloads.openwrt.org/" + key, timeout=120) as upstream:
                                with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as tmp:
                                    temporary = Path(tmp.name)
                                    shutil.copyfileobj(upstream, tmp)
                                length = upstream.headers.get("Content-Length")
                                if length is not None and temporary.stat().st_size != int(length):
                                    raise OSError("incomplete mirror response")
                            temporary.replace(path)
                        except Exception:
                            if temporary is not None:
                                temporary.unlink(missing_ok=True)
                            self.send_error(502, "OpenWrt download failed")
                            return
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(path.stat().st_size))
            self.end_headers()
            with path.open("rb") as source:
                shutil.copyfileobj(source, self.wfile)

        def log_message(self, fmt, *args):
            print("asset server: " + fmt % args, flush=True)

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        yield server.server_address[1]
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=5)


def guest(serial, command, timeout=180):
    serial.run(command, timeout=timeout, show_output=True)
    return serial.recent.decode(errors="replace").replace("\r", "")


def shell(script):
    return "sh -ec " + shlex.quote(script)


def stop_service(serial):
    guest(serial, shell("""if test -x /etc/init.d/magitrickle; then /etc/init.d/magitrickle stop; fi
# OpenWrt BusyBox sleep may be built without fractional-second support.
for i in $(seq 1 15); do pidof magitrickled >/dev/null || break; sleep 1; done
! pidof magitrickled >/dev/null"""), 30)


def identity_command(variant):
    if variant not in ("baseline", "candidate"):
        raise ValueError("unknown variant")
    return shell(f"""expected=$(sha256sum /tmp/mt-c-{variant} | cut -d' ' -f1)
case "$expected" in ''|*[!0-9a-f]*) exit 1;; esac
test "$(printf '%s' "$expected" | wc -c)" -eq 64
test "$(sha256sum /usr/bin/magitrickled | cut -d' ' -f1)" = "$expected"
pid=$(pidof magitrickled)
case "$pid" in ''|*[!0-9]*) exit 1;; esac
test "$(sha256sum /proc/$pid/exe | cut -d' ' -f1)" = "$expected"
printf 'MTC_BINARY %s %s\\n' "$expected" "$pid" """)


def verify_running_binary(serial, variant):
    text = guest(serial, identity_command(variant))
    match = re.search(r"^MTC_BINARY ([0-9a-f]{64}) ([0-9]+)$", text, re.M)
    if not match:
        raise RuntimeError("missing running-binary identity")
    return dict(sha256=match[1], pid=int(match[2]))


def download(serial, port, name, destination, digest, executable=False):
    guest(serial, f"wget -T 60 -O {shlex.quote(destination)} http://10.0.2.2:{port}/{name}", 120)
    guest(serial, f"printf '%s  %s\\n' {digest} {shlex.quote(destination)} | sha256sum -c -")
    if executable:
        guest(serial, f"chmod 700 {shlex.quote(destination)}")


def prepare(serial, port, manager, version):
    guest(serial, f"date -u -s '@{int(time.time())}' && command -v {manager}")
    guest(serial, f"grep -Fxq \"DISTRIB_RELEASE='{version}'\" /etc/openwrt_release")
    guest(serial, "for i in $(seq 1 30); do ip -4 route show default | grep -q '^default ' && break; sleep 2; done; ip -4 route show default | grep -q '^default '", 75)
    paths = "/etc/opkg/distfeeds.conf" if manager == "opkg" else "/etc/apk/repositories /etc/apk/repositories.d/*.list"
    guest(serial, shell(f"""for file in {paths}; do
if test -f "$file"; then
# A preserved NAND may contain the previous invocation's ephemeral mirror port.
sed -i -e 's#https://downloads.openwrt.org/#http://10.0.2.2:{port}/openwrt/#g' \
-e 's#http://10[.]0[.]2[.]2:[0-9][0-9]*/openwrt/#http://10.0.2.2:{port}/openwrt/#g' "$file"
fi
done"""))
    SMOKE.run_package_command(serial, f"{manager} update", timeout=240)
    install = "opkg install" if manager == "opkg" else "apk add"
    packages = "ip-full kmod-veth ipset uhttpd curl iptables-nft iptables-mod-conntrack-extra kmod-ipt-nat kmod-ipt-ipset ip6tables-nft libatomic libyaml libpcre2 libmnl libcurl libubus libubox rpcd-mod-luci"
    SMOKE.run_package_command(serial, f"{install} {packages}", timeout=600)


def install(serial, port, package, variant, manager, expected_sha256):
    stop_service(serial)
    remove = "if apk info -e mt-c; then apk del mt-c; fi" if manager == "apk" else "if opkg list-installed mt-c | grep -q '^mt-c '; then opkg remove mt-c; fi"
    guest(serial, remove)
    guest(serial, "rm -f /etc/config/magitrickle /etc/magitrickle/state/config.yaml")
    path = f"/tmp/{variant}{package.suffix}"
    download(serial, port, f"{variant}{package.suffix}", path, sha256(package))
    command = "apk add --allow-untrusted" if manager == "apk" else "opkg install"
    SMOKE.run_package_command(serial, f"{command} {path}", timeout=300)
    guest(serial, f"cp /usr/bin/magitrickled /tmp/mt-c-{variant} && sha256sum /tmp/mt-c-{variant}")
    guest(serial, "/etc/init.d/magitrickle restart; sleep 2; pidof magitrickled", 30)
    SMOKE.check_http_api(timeout=75)
    identity = verify_running_binary(serial, variant)
    if identity["sha256"] != expected_sha256:
        raise RuntimeError(f"installed {variant} differs from independently extracted package payload")
    return identity


def smoke(serial, port, output, variant):
    for name, limit in (("dhcp", 250), ("routing", 370)):
        path = output / f"{name}-executed.sh"
        download(serial, port, name + ".sh", f"/tmp/mtci-{name}.sh", sha256(path), True)
        guest(serial, f"rm -f /tmp/mtci-{name}-done; (sh /tmp/mtci-{name}.sh >/tmp/mtci-{name}.log 2>&1; echo $? >/tmp/mtci-{name}-done) </dev/null >/dev/null 2>&1 &")
        guest(serial, f"for i in $(seq 1 {limit}); do test -f /tmp/mtci-{name}-done && break; sleep 1; done; test -f /tmp/mtci-{name}-done", limit + 15)
        (output / f"{variant}-{name}.log").write_text(guest(serial, f"cat /tmp/mtci-{name}.log"))
        guest(serial, f"test \"$(cat /tmp/mtci-{name}-done)\" = 0")



def validate_baseline_smoke_reuse(directory, metadata):
    """Reuse only completed, byte-identical baseline LAN/DHCP evidence."""
    path = directory / "metadata.json"
    previous = json.loads(path.read_text())
    if previous.get("completed") is not True:
        raise RuntimeError("baseline smoke reuse needs a completed prior run")
    expected = metadata["build_provenance"]["baseline"]["package_binary_sha256"]
    if previous.get("installed_daemon_sha256", {}).get("baseline") != expected:
        raise RuntimeError("baseline smoke executable differs")
    if previous.get("build_provenance", {}).get("baseline", {}).get("package_binary_sha256") != expected:
        raise RuntimeError("baseline smoke package payload differs")
    for key in ("openwrt_version", "preset"):
        if previous.get("parameters", {}).get(key) != metadata["parameters"][key]:
            raise RuntimeError("baseline smoke guest parameters differ: " + key)
    for key in ("qemu_binary_sha256", "preset_sha256", "official_firmware_sha256"):
        if not previous.get(key) or previous[key] != metadata[key]:
            raise RuntimeError("baseline smoke runtime differs: " + key)
    for values in (previous, metadata):
        launchers = [digest for name, digest in values["helper_sha256"].items()
                     if Path(name).name == "emulator.sh"]
        if len(launchers) != 1:
            raise RuntimeError("missing unique emulator launcher hash")
        if values is previous:
            old_launcher = launchers[0]
        elif launchers[0] != old_launcher:
            raise RuntimeError("baseline smoke emulator launcher differs")
    logs = {}
    for name, marker in (("dhcp", "PASS: device name rediscovered after closing and reopening the journal"),
                         ("routing", "PASS: all six rule types forwarded real client traffic and stopped after deletion")):
        asset = "/" + name + ".sh"
        if previous.get("asset_sha256", {}).get(asset) != metadata["asset_sha256"][asset]:
            raise RuntimeError("baseline smoke fixture differs: " + name)
        log = directory / f"baseline-{name}.log"
        if marker not in log.read_text():
            raise RuntimeError("baseline smoke pass marker missing: " + name)
        logs[str(log)] = sha256(log)
    return dict(directory=str(directory), metadata_sha256=sha256(path),
                baseline_binary_sha256=expected, log_sha256=logs)


def activate(serial, port, variant, config_name, config_path):
    stop_service(serial)
    guest(serial, shell(f"cp /tmp/mt-c-{variant} /usr/bin/magitrickled.next && chmod 755 /usr/bin/magitrickled.next && mv /usr/bin/magitrickled.next /usr/bin/magitrickled"))
    download(serial, port, config_name, "/etc/magitrickle/state/config.yaml", sha256(config_path))
    guest(serial, "/etc/init.d/magitrickle start; sleep 1; pidof magitrickled", 30)
    return verify_running_binary(serial, variant)


def query(serial, proto="udp", name="d000000.bench.example.com.", typ="A"):
    text = guest(serial, "GOMAXPROCS=1 /tmp/dnsquery -server 127.0.0.1:3553 -proto " + proto + " -name " + shlex.quote(name) + " -type " + typ + " -timeout 3s", 10)
    lines = [line for line in text.splitlines() if line.startswith("{")]
    if len(lines) != 1:
        raise RuntimeError("expected exactly one DNS result")
    return json.loads(lines[0])


def check_records(response, name="d000000.bench.example.com.", cnames=2, addresses=4, ttl=300, aaaa=False, typ="A"):
    expected, owner = [], name
    for i in range(cnames):
        target = f"c{i}.{name}"
        expected.append(dict(name=owner, type=5, **{"class": 1}, ttl=ttl, value=target))
        owner = target
    if typ == "A":
        expected += [dict(name=owner, type=1, **{"class": 1}, ttl=ttl, value=f"10.99.0.{i+1}") for i in range(addresses)]
    if aaaa:
        expected += [dict(name=owner, type=28, **{"class": 1}, ttl=ttl, value=f"fd00::{i+1}") for i in range(addresses)]
    if response.get("error") or response.get("records", []) != expected:
        raise RuntimeError(f"DNS records differ: expected {expected}, received {response}")


def parse_sets(text):
    sets = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0] == "create" and fields[1].startswith("mt_aa"):
            sets.setdefault(fields[1], [])
        if len(fields) >= 3 and fields[0] == "add" and fields[1].startswith("mt_aa"):
            ttl = int(fields[fields.index("timeout") + 1]) if "timeout" in fields else 0
            sets.setdefault(fields[1], []).append(dict(address=str(ipaddress.ip_address(fields[2])), ttl=ttl))
    return sets


def check_sets(serial, groups=2, addresses=4, ttl=3900, ipv6=True):
    sets = parse_sets(guest(serial, "ipset save"))
    for group in range(groups):
        for family in (4, 6):
            name = f"mt_aa{group+1:06x}_{family}"
            expected = {f"10.99.0.{i+1}" if family == 4 else f"fd00::{i+1}" for i in range(addresses if family == 4 or ipv6 else 0)}
            entries = sets.get(name)
            if entries is None or {r["address"] for r in entries} != expected or len(entries) != len(expected):
                raise RuntimeError(f"kernel membership mismatch: {name}, expected {expected}, got {entries}")
            if any(abs(row["ttl"] - ttl) > 30 for row in entries):
                raise RuntimeError(f"kernel TTL mismatch: expected {ttl}, got {entries}")
    return sets


@contextmanager
def upstream(args, output, cnames, addresses, ttl=300, mixed=False):
    command = ["taskset", "-c", str(args.stub_cpu), str(args.dnsstub), "-listen", "127.0.0.1:15399", "-cname", str(cnames),
               "-answer-count", str(addresses), "-ttl", str(ttl), "-compress", "-drop-suffix", "drop.timeout.test."]
    if mixed:
        command.append("-also-aaaa")
    with (output / "upstream.log").open("a") as log:
        process = subprocess.Popen(command, env={**os.environ, "GOMAXPROCS": "1"}, stdout=log, stderr=subprocess.STDOUT)
        try:
            for proto in ("udp", "tcp"):
                deadline = time.monotonic() + 5
                while True:
                    response = json.loads(subprocess.check_output([str(args.host_dnsquery), "-server", "127.0.0.1:15399", "-proto", proto], text=True, timeout=5))
                    if not response.get("error"):
                        break
                    if process.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError("upstream not ready")
                    time.sleep(0.05)
            yield
            if process.poll() is not None:
                raise RuntimeError("upstream exited unexpectedly")
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)


def correctness(args, serial, port, output, variant):
    checks, wire = [], []
    def receive(proto="udp", name="d000000.bench.example.com.", typ="A"):
        response = query(serial, proto, name, typ)
        wire.append(dict(proto=proto, name=name, type=typ, response=response))
        return response
    with upstream(args, output, 2, 4, mixed=True):
        activate(serial, port, variant, "correctness.yaml", output / "correctness.yaml")
        check_records(receive(name="unmatched.invalid."), name="unmatched.invalid.")
        check_sets(serial, addresses=0)
        checks.append(dict(name="unmatched_resolves_without_kernel_write"))
        for _ in range(2):
            for proto in ("udp", "tcp"):
                check_records(receive(proto))
        checks.append(dict(name="cold_warm_udp_tcp_filter_overlap", ipsets=check_sets(serial)))
    with upstream(args, output, 2, 4, ttl=60, mixed=True):
        check_records(receive(), ttl=60)
        checks.append(dict(name="reduced_ttl_last_write", ipsets=check_sets(serial, ttl=3660)))
        before = verify_running_binary(serial, variant)
        download(serial, port, "reload.yaml", "/etc/magitrickle/state/config.yaml", sha256(output / "reload.yaml"))
        guest(serial, f"kill -HUP {before['pid']}; sleep 1")
        if before != verify_running_binary(serial, variant):
            raise RuntimeError("reload changed daemon identity")
        check_records(receive("tcp"), ttl=60, aaaa=True)
        check_records(receive(typ="AAAA"), ttl=60, aaaa=True, typ="AAAA")
        checks.append(dict(name="live_reload_aaaa", ipsets=check_sets(serial, ttl=3660)))
        dropped = receive(name="drop.timeout.test.")
        if dropped.get("error") != "timeout":
            raise RuntimeError(f"unexpected UDP drop result {dropped}")
        check_records(receive(), ttl=60, aaaa=True)
        checks.append(dict(name="udp_dropped_reply_then_recovery", elapsed_seconds=dropped["elapsed_seconds"]))
        dropped = receive("tcp", name="drop.timeout.test.")
        if dropped.get("error") not in ("eof", "reset") or not 0.8 <= dropped["elapsed_seconds"] <= 2.8:
            raise RuntimeError(f"TCP proxy deadline not observed: {dropped}")
        check_records(receive("tcp"), ttl=60, aaaa=True)
        checks.append(dict(name="tcp_one_second_deadline", elapsed_seconds=dropped["elapsed_seconds"]))
    (output / f"{variant}-dns-records.json").write_text(json.dumps(wire, indent=2) + "\n")
    return dict(variant=variant, checks=checks)


def snapshot(serial):
    text = guest(serial, shell("""p=$(pidof magitrickled)
awk '{print "MTC_TICKS " ($14+$15)}' /proc/$p/stat
awk '/VmRSS:/{print "MTC_RSS " $2}' /proc/$p/status
printf 'MTC_TIMERS '; ls -l /proc/$p/fd | awk '/anon_inode:\\[timerfd\\]/{n++} END{print n+0}' """))
    found = {key: int(value) for key, value in re.findall(r"^MTC_(TICKS|RSS|TIMERS) ([0-9]+)$", text, re.M)}
    if set(found) != {"TICKS", "RSS", "TIMERS"}:
        raise RuntimeError("missing daemon snapshot")
    return found


def load(serial, proto, duration, args):
    text = guest(serial, f"GOMAXPROCS=1 /tmp/dnsload -server 127.0.0.1:3553 -proto {proto} -ndomains {args.names} -concurrency {args.concurrency} -duration {duration}s", duration + 15)
    rows = [json.loads(line) for line in text.splitlines() if line.startswith("{")]
    if len(rows) != 1:
        raise RuntimeError("missing load result")
    validate_load(rows[0])
    return rows[0]


def benchmark(args, serial, port, output):
    rows = []
    with (output / "results.jsonl").open("x") as raw:
        for proto, cnames, addresses in (("udp", 0, 1), ("udp", 2, 4), ("tcp", 2, 4)):
            with upstream(args, output, cnames, addresses):
                for rep in range(args.repeats):
                    for variant in (("baseline", "candidate") if rep % 2 == 0 else ("candidate", "baseline")):
                        activate(serial, port, variant, "benchmark.yaml", output / "benchmark.yaml")
                        check_records(query(serial, proto), cnames=cnames, addresses=addresses)
                        warm = load(serial, proto, args.warmup, args)
                        sets = check_sets(serial, groups=1, addresses=addresses, ipv6=False)
                        before = snapshot(serial)
                        row = load(serial, proto, args.duration, args)
                        after = snapshot(serial)
                        verify_running_binary(serial, variant)
                        row.update(variant=variant, rep=rep, cnames=cnames, addresses=addresses, before=before, after=after,
                                   cpu_ticks_per_query=(after["TICKS"] - before["TICKS"]) / row["ok"], kernel_sets=sets, warmup_ok=warm["ok"])
                        raw.write(json.dumps(row) + "\n")
                        raw.flush()
                        rows.append(row)
                        print(f"MEASURED {variant} {proto} CNAME={cnames} A={addresses}: {row['rps']:.2f} QPS", flush=True)
    (output / "summary.json").write_text(json.dumps(summary(rows), indent=2) + "\n")


def stop_emulator(process, monitor):
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as conn:
            conn.settimeout(2)
            conn.connect(str(monitor))
            conn.sendall(b"quit\n")
        process.wait(timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        pass
    # The launcher is a shell, not exec(QEMU). Reap only our owned process group.
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(process.pid, sig)
        except ProcessLookupError:
            break
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            pass
    process.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("emulator-dir", "baseline", "candidate", "dnsstub", "dnsload", "dnsquery", "host-dnsquery", "output", "cache"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--openwrt-version", choices=("24.10.4", "25.12.5"), required=True)
    parser.add_argument("--preset", default="cudy-wr3000p-v1")
    parser.add_argument("--correctness-only", action="store_true")
    parser.add_argument("--benchmark-only", action="store_true")
    parser.add_argument("--skip-lan-smoke", action="store_true", help="cite separately saved passing LAN smoke")
    parser.add_argument("--reuse-baseline-smoke", type=Path, help="completed matching run; candidate still runs LAN/DHCP smoke")
    parser.add_argument("--guest-test", type=Path, action="append", default=[], help="cross-built unit executable, verified before execution")
    parser.add_argument("--emulator-cpus", default="4,5")
    parser.add_argument("--stub-cpu", type=int, default=6)
    for name, default in (("rules", 1000), ("names", 32), ("concurrency", 10), ("repeats", 3)):
        parser.add_argument("--" + name, type=int, default=default)
    parser.add_argument("--duration", type=float, default=10)
    parser.add_argument("--warmup", type=float, default=2)
    args = parser.parse_args()
    for name, value in vars(args).items():
        if isinstance(value, Path):
            setattr(args, name, value.resolve())
    args.guest_test = [path.resolve() for path in args.guest_test]
    if args.correctness_only and args.benchmark_only:
        parser.error("choose one phase or the full run")
    if args.reuse_baseline_smoke and (args.benchmark_only or args.skip_lan_smoke):
        parser.error("baseline smoke reuse requires a correctness phase with candidate smoke")
    if min(args.rules, args.names, args.concurrency, args.repeats, args.duration, args.warmup) <= 0 or args.names > args.rules:
        parser.error("positive parameters and names <= rules required")
    if not re.fullmatch(r"[A-Za-z0-9_-]+", args.preset) or not re.fullmatch(r"[0-9]+(?:,[0-9]+)*", args.emulator_cpus):
        parser.error("invalid preset or CPU list")
    manager = "opkg" if args.openwrt_version == "24.10.4" else "apk"
    suffix = ".ipk" if manager == "opkg" else ".apk"
    if any(not p.is_file() or p.suffix != suffix for p in (args.baseline, args.candidate)):
        parser.error("incorrect package format")
    builds = {}
    for variant in ("baseline", "candidate"):
        package = getattr(args, variant)
        build = json.loads((package.parent / "build-metadata.json").read_text())
        if build["package_sha256"] != sha256(package) or not re.fullmatch(r"[0-9a-f]{64}", build["package_binary_sha256"]):
            parser.error("package build provenance does not match artifact")
        builds[variant] = build
    output = args.output
    output.mkdir(parents=True, exist_ok=True)
    with (output / "harness-executed.py").open("xb") as file:
        file.write(Path(__file__).read_bytes())
    for name in ("routing", "dhcp"):
        shutil.copyfile(HERE / f"router-emulator-{name}.sh", output / f"{name}-executed.sh")
    for name, rules, groups, drop in (("benchmark", args.rules, 1, False), ("correctness", 1, 2, False), ("reload", 1, 2, True)):
        (output / f"{name}.yaml").write_text(config(upstream="10.0.2.2", port=3553, rules=rules, groups=groups, disable_drop_aaaa=drop))
    assets = {f"/baseline{suffix}": args.baseline, f"/candidate{suffix}": args.candidate, "/dnsload": args.dnsload, "/dnsquery": args.dnsquery}
    assets.update({f"/{name}.sh": output / f"{name}-executed.sh" for name in ("routing", "dhcp")})
    assets.update({f"/{name}.yaml": output / f"{name}.yaml" for name in ("benchmark", "correctness", "reload")})
    assets.update({f"/unit-{i}": path.resolve() for i, path in enumerate(args.guest_test)})
    helpers = (Path(__file__), HERE / "router-emulator-smoke.py", HERE.parent / "bench/dns_compare.py", args.dnsstub, args.host_dnsquery, args.emulator_dir / "emulator.sh")
    metadata = dict(parameters={k: [str(p) for p in v] if k == "guest_test" else str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
                    start_unix=time.time(), emulated=True, physical_router=False, direct_firc_comparison=False,
                    asset_sha256={k: sha256(v) for k, v in assets.items()},
                    helper_sha256={str(p): sha256(p) for p in helpers}, installed_daemon_sha256={},
                    loader="guest loopback GOMAXPROCS=1", sink="real kernel ipsets")
    metadata["build_provenance"] = builds
    qemu = Path(os.environ.get("QEMU_BIN", args.emulator_dir / "src/qemu/build/qemu-system-aarch64"))
    if not qemu.is_absolute():
        qemu = args.emulator_dir / qemu
    if not os.access(qemu, os.X_OK) and not os.environ.get("QEMU_BIN"):
        qemu = args.emulator_dir / "qemu/qemu-system-aarch64"
    metadata["qemu_binary_path"] = str(qemu)
    metadata["qemu_binary_sha256"] = sha256(qemu)
    loader = qemu.parent / "lib/ld-linux-x86-64.so.2"
    qrun = [str(loader), "--library-path", str(qemu.parent / "lib"), str(qemu)] if os.access(loader, os.X_OK) else [str(qemu)]
    metadata["qemu_version"] = subprocess.check_output(qrun + ["--version"], text=True).strip()
    metadata["preset_sha256"] = sha256(args.emulator_dir / "presets" / (args.preset + ".ini"))
    preset = configparser.ConfigParser(interpolation=None, delimiters=("=",))
    preset.read(args.emulator_dir / "presets" / (args.preset + ".ini"))
    nand = args.emulator_dir / preset["preset"]["nand-dir"]
    metadata["nand_before_boot_sha256"] = {p.name:sha256(p) for p in sorted(nand.glob("*")) if p.is_file()}
    metadata["official_firmware_sha256"] = {str(p.relative_to(args.emulator_dir)):sha256(p)
        for p in sorted((args.emulator_dir / "firmware" / args.openwrt_version).glob("*")) if p.is_file()}
    metadata["emulator_archive_sha256"] = {p.name:sha256(p) for p in
        (args.emulator_dir.parent / "emulator.tar.gz", args.emulator_dir.parent / "presets.zip") if p.is_file()}
    if args.reuse_baseline_smoke:
        metadata["reused_baseline_smoke"] = validate_baseline_smoke_reuse(args.reuse_baseline_smoke, metadata)
    def save_metadata():
        (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    save_metadata()
    work = args.emulator_dir / "work"
    work.mkdir(exist_ok=True)
    with (work / "mt-c-compare.lock").open("w") as lock, tempfile.TemporaryDirectory(prefix="mtc-compare-") as sockets:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        console, monitor = Path(sockets) / "serial.sock", Path(sockets) / "monitor.sock"
        command = ["taskset", "-c", args.emulator_cpus, str(args.emulator_dir / "emulator.sh"), "-P", args.preset, "-w", "user", "-l", "user", "-u", "none",
                   "-o", f"lan-forwards={SMOKE.API_HOST_PORT}:8080", "-S", str(console), "-m", str(monitor), "-L", str(output / "qemu")]
        metadata["emulator_command"] = command
        save_metadata()
        with serve_assets(assets, args.cache) as port, (output / "launcher.log").open("wb") as launcher:
            process = subprocess.Popen(command, cwd=args.emulator_dir, stdout=launcher, stderr=subprocess.STDOUT, start_new_session=True)
            serial = None
            try:
                serial = SMOKE.SerialConsole(SMOKE.connect_console(console, process), process, output / "serial.log")
                serial.wait_for_shell()
                prepare(serial, port, manager, args.openwrt_version)
                for name in ("dnsload", "dnsquery"):
                    download(serial, port, name, "/tmp/" + name, sha256(assets["/" + name]), True)
                for i, path in enumerate(args.guest_test):
                    download(serial, port, f"unit-{i}", f"/tmp/unit-{i}", sha256(path), True)
                    text = guest(serial, f"/tmp/unit-{i}", 120)
                    (output / f"guest-test-{i}-{path.name}.log").write_text(text)
                checks = []
                for variant in ("baseline", "candidate"):
                    identity = install(serial, port, getattr(args, variant), variant, manager, builds[variant]["package_binary_sha256"])
                    metadata["installed_daemon_sha256"][variant] = identity["sha256"]
                    save_metadata()
                    if not args.benchmark_only:
                        if not args.skip_lan_smoke and not (variant == "baseline" and args.reuse_baseline_smoke):
                            smoke(serial, port, output, variant)
                        checks.append(correctness(args, serial, port, output, variant))
                        (output / "correctness.json").write_text(json.dumps(checks, indent=2) + "\n")
                        print(f"PASS: {variant} real-kernel DNS correctness", flush=True)
                if not args.correctness_only:
                    benchmark(args, serial, port, output)
                metadata["completed"] = True
                save_metadata()
            finally:
                if serial is not None:
                    try:
                        guest(serial, "sync", 30)
                    except Exception:
                        serial.print_tail()
                    serial.close()
                stop_emulator(process, monitor)


if __name__ == "__main__":
    main()
