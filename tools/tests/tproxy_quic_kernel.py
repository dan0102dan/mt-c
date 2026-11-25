#!/usr/bin/env python3
"""Real HTTP/3 coverage for the production UDP TPROXY relay.

Run in a NEW network namespace with the CI-only aioquic==1.9.1 environment:
  sudo unshare --net /path/to/venv/bin/python tools/tests/tproxy_quic_kernel.py \
    --daemon /path/to/magitrickled-c --family 4

The existing TCP lab supplies isolated LAN/provider/VPN/guest networks. There
is no Internet traffic, fake QUIC handshake, or production QUIC dependency.
"""

import argparse
import asyncio
from functools import partial
import ipaddress
import json
from pathlib import Path
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace

from aioquic.asyncio import QuicConnectionProtocol, connect
from aioquic.asyncio.server import QuicServer
from aioquic.buffer import Buffer
from aioquic.h3.connection import H3_ALPN, H3Connection
from aioquic.h3.events import DataReceived, HeadersReceived
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.events import ConnectionTerminated, HandshakeCompleted
from aioquic.quic.packet import pull_quic_header

from tproxy_kernel import Lab, command, emit, endpoint, network, verify_isolation


SCRIPT = Path(__file__).resolve()
VERSIONS = {1: 0x00000001, 2: 0x6B3343CF}


def packet_types(data):
    """Decode public packet boundaries for fixture diagnostics only."""
    types = []
    offset = 0
    try:
        while offset < len(data):
            header = pull_quic_header(Buffer(data=data[offset:]), host_cid_length=8)
            if header.packet_length <= 0:
                raise ValueError("zero-length packet")
            types.append(header.packet_type.name)
            offset += header.packet_length
    except ValueError:
        types.append("OPAQUE")
    return types


class ResponseGate:
    """Suppress actual server responses without changing received QUIC data."""
    def __init__(self, transport, route, destination, directory, retry_only):
        self.transport = transport
        self.route = route
        self.destination = destination
        self.drop_file = Path(directory) / f"drop-{destination.replace(':', '_')}"
        self.retry_only = retry_only

    def sendto(self, data, addr=None):
        types = packet_types(data)
        dropped = self.route == "direct" and (
            self.drop_file.exists() or (self.retry_only and types != ["RETRY"]))
        emit("send", route=self.route, destination=self.destination,
             types=types, dropped=dropped)
        if not dropped:
            self.transport.sendto(data, addr)

    def __getattr__(self, name):
        return getattr(self.transport, name)


