#!/usr/bin/env python3
"""Exercise the production adaptive relay over disposable Linux networks.

Run as root in a NEW network namespace, never on a router's active network:
  sudo unshare --net python3 tools/tests/tproxy_kernel.py \
    --daemon "$PWD/src/backend-c/build/host/magitrickled-c" --family 4

Repeat with --family 6 and with iptables/ip6tables alternatives set to legacy
and nft. Requires iproute2, iptables, ipset, util-linux, openssl and Python 3.
There is no Internet traffic. Missing kernel support is a failure, not a skip.
Child namespaces are held by processes, avoiding named netns mount changes.
"""

import argparse
import http.client
import ipaddress
import json
import os
from pathlib import Path
import select
import shutil
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request


SCRIPT = Path(__file__).resolve()
API = "http://127.0.0.1:18080/api/v1/adaptive"


def command(args, *, check=True, timeout=15):
    result = subprocess.run([str(x) for x in args], capture_output=True,
                            text=True, timeout=timeout, check=False)
    if check and result.returncode:
        raise RuntimeError(f"{' '.join(map(str, args))}: {result.stderr or result.stdout}")
    return result


def endpoint(family, number):
    return f"203.0.113.{number}" if family == 4 else f"2001:db8:113::{number}"


def network(family, number, host):
    address = f"10.250.{number}.{host}" if family == 4 else f"fd42:{number}::{host}"
    return str(ipaddress.ip_address(address))


def emit(event, **fields):
    print(json.dumps({"event": event, **fields}), flush=True)


