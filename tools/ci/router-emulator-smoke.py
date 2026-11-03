#!/usr/bin/env python3
"""Install and exercise a built OpenWrt mt-c IPK/APK in stavultras/router-emulator.

The emulator and its prepared flash folder are supplied by the calling CI job.
This is a guest integration test, not a host binary/package extraction test:
OpenWrt boots from flash and executes the real package manager, procd service,
and HTTP API. Only Python's standard library is needed on the CI host.
"""

import argparse
import hashlib
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import re
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request


BOOT_TIMEOUT = 720
COMMAND_TIMEOUT = 180
API_HOST_PORT = 18881
ROUTING_SCRIPT = Path(__file__).with_name("router-emulator-routing.sh")
DHCP_SCRIPT = Path(__file__).with_name("router-emulator-dhcp.sh")


class SmokeFailure(RuntimeError):
    pass


@contextmanager
def serve_package(package, baseline_package=None):
    """Expose the candidate and optional upgrade fixture to the guest."""
    class PackageHandler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path == f"/mt-c{package.suffix}":
                payload = package
            elif baseline_package is not None and self.path == f"/mt-c-baseline{package.suffix}":
                payload = baseline_package
            elif self.path == "/routing.sh":
                payload = ROUTING_SCRIPT
            elif self.path == "/dhcp.sh":
                payload = DHCP_SCRIPT
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(payload.stat().st_size))
            self.end_headers()
            with payload.open("rb") as file:
                while chunk := file.read(65536):
                    self.wfile.write(chunk)

        def log_message(self, format_string, *args):
            print("package server: " + (format_string % args), flush=True)

    # QEMU user networking maps guest 10.0.2.2 to the host loopback.
    server = ThreadingHTTPServer(("127.0.0.1", 0), PackageHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server.server_address[1]
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


class SerialConsole:
    def __init__(self, conn, process, log_path):
        self.conn = conn
        self.conn.settimeout(0.6)
        self.process = process
        self.log = log_path.open("wb")
        self.recent = bytearray()
        self.counter = 0
        self.ready = False

    def close(self):
        self.conn.close()
        self.log.close()

    def read(self):
        if self.process.poll() is not None:
            raise SmokeFailure(f"emulator exited prematurely (code {self.process.returncode})")
        try:
            chunk = self.conn.recv(65536)
        except socket.timeout:
            return
        if not chunk:
            raise SmokeFailure("emulator closed its serial console")
        self.log.write(chunk)
        self.log.flush()
        self.recent.extend(chunk)
        if len(self.recent) > 256 * 1024:
            del self.recent[: len(self.recent) - 128 * 1024]

    def wait_for_shell(self, timeout=BOOT_TIMEOUT):
        deadline = time.monotonic() + timeout
        enter_sent = False
        login_sent = False
        while time.monotonic() < deadline:
            self.read()
            # OpenWrt exposes its root shell after Enter on the serial TTY.
            if not enter_sent and b"Please press Enter to activate this console" in self.recent:
                self.conn.sendall(b"\r")
                enter_sent = True
            if not login_sent and b"OpenWrt login:" in self.recent:
                self.conn.sendall(b"root\r")
                login_sent = True
            if re.search(rb"root@[^\r\n:]+:[^\r\n]{0,128}#\s*$", self.recent):
                self.ready = True
                return
        raise SmokeFailure("OpenWrt did not reach a root shell before boot timeout")

    def run(self, command, timeout=COMMAND_TIMEOUT, check=True, show_output=False):
        if not self.ready:
            raise SmokeFailure("guest serial shell is not ready")
        self.counter += 1
        marker = f"__MTC_CI_DONE_{self.counter:04d}__"
        print(f"guest$ {command}", flush=True)
        # The terminal echo contains a literal '%s', so only the shell's
        # actual output can match the numerical status marker.
        self.conn.sendall((command + "\r").encode())
        self.conn.sendall((f"printf '{marker}:%s\\n' \"$?\"\r").encode())
        regex = re.compile(rb"(?:^|\n)" + re.escape(marker.encode()) + rb":([0-9]+)\r?(?:\n|$)")
        deadline = time.monotonic() + timeout
        # Prevent false matches from old commands and stale buffer offsets.
        self.recent.clear()
        while time.monotonic() < deadline:
            self.read()
            match = regex.search(self.recent)
            if match:
                status = int(match.group(1))
                if show_output:
                    print("--- guest output ---", flush=True)
                    print(self.recent.decode("utf-8", errors="replace"), flush=True)
                if check and status:
                    raise SmokeFailure(f"guest command failed (exit {status}): {command}")
                return status
        raise SmokeFailure(f"guest command timed out after {timeout}s: {command}")

    def print_tail(self):
        if self.recent:
            print("--- serial output tail ---", file=sys.stderr)
            print(self.recent[-6000:].decode("utf-8", errors="replace"), file=sys.stderr)


def connect_console(path, process, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise SmokeFailure(f"emulator exited early (code {process.returncode})")
        try:
            conn = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            conn.connect(str(path))
            return conn
        except OSError:
            conn.close()
            time.sleep(0.25)
    raise SmokeFailure(f"emulator console socket not available: {path}")


def check_http_api(timeout=60):
    url = f"http://127.0.0.1:{API_HOST_PORT}/api/v1/groups"
    deadline = time.monotonic() + timeout
    last = "no HTTP response"
    while time.monotonic() < deadline:
        try:
            with urllib.request.urlopen(url, timeout=3) as response:
                data = response.read(1024 * 1024)
                if response.status != 200:
                    raise SmokeFailure(f"unexpected API HTTP status {response.status}")
                decoded = json.loads(data)
                if not isinstance(decoded, dict) or not isinstance(decoded.get("groups"), list):
                    raise SmokeFailure(f"unexpected /api/v1/groups JSON response: {str(decoded)[:200]}")
                print(f"HTTP API OK: {url} returned {len(decoded['groups'])} groups", flush=True)
                return
        except (OSError, urllib.error.URLError, ValueError) as error:
            last = str(error)
            time.sleep(2)
    raise SmokeFailure(f"HTTP API not reachable: {url} ({last})")


def stop_emulator(process, monitor_path):
    if process.poll() is not None:
        return
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
            monitor.settimeout(2)
            monitor.connect(str(monitor_path))
            monitor.sendall(b"quit\n")
        process.wait(timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        process.kill()
        process.wait(timeout=10)


def run_package_command(serial, command, timeout):
    # Early-boot networking and release mirrors can interrupt downloads.
    # Retry a whole operation, but never ignore a persistent package failure.
    for attempt in range(1, 5):
        if serial.run(command, timeout=timeout, check=False, show_output=True) == 0:
            return
        if attempt == 4:
            raise SmokeFailure(f"{command} failed after 4 attempts (see guest output)")
        print(f"{command} attempt {attempt}/4 failed; retrying in 5s", flush=True)
        time.sleep(5)



def wait_for_boot_services(serial):
    """A root prompt can appear before procd finishes initializing OpenWrt.

    The guest shell can accept commands while hostname and logd are absent.
    Wait for both, after netifd has established the default route, instead
    of racing its initialization. A permanently unhealthy guest still fails.
    """
    serial.run(
        "for i in $(seq 1 90); do "
        "test \"$(hostname)\" != '(none)' && "
        "ubus list log | grep -Fxq log && break; sleep 1; "
        "done; test \"$(hostname)\" != '(none)' && "
        "ubus list log | grep -Fxq log",
        timeout=115,
        show_output=True,
    )


def set_guest_clock(serial):
    """Set UTC after system initialization; retry only a bounded boot race.

    The immutable flash image has no usable RTC. Certificate verification of
    OpenWrt's HTTPS feeds requires a current clock; do not disable TLS checks
    or permit package tests to proceed with the wrong time.
    """
    for attempt in range(1, 5):
        host_epoch = int(time.time())
        command = (
            f"date -u -s '@{host_epoch}' >/dev/null && date -u && "
            f"test \"$(date +%s)\" -ge {host_epoch - 2}"
        )
        if serial.run(command, timeout=20, check=False, show_output=True) == 0:
            return
        if attempt == 4:
            raise SmokeFailure("guest UTC clock failed verification after 4 attempts")
        print(
            f"guest UTC clock attempt {attempt}/4 failed during boot; retrying",
            flush=True,
        )
        time.sleep(2)


def download_package(serial, package, guest_path, http_port, name):
    serial.run(f"wget -T 30 -O {guest_path} http://10.0.2.2:{http_port}/{name}", timeout=120)
    package_sha256 = hashlib.sha256(package.read_bytes()).hexdigest()
    serial.run(f"printf '%s  %s\\n' '{package_sha256}' '{guest_path}' | sha256sum -c -")


def install_package(serial, manager, guest_package):
    if manager == "opkg":
        run_package_command(serial, f"opkg install {guest_package}", timeout=240)
        serial.run("opkg list-installed mt-c | grep -q '^mt-c - '")
    else:
        run_package_command(serial, f"apk add --allow-untrusted {guest_package}", timeout=240)
        serial.run("apk info -e mt-c")


def check_running_service(serial):
    # Package scripts, rather than a test-side restart, must start the daemon.
    serial.run("for i in $(seq 1 60); do pidof magitrickled >/dev/null && break; sleep 1; done; pidof magitrickled", timeout=75)
    check_http_api(timeout=75)
    serial.run(
        'sleep 3; set -- $(pidof magitrickled); test "$#" -eq 1 && '
        'test "$(sha256sum /proc/$1/exe | cut -d\' \' -f1)" = '
        '"$(sha256sum /usr/bin/magitrickled | cut -d\' \' -f1)"',
        timeout=20,
    )


def record_upgrade_baseline(serial):
    serial.run(
        'set -- $(pidof magitrickled); test "$#" -eq 1 && '
        'echo "$1" > /tmp/mtci-upgrade.pid && '
        'sha256sum /proc/$1/exe | cut -d\' \' -f1 > /tmp/mtci-upgrade.sha256 && '
        "printf '\\n# mt-c upgrade conffile check\\n' >> /etc/config/magitrickle && "
        'sha256sum /etc/config/magitrickle > /tmp/mtci-upgrade.config'
    )


def check_upgraded_service(serial):
    # The path is unchanged by an upgrade. Check the *running* executable,
    # not just the installed file or an API served by the stale process.
    serial.run(
        'old_pid=$(cat /tmp/mtci-upgrade.pid); '
        'for i in $(seq 1 60); do '
        'set -- $(pidof magitrickled); '
        'test "$#" -eq 1 && test "$1" != "$old_pid" && '
        'test ! -e /proc/$old_pid/exe && break; sleep 1; '
        'done; test "$#" -eq 1 && test "$1" != "$old_pid" && '
        'test ! -e /proc/$old_pid/exe && '
        'test "$(sha256sum /proc/$1/exe | cut -d\' \' -f1)" != '
        '"$(cat /tmp/mtci-upgrade.sha256)" && '
        'test "$(sha256sum /proc/$1/exe | cut -d\' \' -f1)" = '
        '"$(sha256sum /usr/bin/magitrickled | cut -d\' \' -f1)" && '
        'sha256sum -c /tmp/mtci-upgrade.config',
        timeout=80,
        show_output=True,
    )
    check_running_service(serial)
    print("PASS: in-place package upgrade replaced the running executable and preserved the conffile", flush=True)


def run_test(emulator, package, preset, log_dir, openwrt_version, baseline_package=None):
    manager = "opkg" if package.suffix == ".ipk" else "apk"
    guest_package = f"/tmp/mt-c-ci{package.suffix}"
    log_dir.mkdir(parents=True, exist_ok=True)
    work = emulator / "work"
    work.mkdir(parents=True, exist_ok=True)
    console_path = work / "ci-console.sock"
    monitor_path = work / "ci-monitor.sock"
    for path in (console_path, monitor_path):
        path.unlink(missing_ok=True)

    cmd = [str(emulator / "emulator.sh"), "-P", preset,
           "-w", "user", "-l", "user", "-u", "none",
           "-o", f"lan-forwards={API_HOST_PORT}:8080",
           "-S", str(console_path), "-m", str(monitor_path),
           "-L", str(log_dir)]
    with serve_package(package, baseline_package) as http_port, (work / "ci-emulator-launch.log").open("wb") as launcher:
        print(f"Starting emulated OpenWrt {openwrt_version} ({preset}), package bytes: {package.stat().st_size}", flush=True)
        process = subprocess.Popen(cmd, cwd=emulator, stdout=launcher, stderr=subprocess.STDOUT)
        serial = None
        try:
            conn = connect_console(console_path, process)
            serial = SerialConsole(conn, process, work / "ci-serial.log")
            serial.wait_for_shell()
            print("OpenWrt root shell is ready", flush=True)
            serial.run(
                f"command -v {manager} && "
                f'grep -Fxq "DISTRIB_RELEASE=\'{openwrt_version}\'" /etc/openwrt_release'
            )
            serial.run("for i in $(seq 1 30); do ip -4 route show default | grep -q '^default ' && break; sleep 2; done; ip -4 route show default | grep -q '^default '", timeout=75)
            # The serial root prompt is available before OpenWrt finishes
            # procd/netifd/logd initialization. Avoid racing the guest's
            # clock setup while system services are still starting.
            wait_for_boot_services(serial)
            # Fresh flash has no battery-backed RTC. Set and verify UTC only
            # after boot readiness, before any TLS-protected feed access.
            set_guest_clock(serial)
            download_package(serial, package, guest_package, http_port, f"mt-c{package.suffix}")
            run_package_command(serial, f"{manager} update", timeout=180)
            if baseline_package is not None:
                guest_baseline = f"/tmp/mt-c-baseline{package.suffix}"
                download_package(serial, baseline_package, guest_baseline, http_port,
                                 f"mt-c-baseline{package.suffix}")
                install_package(serial, manager, guest_baseline)
                check_running_service(serial)
                record_upgrade_baseline(serial)
            install_package(serial, manager, guest_package)
            serial.run("test -x /usr/bin/magitrickled && test -x /etc/init.d/magitrickle")
            serial.run("grep -A 3 '^  link:' /etc/magitrickle/state/config.yaml | grep -q 'br-lan'")
            if baseline_package is not None:
                check_upgraded_service(serial)
            else:
                check_running_service(serial)
            print(f"PASS: {package.suffix[1:].upper()} installed via {manager}, procd service running, HTTP API responding", flush=True)
            # The forwarding test requires isolated client/server network
            # namespaces, and fails rather than degrading to a rule-only test.
            install = "opkg install" if manager == "opkg" else "apk add"
            run_package_command(serial, f"{install} ip-full kmod-veth ipset uhttpd curl", timeout=240)
            # Exercise discovery against a real dnsmasq lease and the exact
            # SSE/JSON interfaces used by the journal, before changing fw4.
            serial.run(f"wget -T 30 -O /tmp/mtci-dhcp.sh http://10.0.2.2:{http_port}/dhcp.sh", timeout=60)
            serial.run("rm -f /tmp/mtci-dhcp-status /tmp/mtci-dhcp.log")
            serial.run("(sh /tmp/mtci-dhcp.sh > /tmp/mtci-dhcp.log 2>&1; echo $? > /tmp/mtci-dhcp-status) </dev/null >/dev/null 2>&1 &")
            serial.run(
                "for i in $(seq 1 240); do test -f /tmp/mtci-dhcp-status && break; sleep 1; done; test -f /tmp/mtci-dhcp-status",
                timeout=255,
            )
            serial.run("cat /tmp/mtci-dhcp.log", show_output=True)
            serial.run("test \"$(cat /tmp/mtci-dhcp-status)\" = 0")
            print("PASS: real DHCP device discovery, rename and journal reconnect", flush=True)
            serial.run(f"wget -T 30 -O /tmp/mtci-routing.sh http://10.0.2.2:{http_port}/routing.sh", timeout=60)
            serial.run("chmod 700 /tmp/mtci-routing.sh")
            serial.run("rm -f /tmp/mtci-status /tmp/mtci-routing.log")
            # Keep group creation in the guest's real HTTP API. Host-side
            # QEMU port forwarding is already exercised by check_http_api;
            # the routing fixture should not depend on that forwarding
            # still working after a full fw4 reload.
            serial.run("(sh /tmp/mtci-routing.sh > /tmp/mtci-routing.log 2>&1; echo $? > /tmp/mtci-status) </dev/null >/dev/null 2>&1 &")
            serial.run(
                "for i in $(seq 1 360); do test -f /tmp/mtci-status && break; sleep 1; done; test -f /tmp/mtci-status",
                timeout=375,
            )
            serial.run("cat /tmp/mtci-routing.log", show_output=True)
            serial.run("test \"$(cat /tmp/mtci-status)\" = 0")
            print("PASS: all six rule types route client HTTP traffic over selected interface (IPv4 and IPv6)", flush=True)
        except Exception:
            if serial is not None:
                for diagnostic in (
                    "ubus call service list '{\"name\":\"magitrickle\",\"verbose\":true}'",
                    "logread | tail -n 90",
                    "ip -br link",
                    "cat /etc/magitrickle/state/config.yaml | head -n 45",
                    "pidof magitrickled || true",
                    "cat /tmp/mtci-routing.log 2>/dev/null || true",
                    "cat /tmp/mtci-dhcp.log 2>/dev/null || true",
                ):
                    try:
                        serial.run(diagnostic, timeout=15, check=False, show_output=True)
                    except (SmokeFailure, OSError):
                        pass
                serial.print_tail()
            raise
        finally:
            if serial is not None:
                serial.close()
            stop_emulator(process, monitor_path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator-dir", type=Path, required=True)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--baseline-package", type=Path,
                        help="lower-version fixture to install before upgrading to --package")
    parser.add_argument("--preset", default="cudy-wr3000p-v1")
    parser.add_argument("--openwrt-version", required=True)
    args = parser.parse_args()
    emulator = args.emulator_dir.resolve()
    package = args.package.resolve()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.openwrt_version):
        parser.error("--openwrt-version must be a release version, e.g. 24.10.4")
    expected_suffix = ".ipk" if int(args.openwrt_version.split(".")[0]) < 25 else ".apk"
    if not package.is_file() or package.suffix != expected_suffix:
        parser.error(f"--package must be an existing OpenWrt {expected_suffix} for {args.openwrt_version}")
    baseline = args.baseline_package.resolve() if args.baseline_package else None
    if baseline is not None and (not baseline.is_file() or baseline.suffix != expected_suffix or baseline == package):
        parser.error("--baseline-package must be a separate existing package of the same format")
    if not (emulator / "emulator.sh").is_file():
        parser.error("--emulator-dir does not contain emulator.sh")
    try:
        run_test(emulator, package, args.preset, emulator / "work" / "logs", args.openwrt_version, baseline)
    except (SmokeFailure, OSError, subprocess.SubprocessError) as error:
        print(f"FAIL: {error}", file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