class H3Protocol(QuicConnectionProtocol):
    def __init__(self, *args, route=None, destination=None, directory=None, **kwargs):
        super().__init__(*args, **kwargs)
        self.http = H3Connection(self.quic_connection)
        self.route = route
        self.destination = destination
        self.directory = directory
        self.peer = None
        self.transport = None
        self.handshake_complete = False
        self.ready = asyncio.Event()
        self.requests = {}
        self.responses = {}
        self.waiters = {}
        self.deferred = set()

    def connection_made(self, transport):
        self.transport = transport
        super().connection_made(transport)

    def datagram_received(self, data, addr):
        self.peer = addr
        super().datagram_received(data, addr)

    def quic_event_received(self, event):
        if isinstance(event, HandshakeCompleted):
            self.handshake_complete = True
            self.ready.set()
            if self.route:
                emit("handshake", route=self.route, destination=self.destination,
                     early=event.early_data_accepted, resumed=event.session_resumed)
        if isinstance(event, ConnectionTerminated):
            self.ready.set()
            for task in self.deferred:
                task.cancel()
            for waiter in self.waiters.values():
                if not waiter.done():
                    waiter.set_exception(ConnectionError(event.reason_phrase))
        for message in self.http.handle_event(event):
            if self.route:
                self.receive_request(message)
            elif isinstance(message, HeadersReceived):
                self.responses.setdefault(message.stream_id, {"body": bytearray()})["headers"] = dict(message.headers)
                if message.stream_ended:
                    self.finish_response(message.stream_id)
            elif isinstance(message, DataReceived):
                self.responses.setdefault(message.stream_id, {"body": bytearray()})["body"].extend(message.data)
                if message.stream_ended:
                    self.finish_response(message.stream_id)

    def receive_request(self, message):
        if isinstance(message, HeadersReceived):
            headers = dict(message.headers)
            self.requests[message.stream_id] = {
                "path": headers[b":path"].decode(),
                "method": headers[b":method"].decode(),
            }
        if isinstance(message, (HeadersReceived, DataReceived)) and message.stream_ended:
            request = self.requests.pop(message.stream_id)
            body = {"route": self.route, "destination": self.destination,
                    "peer": self.peer[0], "peer_port": self.peer[1],
                    "early": not self.handshake_complete,
                    "connection": self.quic_connection.original_destination_connection_id.hex(),
                    **request}
            emit("request", **body)
            if request["path"] == "/server-first":
                task = asyncio.create_task(self.deferred_response(message.stream_id, body))
                self.deferred.add(task)
                task.add_done_callback(self.deferred.discard)
            else:
                self.send_response(message.stream_id, body)

    def send_response(self, stream, body):
        self.http.send_headers(stream, [(b":status", b"200"),
                                       (b"content-type", b"application/json")])
        self.http.send_data(stream, json.dumps(body).encode(), end_stream=True)
        self.transmit()

    async def deferred_response(self, stream, body):
        release = Path(self.directory) / f"release-{body['connection']}"
        while not release.exists():
            await asyncio.sleep(0.01)
        emit("server_first", **body)
        self.send_response(stream, body)

    def finish_response(self, stream):
        response = self.responses.pop(stream)
        waiter = self.waiters.pop(stream)
        if waiter.done():
            return
        if response.get("headers", {}).get(b":status") != b"200":
            waiter.set_exception(AssertionError("unexpected HTTP/3 status"))
        else:
            waiter.set_result(json.loads(response["body"]))

    async def request(self, path="/probe", method="GET"):
        stream = self.quic_connection.get_next_available_stream_id()
        waiter = asyncio.get_running_loop().create_future()
        self.waiters[stream] = waiter
        self.http.send_headers(stream, [(b":method", method.encode()), (b":scheme", b"https"),
                                        (b":authority", b"tproxy.test"), (b":path", path.encode())],
                               end_stream=method != "POST")
        if method == "POST":
            self.http.send_data(stream, b"action", end_stream=True)
        self.transmit()
        return await waiter


class InstrumentedServer(QuicServer):
    def __init__(self, *, route, destination, directory, retry_only=False, **kwargs):
        super().__init__(**kwargs)
        self.route = route
        self.destination = destination
        self.directory = directory
        self.retry_only = retry_only

    def connection_made(self, transport):
        super().connection_made(ResponseGate(transport, self.route, self.destination,
                                             self.directory, self.retry_only))

    def datagram_received(self, data, addr):
        try:
            cid = pull_quic_header(Buffer(data=data), host_cid_length=8).destination_cid.hex()
        except ValueError:
            cid = ""
        emit("receive", route=self.route, destination=self.destination,
             types=packet_types(data), dcid=cid,
             peer=addr[0], peer_port=addr[1])
        super().datagram_received(data, addr)


class EchoProtocol(asyncio.DatagramProtocol):
    def __init__(self, route, destination):
        self.route = route
        self.destination = destination

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        emit("opaque", route=self.route, destination=self.destination)
        self.transport.sendto(json.dumps({"route": self.route, "size": len(data)}).encode(), addr)


async def start_quic_server(args, destination, port=443, retry=False, retry_only=False):
    config = QuicConfiguration(is_client=False, alpn_protocols=H3_ALPN, idle_timeout=4)
    config.load_cert_chain(args.cert, args.key)
    tickets = {}
    def save_ticket(ticket):
        tickets[ticket.ticket] = ticket
    transport, server = await asyncio.get_running_loop().create_datagram_endpoint(
        lambda: InstrumentedServer(
            configuration=config, route=args.server, destination=destination,
            directory=args.directory, retry=retry, retry_only=retry_only,
            create_protocol=partial(H3Protocol, route=args.server, destination=destination,
                                    directory=args.directory),
            session_ticket_handler=save_ticket, session_ticket_fetcher=tickets.get),
        local_addr=(destination, port))
    return transport, server