def serve(args):
    family = socket.AF_INET if args.family == 4 else socket.AF_INET6
    bind_address = "0.0.0.0" if args.family == 4 else "::"
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(args.cert, args.key)

    def http_session(raw, secure):
        connection = raw
        destination = raw.getsockname()[0]
        peer = raw.getpeername()
        try:
            raw.settimeout(8)
            if secure:
                if args.server == "direct" and destination == endpoint(args.family, 13):
                    hello = raw.recv(16384)
                    emit("tls_wait", route=args.server, destination=destination, bytes=len(hello))
                    # Complete TCP, receive the real ClientHello, never reply.
                    # Retry must retain the original browser-side TCP socket.
                    while raw.recv(16384):
                        pass
                    return
                connection = context.wrap_socket(raw, server_side=True)
            with connection.makefile("rb") as reader:
                while True:
                    request = reader.readline(8193)
                    if not request:
                        return
                    if len(request) > 8192:
                        raise ValueError("oversized fixture request")
                    method, path, _ = request.decode("ascii").strip().split(" ")
                    content_length = 0
                    for _ in range(32):
                        line = reader.readline(8193)
                        if line == b"\r\n":
                            break
                        if not line or len(line) > 8192:
                            raise ValueError("incomplete fixture headers")
                        if line.lower().startswith(b"content-length:"):
                            content_length = int(line.split(b":", 1)[1])
                    else:
                        raise ValueError("too many fixture headers")
                    if not 0 <= content_length <= 1024:
                        raise ValueError("oversized fixture body")
                    body = reader.read(content_length)
                    if len(body) != content_length:
                        raise ValueError("incomplete fixture body")
                    emit("request", route=args.server, destination=destination,
                         method=method, path=path, peer=peer[0], peer_port=peer[1])
                    if method == "POST" and destination == endpoint(args.family, 16) and args.server == "direct":
                        # The action has happened. A silent close must never
                        # cause the relay to execute the POST on the other path.
                        return
                    response = json.dumps({"route": args.server, "destination": destination,
                                           "peer": peer[0], "peer_port": peer[1], "path": path}).encode()
                    connection.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                       b"Content-Length: " + str(len(response)).encode() +
                                       b"\r\nConnection: keep-alive\r\n\r\n" + response)
        except (OSError, ValueError, ssl.SSLError):
            return
        finally:
            connection.close()
            if connection is not raw:
                raw.close()

    listeners = []
    for port in (80, 443):
        listener = socket.socket(family, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if family == socket.AF_INET6:
            listener.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
        listener.bind((bind_address, port))
        listener.listen(16)
        listeners.append(listener)
    banner_listener = socket.socket(family, socket.SOCK_STREAM)
    banner_listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if family == socket.AF_INET6:
        banner_listener.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
    banner_listener.bind((bind_address, 8443))
    banner_listener.listen(16)
    datagrams = []
    for number in (() if args.no_udp else range(11, 18)):
        datagram = socket.socket(family, socket.SOCK_DGRAM)
        # A bound destination preserves the response source for conntrack;
        # wildcard sendto() could choose the namespace's gateway-facing IP.
        datagram.bind((endpoint(args.family, number), 443))
        datagrams.append(datagram)

    def accept_loop(listener, secure):
        while True:
            conn, _ = listener.accept()
            threading.Thread(target=http_session, args=(conn, secure), daemon=True).start()

    def udp_loop(datagram):
        while True:
            message, peer = datagram.recvfrom(2048)
            datagram.sendto(json.dumps({"route": args.server, "message": message.decode()}).encode(), peer)

    def banner_loop():
        while True:
            connection, peer = banner_listener.accept()
            try:
                connection.settimeout(3)
                connection.sendall(b"SSH-2.0-local-fixture\r\n")
                with connection.makefile("rb") as reader:
                    request = reader.readline(128)
                if request != b"probe\n":
                    raise ValueError("unexpected banner probe")
                connection.sendall(json.dumps({"route": args.server, "peer": peer[0],
                                               "peer_port": peer[1]}).encode())
            except (OSError, ValueError):
                pass
            finally:
                connection.close()

    for listener, secure in zip(listeners, (False, True)):
        threading.Thread(target=accept_loop, args=(listener, secure), daemon=True).start()
    for datagram in datagrams:
        threading.Thread(target=udp_loop, args=(datagram,), daemon=True).start()
    threading.Thread(target=banner_loop, daemon=True).start()
    Path(args.ready).write_text("ready")
    signal.pause()


def client(args):
    family = socket.AF_INET if args.family == 4 else socket.AF_INET6
    if args.mode == "banner":
        with socket.create_connection((args.client, 8443), timeout=5) as connection:
            with connection.makefile("rb") as reader:
                if reader.readline() != b"SSH-2.0-local-fixture\r\n":
                    raise AssertionError("server-first banner was changed")
                connection.sendall(b"probe\n")
                print(reader.read().decode())
        return
    if args.mode == "udp":
        with socket.socket(family, socket.SOCK_DGRAM) as conn:
            conn.settimeout(5)
            conn.sendto(b"udp-stays-direct", (args.client, 443))
            print(conn.recvfrom(2048)[0].decode())
        return
    port = 443 if args.mode == "tls" else 80
    with socket.create_connection((args.client, port), timeout=5) as raw:
        connection = raw
        if args.mode == "tls":
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            # Only this generated, local fixture certificate is untrusted.
            # The production relay never terminates or verifies client TLS.
            context.check_hostname = False
            context.verify_mode = ssl.CERT_NONE
            connection = context.wrap_socket(raw, server_hostname="tproxy.test")
        try:
            if args.mode == "post-error":
                connection.sendall(b"POST /action HTTP/1.1\r\nHost: tproxy.test\r\n"
                                   b"Content-Length: 1\r\n\r\nx")
                try:
                    closed = connection.recv(1) == b""
                except ConnectionResetError:
                    closed = True
                print(json.dumps({"closed": closed}))
                return
            replies = []
            for index in range(args.requests):
                connection.sendall(f"GET /probe/{index} HTTP/1.1\r\nHost: tproxy.test\r\n"
                                   "Content-Length: 0\r\nConnection: keep-alive\r\n\r\n".encode())
                response = http.client.HTTPResponse(connection)
                response.begin()
                if response.status != 200:
                    raise RuntimeError(f"unexpected HTTP status {response.status}")
                replies.append(json.loads(response.read()))
                response.close()
            print(json.dumps({"replies": replies, "local_port": connection.getsockname()[1]}))
        finally:
            if connection is not raw:
                connection.close()


class Lab:
    def __init__(self, args, directory):
        self.args = args
        self.directory = Path(directory)
        self.processes = []
        self.handles = []
        self.namespaces = {}
        self.daemon = None
        self.log = self.directory / "daemon.log"
        self.tool = "iptables" if args.family == 4 else "ip6tables"
        self.ip_family = f"-{args.family}"

    def ns_command(self, name, *args, **kwargs):
        return command(["nsenter", "--target", self.namespaces[name].pid, "--net", "--", *args], **kwargs)

    def namespace(self, name, number, interface):
        keeper = subprocess.Popen(["unshare", "--net", sys.executable, "-u", "-c",
                                   "import signal; print('ready', flush=True); signal.pause()"],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.processes.append(keeper)
        ready, _, _ = select.select([keeper.stdout], [], [], 5)
        if not ready or keeper.stdout.readline().strip() != "ready":
            raise RuntimeError(f"Cannot create {name} network namespace")
        self.namespaces[name] = keeper
        command(["ip", "link", "add", interface, "type", "veth", "peer", "name", "peer0"])
        command(["ip", "link", "set", "peer0", "netns", keeper.pid])
        self.ns_command(name, "ip", "link", "set", "peer0", "name", "eth0")
        self.ns_command(name, "ip", "link", "set", "lo", "up")
        self.ns_command(name, "ip", "link", "set", "eth0", "up")
        command(["ip", "link", "set", interface, "up"])
        prefix = 24 if self.args.family == 4 else 64
        extra = ["nodad"] if self.args.family == 6 else []
        command(["ip", self.ip_family, "addr", "add", f"{network(self.args.family, number, 1)}/{prefix}",
                 "dev", interface, *extra])
        self.ns_command(name, "ip", self.ip_family, "addr", "add",
                        f"{network(self.args.family, number, 2)}/{prefix}", "dev", "eth0", *extra)
        self.ns_command(name, "ip", self.ip_family, "route", "add", "default", "via",
                        network(self.args.family, number, 1), "dev", "eth0")
        if self.args.family == 4:
            self.ns_command(name, "sysctl", "-qw", "net.ipv4.conf.all.rp_filter=0", "net.ipv4.conf.default.rp_filter=0",
                            "net.ipv4.conf.eth0.rp_filter=0")
        if name in ("direct", "alternate"):
            for destination in range(11, 18):
                self.ns_command(name, "ip", self.ip_family, "addr", "add",
                                f"{endpoint(self.args.family, destination)}/{32 if self.args.family == 4 else 128}",
                                "dev", "lo", *extra)

    def start_server(self, name):
        log = self.directory / f"{name}.log"
        ready = self.directory / f"{name}.ready"
        handle = log.open("w"); self.handles.append(handle)
        proc = subprocess.Popen(["nsenter", "--target", str(self.namespaces[name].pid), "--net", "--",
                                 sys.executable, "-u", str(SCRIPT), "--server", name,
                                 "--family", str(self.args.family), "--cert", str(self.directory / "cert.pem"),
                                 "--key", str(self.directory / "key.pem"), "--ready", str(ready),
                                 *(["--no-udp"] if getattr(self.args, "no_udp", False) else [])],
                                stdout=handle, stderr=subprocess.STDOUT)
        self.processes.append(proc)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if ready.exists():
                return
            if proc.poll() is not None:
                break
            time.sleep(0.02)
        raise RuntimeError(f"{name} fixture did not start: {log.read_text()}")

    def configure(self):
        command(["ip", "link", "set", "lo", "up"])
        command(["sysctl", "-qw", "net.ipv4.ip_forward=1", "net.ipv4.conf.all.rp_filter=0",
                 "net.ipv4.conf.default.rp_filter=0", "net.ipv6.conf.all.forwarding=1"])
        for name, index, interface in (("client", 0, "lan0"), ("direct", 1, "wan0"),
                                       ("alternate", 2, "vpn0"), ("outside", 3, "guest0")):
            self.namespace(name, index, interface)
        for interface, number, metric in (("wan0", 1, 100), ("vpn0", 2, 200)):
            command(["ip", self.ip_family, "route", "add", "default", "via", network(self.args.family, number, 2),
                     "dev", interface, "metric", metric])
        if self.args.family == 4:
            command(["sysctl", "-qw", *[f"net.ipv4.conf.{interface}.rp_filter=0"
                                        for interface in ("lan0", "wan0", "vpn0", "guest0")]])
        # Ordinary router forwarding. Alternative traffic is allowed only by
        # production mt-c rules, so a missing policy mark/accept rule is caught.
        command([self.tool, "-P", "INPUT", "DROP"])
        command([self.tool, "-A", "INPUT", "-i", "lo", "-j", "ACCEPT"])
        if self.args.family == 6:
            command([self.tool, "-A", "INPUT", "-p", "ipv6-icmp", "-j", "ACCEPT"])
        command([self.tool, "-A", "INPUT", "-m", "conntrack", "--ctstate", "ESTABLISHED,RELATED", "-j", "ACCEPT"])
        command([self.tool, "-P", "FORWARD", "DROP"])
        command([self.tool, "-A", "FORWARD", "-m", "conntrack", "--ctstate", "ESTABLISHED,RELATED", "-j", "ACCEPT"])
        for ingress in ("lan0", "guest0"):
            command([self.tool, "-A", "FORWARD", "-i", ingress, "-o", "wan0", "-j", "ACCEPT"])
        self.ns_command("direct", self.tool, "-A", "INPUT", "-d", endpoint(self.args.family, 12),
                        "-p", "tcp", "--dport", "80", "-j", "REJECT", "--reject-with", "tcp-reset")
        self.ns_command("direct", self.tool, "-A", "INPUT", "-d", endpoint(self.args.family, 12),
                        "-p", "tcp", "--dport", "8443", "-j", "REJECT", "--reject-with", "tcp-reset")
        command(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                 "-subj", "/CN=tproxy.test", "-addext", "subjectAltName=DNS:tproxy.test",
                 "-keyout", self.directory / "key.pem",
                 "-out", self.directory / "cert.pem"])
        self.start_server("direct"); self.start_server("alternate")
        self.start_daemon()

    def start_daemon(self):
        family = self.args.family
        config = self.directory / "config.yaml"
        config.write_text(f"""configVersion: 0.8.2.2
app:
  httpWeb:
    enabled: true
    auth:
      enabled: false
    host:
      address: 127.0.0.1
      port: 18080
  dnsProxy:
    host:
      address: 127.0.0.1
      port: 3554
    upstream:
      address: 127.0.0.1
      port: 9
    disableRemap53: true
    disableDropAAAA: true
  netfilter:
    disableIPv4: {'false' if family == 4 else 'true'}
    disableIPv6: {'true' if family == 4 else 'false'}
    startMarkTableIndex: 1000
  adaptiveRouting:
    enabled: true
    interface: vpn0
    ingressInterfaces: [lan0]
    connectTimeoutMs: 400
    tlsTimeoutMs: 400
    idleTimeoutMs: 10000
    ttlSeconds: 60
    maxConnections: 16
    maxEntries: 64
  link: [lan0]
  logLevel: debug
groups:
  - id: aa000001
    name: Manual route has precedence
    interface: vpn0
    enable: true
    priority: 300
    rules:
      - id: aa000002
        type: {'subnet' if family == 4 else 'subnet6'}
        rule: {endpoint(family, 15)}/{32 if family == 4 else 128}
        enable: true
subscriptions: []
""")
        handle = self.log.open("w"); self.handles.append(handle)
        self.daemon = subprocess.Popen([str(Path(self.args.daemon).resolve()), "--config", str(config)],
                                       stdout=handle, stderr=subprocess.STDOUT)
        self.processes.append(self.daemon)
        deadline = time.monotonic() + 10
        last_error = "no API response"
        while time.monotonic() < deadline:
            if self.daemon.poll() is not None:
                break
            try:
                state = self.api()
                if state.get("active"):
                    return
                last_error = json.dumps(state)
            except (OSError, ValueError) as error:
                last_error = str(error)
            time.sleep(0.05)
        raise RuntimeError(f"Adaptive routing failed to activate: {last_error}")

    def api(self, method="GET", body=None, suffix=""):
        data = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(API + suffix, data=data, method=method,
                                     headers={"Content-Type": "application/json"})
        # Ignore CI's optional proxy environment for this local administrative API.
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with opener.open(req, timeout=3) as response:
            return json.load(response)

    def request(self, destination, *, mode="plain", count=1, name="client", failure=False):
        result = self.ns_command(name, sys.executable, str(SCRIPT), "--client", endpoint(self.args.family, destination),
                                 "--family", self.args.family, "--mode", mode, "--requests", count,
                                 check=not failure, timeout=10)
        if failure:
            if not result.returncode:
                raise AssertionError("Request unexpectedly succeeded after interception was disabled")
            return None
        return json.loads(result.stdout)

    def accepted(self):
        return self.api()["stats"]["accepted"]

    def expect_route(self, result, expected):
        replies = result["replies"]
        assert replies and all(reply["route"] == expected for reply in replies), result
        assert len({reply["peer_port"] for reply in replies}) == 1, "existing stream changed upstream"

    def expect_entry(self, destination, port, alternate, protocol="tcp"):
        wanted = ipaddress.ip_address(endpoint(self.args.family, destination))
        entries = self.api()["entries"]
        assert any(ipaddress.ip_address(entry["address"]) == wanted and entry["port"] == port and
                   entry.get("protocol", "tcp") == protocol and
                   entry["alternate"] == alternate for entry in entries), entries

    def test(self):
        before = self.accepted()
        result = self.request(11, count=2)
        self.expect_route(result, "direct")
        assert self.accepted() == before + 1, "unknown destination did not use TPROXY exactly once"
        assert result["replies"][0]["peer"] == network(self.args.family, 1, 1), result
        self.expect_entry(11, 80, False)
        before = self.accepted()
        result = self.request(11)
        self.expect_route(result, "direct")
        assert result["replies"][0]["peer"] == network(self.args.family, 0, 2), result
        assert self.accepted() == before, "learned direct connection was proxied again"
        print("PASS direct learning, persistent stream and subsequent kernel bypass", flush=True)

        before = self.accepted()
        self.expect_route(self.request(12, count=2), "alternate")
        assert self.accepted() == before + 1, "fallback replaced the client-side stream"
        self.expect_entry(12, 80, True)
        before = self.accepted()
        self.expect_route(self.request(12), "alternate")
        assert self.accepted() == before, "learned alternate connection was proxied again"
        print("PASS failed direct TCP connection retries alternate on the same client socket", flush=True)

        before = self.accepted()
        self.expect_route(self.request(13, mode="tls", count=2), "alternate")
        assert self.accepted() == before + 1, "TLS fallback required another client TCP connection"
        self.expect_entry(13, 443, True)
        assert '"event": "tls_wait"' in (self.directory / "direct.log").read_text()
        before = self.accepted()
        self.expect_route(self.request(13, mode="tls"), "alternate")
        assert self.accepted() == before, "learned TLS flow did not bypass relay"
        print("PASS silent ClientHello fallback, real TLS handshake and learned bypass", flush=True)

        before = self.accepted()
        assert self.request(13, mode="udp")["route"] == "direct"
        assert self.accepted() == before, "UDP entered TCP relay"
        print("PASS UDP/443 remains on ordinary routing", flush=True)

        before = self.accepted()
        self.expect_route(self.request(15), "alternate")
        assert self.accepted() == before, "manual group was overridden by adaptive capture"
        self.expect_route(self.request(17, name="outside"), "direct")
        assert self.accepted() == before, "unselected ingress was captured"
        print("PASS manual route priority and excluded ingress", flush=True)

        assert self.request(16, mode="post-error")["closed"]
        direct_events = [json.loads(line) for line in (self.directory / "direct.log").read_text().splitlines()
                         if line.startswith("{")]
        alternate_events = [json.loads(line) for line in (self.directory / "alternate.log").read_text().splitlines()
                            if line.startswith("{")]
        assert sum(event.get("method") == "POST" for event in direct_events) == 1
        assert not any(event.get("method") == "POST" for event in alternate_events), "POST was replayed"
        print("PASS accepted HTTP POST is never repeated after silent close", flush=True)

        # Old configurations keep non-web TCP ports on ordinary routing.
        before = self.accepted()
        self.request(12, mode="banner", failure=True)
        assert self.accepted() == before, "all TCP ports were enabled implicitly"
        settings = self.api()["settings"]
        assert settings["tcpAllPorts"] is False and settings["quicEnabled"] is False, settings
        settings["tcpAllPorts"] = True
        assert self.api("PUT", {"settings": settings})["active"]
        before = self.accepted()
        assert self.request(12, mode="banner")["route"] == "alternate"
        assert self.accepted() == before + 1
        self.expect_entry(12, 8443, True)
        before = self.accepted()
        assert self.request(12, mode="banner")["route"] == "alternate"
        assert self.accepted() == before, "learned non-web TCP flow was captured again"
        print("PASS opt-in all-port TCP fallback with an unmodified server-first protocol", flush=True)

        cleared = self.api("DELETE", suffix="/cache")
        assert cleared["entries"] == [], cleared
        before = self.accepted()
        self.expect_route(self.request(11), "direct")
        assert self.accepted() == before + 1, "cache clear failed to re-enable learning"
        settings = self.api()["settings"]
        settings["enabled"] = False
        stopped = self.api("PUT", {"settings": settings})
        assert stopped["active"] is False, stopped
        saved = command([f"{self.tool}-save"]).stdout
        assert "TPROXY" not in saved, saved
        names = command(["ipset", "list", "-name"]).stdout.splitlines()
        assert not any(name.startswith("mt_t") for name in names), names
        routes = json.loads(command(["ip", self.ip_family, "-j", "route", "show", "table", "all"]).stdout)
        assert not any(route.get("type") == "local" and route.get("dst") == "default" for route in routes), routes
        self.request(12, failure=True)
        settings["enabled"] = True
        restarted = self.api("PUT", {"settings": settings})
        assert restarted["active"] is True, restarted
        self.expect_route(self.request(11), "direct")
        assert self.accepted() == 1, "fresh activation did not start a fresh route cache"
        print("PASS clear, disable kernel cleanup and re-enable", flush=True)

    def diagnostics(self):
        print("--- daemon log ---", file=sys.stderr)
        if self.log.exists():
            print(self.log.read_text()[-18000:], file=sys.stderr)
        for args in ([f"{self.tool}-save"], ["ip", self.ip_family, "rule", "show"],
                     ["ip", self.ip_family, "route", "show", "table", "all"], ["ipset", "list"]):
            result = command(args, check=False)
            print(result.stdout[-18000:] + result.stderr, file=sys.stderr)

    def close(self):
        # Workers exit before keepers, so child network namespaces disappear
        # with their final reference; no host routes or netns mounts remain.
        for process in reversed(self.processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait(timeout=3)
            if process.stdout:
                process.stdout.close()
            if process.stderr:
                process.stderr.close()
        for handle in self.handles:
            handle.close()


def verify_isolation(args):
    if os.geteuid() != 0 or os.stat("/proc/self/ns/net").st_ino == os.stat("/proc/1/ns/net").st_ino:
        raise RuntimeError("Refusing host network: run sudo unshare --net ...")
    for executable in ("ip", "iptables", "ip6tables", "ipset", "unshare", "nsenter", "openssl", "sysctl"):
        if not shutil.which(executable):
            raise RuntimeError(f"Missing required test dependency: {executable}")
    links = json.loads(command(["ip", "-j", "link", "show"]).stdout)
    if {link["ifname"] for link in links} != {"lo"}:
        raise RuntimeError("Refusing a non-empty network namespace")
    if Path("/var/run/magitrickle.sock").exists():
        raise RuntimeError("Refusing to replace an existing daemon Unix socket")
    if not Path(args.daemon).is_file():
        raise RuntimeError("Build the host production daemon before this test")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--daemon")
    parser.add_argument("--family", type=int, choices=(4, 6), default=4)
    parser.add_argument("--server", choices=("direct", "alternate"), help=argparse.SUPPRESS)
    parser.add_argument("--cert", help=argparse.SUPPRESS)
    parser.add_argument("--key", help=argparse.SUPPRESS)
    parser.add_argument("--ready", help=argparse.SUPPRESS)
    parser.add_argument("--no-udp", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--client", help=argparse.SUPPRESS)
    parser.add_argument("--mode", choices=("plain", "tls", "udp", "post-error", "banner"), default="plain", help=argparse.SUPPRESS)
    parser.add_argument("--requests", type=int, default=1, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.server:
        serve(args)
    elif args.client:
        client(args)
    elif not args.daemon:
        parser.error("--daemon is required")
    else:
        verify_isolation(args)
        with tempfile.TemporaryDirectory(prefix="mt-tproxy-kernel-") as directory:
            lab = Lab(args, directory)
            try:
                lab.configure()
                lab.test()
                print(f"PASS complete production TPROXY forwarding over IPv{args.family}", flush=True)
            except Exception:
                lab.diagnostics()
                raise
            finally:
                lab.close()


if __name__ == "__main__":
    main()
