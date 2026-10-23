#!/usr/bin/env python3
"""Install and exercise a built OpenWrt mt-c APK in stavultras/router-emulator.

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
GUEST_PACKAGE = "/tmp/mt-c-ci.apk"
ROUTING_SCRIPT = Path(__file__).with_name("router-emulator-routing.sh")
DHCP_SCRIPT = Path(__file__).with_name("router-emulator-dhcp.sh")


class SmokeFailure(RuntimeError):
    pass


@contextmanager
def serve_package(package):
    """Expose exactly one package to QEMU's 10.0.2.2 host gateway."""
    class PackageHandler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path == "/mt-c.apk":
                payload = package
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


def run_test(emulator, package, preset, log_dir):
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
    with serve_package(package) as http_port, (work / "ci-emulator-launch.log").open("wb") as launcher:
        print(f"Starting emulated OpenWrt ({preset}), package bytes: {package.stat().st_size}", flush=True)
        process = subprocess.Popen(cmd, cwd=emulator, stdout=launcher, stderr=subprocess.STDOUT)
        serial = None
        try:
            conn = connect_console(console_path, process)
            serial = SerialConsole(conn, process, work / "ci-serial.log")
            serial.wait_for_shell()
            print("OpenWrt root shell is ready", flush=True)
            # A freshly prepared NAND has no saved wall clock, and this
            # QEMU board need not expose a battery-backed RTC. Before an
            # HTTPS feed update, set the guest clock to the CI host's UTC
            # timestamp so certificate validity checks can work. Do not
            # disable TLS certificate verification to mask the issue.
            host_epoch = int(time.time())
            serial.run(
                f"date -u -s '@{host_epoch}' >/dev/null && "
                f"test \"$(date +%s)\" -ge {host_epoch - 2} && date -u",
                timeout=20,
            )
            serial.run("command -v apk && grep -q '25.12.5' /etc/openwrt_release")
            serial.run("for i in $(seq 1 30); do ip -4 route show default | grep -q '^default ' && break; sleep 2; done; ip -4 route show default | grep -q '^default '", timeout=75)
            serial.run(f"wget -T 30 -O {GUEST_PACKAGE} http://10.0.2.2:{http_port}/mt-c.apk", timeout=120)
            package_sha256 = hashlib.sha256(package.read_bytes()).hexdigest()
            serial.run(
                f"printf '%s  %s\\n' '{package_sha256}' '{GUEST_PACKAGE}' | sha256sum -c -"
            )
            # Early boot can briefly disrupt external feed downloads while
            # netifd/dnsmasq finish initializing. Retry failed index updates,
            # but never proceed with a partial index or hide a persistent error.
            for attempt in range(1, 5):
                result = serial.run("apk update", timeout=180, check=False, show_output=True)
                if result == 0:
                    break
                if attempt == 4:
                    raise SmokeFailure("apk update failed after 4 attempts (see guest output)")
                print(f"apk update attempt {attempt}/4 failed; retrying in 5s", flush=True)
                time.sleep(5)
            serial.run(f"apk add --allow-untrusted {GUEST_PACKAGE}", timeout=240)
            serial.run("apk info -e mt-c && test -x /usr/bin/magitrickled && test -x /etc/init.d/magitrickle")
            serial.run("grep -A 3 '^  link:' /etc/magitrickle/state/config.yaml | grep -q 'br-lan'")
            serial.run("/etc/init.d/magitrickle restart", timeout=90)
            # Detect immediate startup failures before waiting for the API.
            serial.run("sleep 3; pidof magitrickled", timeout=20)
            check_http_api(timeout=75)
            # Verify the service survives the API request, not just a
            # momentary procd spawn immediately after package install.
            serial.run("sleep 3; pidof magitrickled", timeout=20)
            print("PASS: APK installed, procd service running, HTTP API responding", flush=True)
            # The forwarding test requires isolated client/server network
            # namespaces, and fails rather than degrading to a rule-only test.
            serial.run("apk add ip-full kmod-veth ipset uhttpd curl", timeout=240)
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
                "for i in $(seq 1 120); do test -f /tmp/mtci-status && break; sleep 1; done; test -f /tmp/mtci-status",
                timeout=135,
            )
            serial.run("cat /tmp/mtci-routing.log", show_output=True)
            serial.run("test \"$(cat /tmp/mtci-status)\" = 0")
            print("PASS: client DNS and HTTP traffic routed over selected interface", flush=True)
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
    parser.add_argument("--preset", default="cudy-wr3000p-v1")
    args = parser.parse_args()
    emulator = args.emulator_dir.resolve()
    package = args.package.resolve()
    if not package.is_file() or package.suffix != ".apk":
        parser.error("--package must be an existing OpenWrt APK")
    if not (emulator / "emulator.sh").is_file():
        parser.error("--emulator-dir does not contain emulator.sh")
    try:
        run_test(emulator, package, args.preset, emulator / "work" / "logs")
    except (SmokeFailure, OSError, subprocess.SubprocessError) as error:
        print(f"FAIL: {error}", file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