async def serve(args):
    # TCP/443 at destination13 deliberately stalls on direct in the original
    # lab. Real QUIC on that same address must learn an independent direct route.
    numbers = [13] + [base + offset for base in (20, 30) for offset in (1, 2, 3, 4, 5, 7, 8)]
    listeners = []
    for number in numbers:
        offset = number % 10
        listeners.append(await start_quic_server(
            args, endpoint(args.family, number), retry=offset in (3, 4) and number != 13,
            retry_only=offset == 4))
    for number, port in ((15, 443), (26, 443), (26, 8443)):
        destination = endpoint(args.family, number)
        listeners.append(await asyncio.get_running_loop().create_datagram_endpoint(
            lambda d=destination: EchoProtocol(args.server, d), local_addr=(destination, port)))
    Path(args.ready).write_text("ready")
    try:
        await asyncio.Future()
    finally:
        for transport, _ in listeners:
            transport.close()


def client_config(args, ticket=None):
    config = QuicConfiguration(is_client=True, alpn_protocols=H3_ALPN,
                               server_name="tproxy.test", idle_timeout=4)
    config.supported_versions = [VERSIONS[args.version]]
    config.load_verify_locations(cafile=args.cert)
    config.session_ticket = ticket
    return config


async def exchange(args, *, ticket=None, tickets=None, action=False, hold=False,
                   rotate=False, server_first=False):
    config = client_config(args, ticket)
    local_port = getattr(args, "local_port", 0)
    if local_port and ticket is not None:
        local_port += 1
    async with connect(args.client, args.port, configuration=config, create_protocol=H3Protocol,
                       session_ticket_handler=tickets.append if tickets is not None else None,
                       wait_connected=False, local_port=local_port) as protocol:
        if ticket is None:
            # Own this cancellable wait so intentional handshake timeouts do
            # not leave aioquic's shielded wait_connected future unobserved.
            protocol.transmit()
            await protocol.ready.wait()
            if not protocol.handshake_complete:
                raise ConnectionError("QUIC handshake did not complete")
        response = await protocol.request("/action" if action else "/probe", "POST" if action else "GET")
        result = {"reply": response, "local_port": protocol.transport.get_extra_info("sockname")[1],
                  "handshake": protocol.handshake_complete}
        if hold or server_first:
            pending = asyncio.create_task(protocol.request("/server-first")) if server_first else None
            if pending:
                await asyncio.sleep(0)  # Send the request before reporting readiness.
            print(json.dumps(result), flush=True)
            if (await asyncio.to_thread(sys.stdin.readline)).strip() != "next":
                raise RuntimeError("missing continuation")
            if pending:
                result["server_reply"] = await pending
            protocol.change_connection_id()
            result["reply"] = await protocol.request("/after-clear")
        else:
            # Multiple streams exercise real established QUIC traffic, rather
            # than merely detecting a server's first UDP response.
            if rotate:
                protocol.change_connection_id()
            second = await protocol.request("/second")
            assert second["connection"] == response["connection"]
            assert second["peer_port"] == response["peer_port"]
        return result


async def client(args):
    tickets = []
    if args.early:
        primed = await asyncio.wait_for(exchange(args, tickets=tickets), 5)
        if not tickets or not tickets[-1].max_early_data_size:
            raise AssertionError("server did not issue a real 0-RTT session ticket")
        print(json.dumps({"primed": True, **primed}), flush=True)
        if (await asyncio.to_thread(sys.stdin.readline)).strip() != "early":
            raise RuntimeError("missing early-data continuation")
        try:
            await asyncio.wait_for(exchange(args, ticket=tickets[-1], action=True), 1.8)
        except (TimeoutError, ConnectionError):
            print(json.dumps({"timeout": True}), flush=True)
        else:
            raise AssertionError("suppressed 0-RTT response unexpectedly arrived")
    elif args.expected_timeout:
        try:
            await asyncio.wait_for(exchange(args), 1.8)
        except (TimeoutError, ConnectionError):
            print(json.dumps({"timeout": True}), flush=True)
        else:
            raise AssertionError("suppressed handshake unexpectedly succeeded")
    else:
        # A held client has its own bounded parent watchdog while waiting for
        # cache/conntrack changes; normal clients complete within five seconds.
        result = await asyncio.wait_for(exchange(args, hold=args.hold, server_first=args.server_first),
                                        15 if args.hold or args.server_first else 5)
        print(json.dumps(result), flush=True)


class QuicLab(Lab):
    def __init__(self, args, directory):
        super().__init__(args, directory)
        self.next_client_port = 24000

    def configure(self):
        super().configure()
        for name in ("direct", "alternate"):
            for number in range(21, 39):
                self.ns_command(name, "ip", self.ip_family, "addr", "add",
                                f"{endpoint(self.args.family, number)}/{32 if self.args.family == 4 else 128}",
                                "dev", "lo", *(["nodad"] if self.args.family == 6 else []))
            self.start_quic_server(name)
        # Blackhole the direct Initial before the server sees it: this tests a
        # real provider-path failure, not address migration of a live handshake.
        for number in (22, 27, 28, 32, 37, 38):
            self.ns_command("direct", self.tool, "-A", "INPUT", "-d", endpoint(self.args.family, number),
                            "-p", "udp", "--dport", "443", "-j", "DROP")
        settings = self.api()["settings"]
        settings.update(quicEnabled=True, maxConnections=64, maxEntries=128)
        assert self.api("PUT", {"settings": settings})["active"]

    def start_quic_server(self, name):
        ready = self.directory / f"{name}-quic.ready"
        handle = (self.directory / f"{name}-quic.log").open("w")
        self.handles.append(handle)
        process = subprocess.Popen(["nsenter", "--target", str(self.namespaces[name].pid), "--net", "--",
                                    sys.executable, "-u", str(SCRIPT), "--server", name,
                                    "--family", str(self.args.family), "--cert", str(self.directory / "cert.pem"),
                                    "--key", str(self.directory / "key.pem"), "--ready", str(ready),
                                    "--directory", str(self.directory)], stdout=handle, stderr=subprocess.STDOUT)
        self.processes.append(process)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if ready.exists():
                return
            if process.poll() is not None:
                break
            time.sleep(0.02)
        raise RuntimeError(f"QUIC fixture failed: {(self.directory / f'{name}-quic.log').read_text()}")

    def client_args(self, number, version, *extra):
        port = self.next_client_port
        self.next_client_port += 2
        return [sys.executable, "-u", str(SCRIPT), "--client", endpoint(self.args.family, number),
                "--version", version, "--cert", self.directory / "cert.pem",
                "--local-port", port, *extra]

    def quic(self, number, version, *extra, name="client"):
        result = self.ns_command(name, *self.client_args(number, version, *extra), timeout=12)
        return json.loads(result.stdout)

    def held_client(self, number, version, mode):
        process = subprocess.Popen(["nsenter", "--target", str(self.namespaces["client"].pid), "--net", "--",
                                    *map(str, self.client_args(number, version, mode))],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.processes.append(process)
        return process, self.read_result(process)

    def read_result(self, process):
        readable, _, _ = select.select([process.stdout], [], [], 10)
        if not readable:
            raise RuntimeError("QUIC client produced no result")
        line = process.stdout.readline()
        if not line:
            raise RuntimeError(f"QUIC client failed: {process.stderr.read()}")
        return json.loads(line)

    def resume(self, process, command_text):
        process.stdin.write(command_text + "\n")
        process.stdin.flush()
        result = self.read_result(process)
        if process.wait(timeout=5):
            raise RuntimeError(f"QUIC client failed: {process.stderr.read()}")
        return result

    def events(self, name, number):
        records = []
        for line in (self.directory / f"{name}-quic.log").read_text().splitlines():
            if line.startswith("{"):
                try:
                    event = json.loads(line)
                except json.JSONDecodeError:
                    continue  # A concurrent final log write may not be complete.
                if event.get("destination") == endpoint(self.args.family, number):
                    records.append(event)
        return records

    def no_alternate(self, number):
        assert not self.events("alternate", number), "ineligible traffic was sent to alternate"

    def wait_deferred(self, number, connection):
        # Wait for the real request plus ACK traffic to settle. The next server
        # response is controlled by a file, so no client retry needs to race the
        # conntrack deletion. A later assertion verifies its reverse direction.
        deadline = time.monotonic() + 5
        previous = -1
        quiet_since = time.monotonic()
        while time.monotonic() < deadline:
            events = self.events("alternate", number)
            if len(events) != previous:
                previous = len(events)
                quiet_since = time.monotonic()
            pending = any(event["event"] == "request" and event["path"] == "/server-first"
                          and event["connection"] == connection for event in events)
            if pending and time.monotonic() - quiet_since >= 0.15:
                return
            time.sleep(0.02)
        raise AssertionError("deferred HTTP/3 request did not settle")

    def opaque(self, number, port, name="client"):
        script = """import json,socket,sys
family=socket.AF_INET if sys.argv[3]=='4' else socket.AF_INET6
with socket.socket(family,socket.SOCK_DGRAM) as sock:
 sock.settimeout(3)
 sock.sendto(b'opaque-application-action',(sys.argv[1],int(sys.argv[2])))
 print(sock.recvfrom(4096)[0].decode())
"""
        result = self.ns_command(name, sys.executable, "-c", script, endpoint(self.args.family, number),
                                 port, self.args.family)
        return json.loads(result.stdout)

    def test(self):
        for version, base in ((1, 20), (2, 30)):
            before = self.accepted()
            result = self.quic(base + 1, version)
            assert result["handshake"] and result["reply"]["route"] == "direct", result
            assert self.accepted() == before + 1
            self.expect_entry(base + 1, 443, False, "udp")
            before = self.accepted()
            result = self.quic(base + 1, version)
            assert result["reply"]["route"] == "direct"
            assert ipaddress.ip_address(result["reply"]["peer"]) == ipaddress.ip_address(network(self.args.family, 0, 2))
            assert self.accepted() == before, "learned direct QUIC connection did not bypass relay"
            print(f"PASS QUICv{version} direct HTTP/3, multiple streams and kernel bypass", flush=True)

            before = self.accepted()
            result = self.quic(base + 2, version)
            assert result["handshake"] and result["reply"]["route"] == "alternate", result
            assert self.accepted() == before + 1, "QUIC fallback created a second client flow"
            self.expect_entry(base + 2, 443, True, "udp")
            before = self.accepted()
            assert self.quic(base + 2, version)["reply"]["route"] == "alternate"
            assert self.accepted() == before, "learned alternate QUIC connection did not bypass relay"
            print(f"PASS QUICv{version} real Initial fallback and learned alternate routing", flush=True)

            result = self.quic(base + 3, version)
            assert result["reply"]["route"] == "direct"
            assert any("RETRY" in event.get("types", []) for event in self.events("direct", base + 3))
            assert self.quic(base + 4, version, "--expected-timeout")["timeout"]
            self.no_alternate(base + 4)
            assert any(event["event"] == "send" and event["types"] == ["RETRY"]
                       and not event["dropped"] for event in self.events("direct", base + 4))
            assert not any(entry["address"] == endpoint(self.args.family, base + 4)
                           for entry in self.api()["entries"]), "Retry-only response was learned as success"
            print(f"PASS QUICv{version} Retry succeeds and Retry-then-silence never switches", flush=True)

            process, primed = self.held_client(base + 5, version, "--early")
            assert primed["primed"]
            self.api("DELETE", suffix="/cache")
            destination = endpoint(self.args.family, base + 5)
            (self.directory / f"drop-{destination.replace(':', '_')}").touch()
            assert self.resume(process, "early")["timeout"]
            events = self.events("direct", base + 5)
            actions = [event for event in events if event["event"] == "request" and event["path"] == "/action"]
            assert len(actions) == 1 and actions[0]["early"], "fixture did not execute real 0-RTT exactly once"
            assert any("ZERO_RTT" in event.get("types", []) for event in events), "no real 0-RTT packet observed"
            self.no_alternate(base + 5)
            print(f"PASS QUICv{version} real resumed 0-RTT action is never replayed on alternate", flush=True)

            process, first = self.held_client(base + 7, version, "--hold")
            assert first["reply"]["route"] == "alternate"
            previous_cids = {event["dcid"] for event in self.events("alternate", base + 7)
                             if event["event"] == "receive" and event["types"] == ["ONE_RTT"]}
            before = self.accepted()
            self.api("DELETE", suffix="/cache")
            command(["conntrack", "-D", "-f", "ipv4" if self.args.family == 4 else "ipv6", "-p", "udp",
                     "-s", network(self.args.family, 0, 2), "-d", endpoint(self.args.family, base + 7),
                     "--sport", first["local_port"], "--dport", "443"])
            second = self.resume(process, "next")
            assert second["reply"]["connection"] == first["reply"]["connection"]
            assert second["reply"]["peer"] == first["reply"]["peer"]
            assert second["reply"]["peer_port"] == first["reply"]["peer_port"], "live upstream socket changed"
            assert second["local_port"] == first["local_port"]
            current_cids = {event["dcid"] for event in self.events("alternate", base + 7)
                            if event["event"] == "receive" and event["types"] == ["ONE_RTT"]}
            assert current_cids - previous_cids, "client did not rotate its real QUIC connection ID"
            assert self.accepted() == before, "cache/conntrack deletion recaptured an existing QUIC session"
            print(f"PASS QUICv{version} CID rotation and cache/conntrack deletion preserve the session", flush=True)

            process, first = self.held_client(base + 8, version, "--server-first")
            assert first["reply"]["route"] == "alternate"
            self.wait_deferred(base + 8, first["reply"]["connection"])
            before = self.accepted()
            self.api("DELETE", suffix="/cache")
            destination = endpoint(self.args.family, base + 8)
            client_address = network(self.args.family, 0, 2)
            command(["conntrack", "-D", "-f", "ipv4" if self.args.family == 4 else "ipv6", "-p", "udp",
                     "-s", client_address, "-d", destination,
                     "--sport", first["local_port"], "--dport", "443"])
            (self.directory / f"release-{first['reply']['connection']}").touch()
            second = self.resume(process, "next")
            assert second["server_reply"]["connection"] == first["reply"]["connection"]
            assert second["reply"]["connection"] == first["reply"]["connection"]
            assert second["reply"]["peer"] == first["reply"]["peer"]
            assert second["reply"]["peer_port"] == first["reply"]["peer_port"]
            assert second["local_port"] == first["local_port"]
            reverse = command(["conntrack", "-L", "-f", "ipv4" if self.args.family == 4 else "ipv6",
                               "-p", "udp", "-s", destination, "-d", client_address,
                               "--sport", "443", "--dport", first["local_port"]])
            assert reverse.stdout.strip(), "server did not recreate conntrack in the reverse direction"
            assert self.accepted() == before, "reverse-first conntrack recaptured a live QUIC session"
            print(f"PASS QUICv{version} server-first conntrack recreation preserves the live socket", flush=True)

        self.api("DELETE", suffix="/cache")
        self.expect_route(self.request(13, mode="tls"), "alternate")
        assert self.quic(13, 1)["reply"]["route"] == "direct"
        self.expect_entry(13, 443, True, "tcp")
        self.expect_entry(13, 443, False, "udp")
        before = self.accepted()
        self.expect_route(self.request(13, mode="tls"), "alternate")
        assert self.quic(13, 2)["reply"]["route"] == "direct"
        assert self.accepted() == before
        print("PASS TCP/443 and UDP/443 learn distinct routes at the same destination", flush=True)

        assert self.opaque(26, 443)["route"] == "direct"
        self.no_alternate(26)
        assert not any(entry["address"] == endpoint(self.args.family, 26) for entry in self.api()["entries"])
        before = self.accepted()
        assert self.opaque(26, 8443)["route"] == "direct"
        assert self.opaque(15, 443)["route"] == "alternate"
        assert self.opaque(26, 443, name="outside")["route"] == "direct"
        assert self.accepted() == before, "non-443 UDP, manual route or excluded ingress was captured"
        print("PASS opaque UDP single path, non-443 UDP bypass, manual priority and excluded ingress", flush=True)

    def diagnostics(self):
        super().diagnostics()
        for name in ("direct", "alternate"):
            path = self.directory / f"{name}-quic.log"
            if path.exists():
                print(f"--- {name} QUIC log ---\n{path.read_text()[-18000:]}", file=sys.stderr)


async def self_test():
    """Validate the real peer fixture locally; this does not test TPROXY."""
    global emit
    original_emit = emit
    events = []
    emit = lambda event, **fields: events.append({"event": event, **fields})
    try:
        with tempfile.TemporaryDirectory(prefix="mt-quic-peer-") as directory:
            root = Path(directory)
            command(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                     "-subj", "/CN=tproxy.test", "-addext", "subjectAltName=DNS:tproxy.test",
                     "-keyout", root / "key.pem", "-out", root / "cert.pem"])
            args = SimpleNamespace(cert=str(root / "cert.pem"), key=str(root / "key.pem"),
                                   directory=directory, server="direct", client="127.0.0.1", version=1)
            for version in (1, 2):
                args.version = version
                events.clear()
                transport, server = await start_quic_server(args, args.client, port=0)
                args.port = transport.get_extra_info("sockname")[1]
                try:
                    tickets = []
                    first = await asyncio.wait_for(exchange(args, tickets=tickets, rotate=True), 5)
                    assert first["handshake"] and first["reply"]["route"] == "direct"
                    assert tickets and tickets[-1].max_early_data_size
                    cids = {event["dcid"] for event in events
                            if event["event"] == "receive" and event["types"] == ["ONE_RTT"]}
                    assert len(cids) >= 2, "fixture did not rotate connection ID"
                    print(f"PASS fixture QUICv{version}: verified TLS, HTTP/3 streams, CID rotation", flush=True)
                    async def delayed_response():
                        async with connect(args.client, args.port, configuration=client_config(args),
                                           create_protocol=H3Protocol) as protocol:
                            probe = await protocol.request()
                            pending = asyncio.create_task(protocol.request("/server-first"))
                            while not any(event["event"] == "request" and event["path"] == "/server-first"
                                          and event["connection"] == probe["connection"] for event in events):
                                await asyncio.sleep(0.01)
                            assert not pending.done(), "deferred server response arrived before release"
                            (root / f"release-{probe['connection']}").touch()
                            response = await pending
                            assert response["connection"] == probe["connection"]
                            protocol.change_connection_id()
                            second = await protocol.request("/after-clear")
                            assert second["peer_port"] == probe["peer_port"]
                    await asyncio.wait_for(delayed_response(), 5)
                    print(f"PASS fixture QUICv{version}: controlled server-first response and continued session", flush=True)
                    drop = root / "drop-127.0.0.1"
                    drop.touch()
                    try:
                        await asyncio.wait_for(exchange(args, ticket=tickets[-1], action=True), 1.8)
                    except (TimeoutError, ConnectionError):
                        pass
                    else:
                        raise AssertionError("0-RTT server response was not suppressed")
                    actions = [event for event in events
                               if event["event"] == "request" and event["path"] == "/action"]
                    assert len(actions) == 1 and actions[0]["early"]
                    assert any("ZERO_RTT" in event.get("types", []) for event in events)
                    print(f"PASS fixture QUICv{version}: one real 0-RTT action with all responses dropped", flush=True)
                finally:
                    (root / "drop-127.0.0.1").unlink(missing_ok=True)
                    server.close()
                    await asyncio.sleep(0)
                for retry_only in (False, True):
                    events.clear()
                    transport, server = await start_quic_server(args, args.client, port=0,
                                                                retry=True, retry_only=retry_only)
                    args.port = transport.get_extra_info("sockname")[1]
                    try:
                        try:
                            result = await asyncio.wait_for(exchange(args), 1.8 if retry_only else 5)
                        except (TimeoutError, ConnectionError):
                            if not retry_only:
                                raise
                        else:
                            assert not retry_only and result["handshake"]
                        assert any(event["event"] == "send" and event["types"] == ["RETRY"]
                                   and not event["dropped"] for event in events)
                        if retry_only:
                            assert not any(event["event"] == "request" for event in events)
                        print(f"PASS fixture QUICv{version}: Retry {'then silence' if retry_only else 'and HTTP/3'}", flush=True)
                    finally:
                        server.close()
                        await asyncio.sleep(0)
    finally:
        emit = original_emit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--daemon")
    parser.add_argument("--self-test", action="store_true", help="check the real QUIC peer fixture on loopback, without TPROXY")
    parser.add_argument("--family", type=int, choices=(4, 6), default=4)
    parser.add_argument("--server", choices=("direct", "alternate"))
    parser.add_argument("--directory")
    parser.add_argument("--cert")
    parser.add_argument("--key")
    parser.add_argument("--ready")
    parser.add_argument("--client")
    parser.add_argument("--port", type=int, default=443)
    parser.add_argument("--local-port", type=int, default=0)
    parser.add_argument("--version", type=int, choices=(1, 2), default=1)
    parser.add_argument("--hold", action="store_true")
    parser.add_argument("--server-first", action="store_true")
    parser.add_argument("--early", action="store_true")
    parser.add_argument("--expected-timeout", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        asyncio.run(self_test())
    elif args.server:
        asyncio.run(serve(args))
    elif args.client:
        asyncio.run(client(args))
    else:
        if not args.daemon:
            parser.error("--daemon is required")
        verify_isolation(args)
        if not shutil.which("conntrack"):
            raise RuntimeError("Missing required test dependency: conntrack")
        args.no_udp = True
        with tempfile.TemporaryDirectory(prefix="mt-quic-kernel-") as directory:
            lab = QuicLab(args, directory)
            try:
                lab.configure()
                lab.test()
                print(f"PASS complete real HTTP/3 TPROXY over IPv{args.family}", flush=True)
            except Exception:
                lab.diagnostics()
                raise
            finally:
                lab.close()


if __name__ == "__main__":
    main()
