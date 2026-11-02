#!/usr/bin/env python3
"""Hermetic checks for comparison gates; no QEMU, router or public network.

The asset server uses ephemeral loopback sockets. Upstream downloads are
controlled byte fixtures, and binary identity checks run the actual shell
command with real sha256sum over files in a temporary fake filesystem.
"""
from contextlib import redirect_stdout
import copy
import hashlib
import http.client
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import unittest
from unittest.mock import MagicMock, patch


SCRIPT = Path(__file__).resolve().parents[1] / "router-emulator-compare.py"
SPEC = importlib.util.spec_from_file_location("router_compare", SCRIPT)
compare = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(compare)


class SerialFixture:
    def __init__(self, text):
        self.recent = text.encode()
        self.commands = []

    def run(self, command, **kwargs):
        self.commands.append(command)


class WireAndKernelTests(unittest.TestCase):
    def response(self, aaaa=False, typ="A"):
        name = "d000000.bench.example.com."
        records = [
            dict(name=name, type=5, **{"class": 1}, ttl=300, value="c0." + name),
            dict(name="c0." + name, type=5, **{"class": 1}, ttl=300, value="c1." + name),
        ]
        if typ == "A":
            records += [dict(name="c1." + name, type=1, **{"class": 1}, ttl=300,
                             value=f"10.99.0.{i}") for i in range(1, 5)]
        if aaaa:
            records += [dict(name="c1." + name, type=28, **{"class": 1}, ttl=300,
                             value=f"fd00::{i}") for i in range(1, 5)]
        return dict(records=records, elapsed_seconds=0.001)

    def test_exact_a_and_aaaa_answers_are_accepted(self):
        compare.check_records(self.response())
        compare.check_records(self.response(aaaa=True), aaaa=True)
        compare.check_records(self.response(aaaa=True, typ="AAAA"), aaaa=True, typ="AAAA")

    def test_wire_value_header_order_and_count_mismatches_fail(self):
        cases = []
        for field, wrong in (("name", "wrong."), ("type", 28), ("class", 3),
                             ("ttl", 3900), ("value", "10.99.0.99")):
            result = self.response()
            result["records"][2][field] = wrong
            cases.append((field, result))
        result = self.response(); result["records"][0]["value"] = "wrong-target."
        cases.append(("cname target", result))
        result = self.response(); result["records"].reverse(); cases.append(("order", result))
        result = self.response(); result["records"].pop(); cases.append(("missing", result))
        result = self.response(); result["records"].append(copy.deepcopy(result["records"][-1]))
        cases.append(("duplicate", result))
        result = self.response(); result["error"] = "timeout"; cases.append(("network failure", result))
        for label, result in cases:
            with self.subTest(label=label), self.assertRaises(RuntimeError):
                compare.check_records(result)

    def test_query_accepts_one_json_result_and_preserves_arguments(self):
        result = self.response()
        serial = SerialFixture("shell echo\r\n" + json.dumps(result) + "\r\nroot@guest:#\r\n")
        self.assertEqual(compare.query(serial, "tcp", "a'; echo wrong; #.test", "AAAA"), result)
        words = compare.shlex.split(serial.commands[0])
        self.assertEqual(words[words.index("-name") + 1], "a'; echo wrong; #.test")
        self.assertEqual(words[words.index("-proto") + 1], "tcp")
        self.assertEqual(words[words.index("-type") + 1], "AAAA")

    def test_missing_or_ambiguous_json_result_is_rejected(self):
        for text in ("root@guest:#\r\n", "{}\r\n{}\r\n", "{broken\n"):
            with self.subTest(text=text), self.assertRaises((RuntimeError, ValueError)):
                compare.query(SerialFixture(text))

    def sets_text(self, ttl=3900, groups=2, addresses=4):
        lines = ["create unrelated hash:net family inet", "add unrelated 203.0.113.1 timeout 2"]
        for group in range(1, groups + 1):
            for family in (4, 6):
                name = f"mt_aa{group:06x}_{family}"
                lines.append(f"create {name} hash:net family inet{6 if family == 6 else ''} timeout 300")
                for i in range(1, addresses + 1):
                    ip = f"10.99.0.{i}" if family == 4 else f"fd00:0:0:0:0:0:0:{i}"
                    lines.append(f"add {name} {ip} timeout {ttl}")
        return "\n".join(lines)

    def test_kernel_membership_and_remaining_ttl_gate(self):
        text = self.sets_text()
        result = compare.check_sets(SerialFixture(text))
        self.assertEqual(result["mt_aa000001_6"][0], dict(address="fd00::1", ttl=3900))
        compare.check_sets(SerialFixture(self.sets_text(ttl=3630)), ttl=3660)
        compare.check_sets(SerialFixture(self.sets_text(ttl=3690)), ttl=3660)
        for text in (self.sets_text(ttl=3900), self.sets_text(ttl=0),
                     self.sets_text(ttl=3629), self.sets_text(ttl=3691)):
            with self.subTest(text=text[-60:]), self.assertRaises(RuntimeError):
                compare.check_sets(SerialFixture(text), ttl=3660)

    def test_missing_wrong_family_duplicate_and_extra_members_fail(self):
        good = self.sets_text()
        bad = [good.replace("add mt_aa000001_4 10.99.0.1 timeout 3900\n", ""),
               good.replace("add mt_aa000001_4 10.99.0.1", "add mt_aa000001_4 fd00::1"),
               good + "\nadd mt_aa000001_4 10.99.0.1 timeout 3900",
               good + "\nadd mt_aa000001_6 fd00::9 timeout 3900",
               good.replace("mt_aa000002_6", "unrelated6")]
        for text in bad:
            with self.subTest(text=text[-60:]), self.assertRaises(RuntimeError):
                compare.check_sets(SerialFixture(text))

    def test_unmatched_control_cannot_hide_permanent_entries(self):
        empty = self.sets_text(addresses=0)
        compare.check_sets(SerialFixture(empty), addresses=0)
        for suffix in ("", " timeout 0"):
            text = empty + "\nadd mt_aa000001_4 10.99.0.1" + suffix
            with self.subTest(suffix=suffix), self.assertRaises(RuntimeError):
                compare.check_sets(SerialFixture(text), addresses=0)


class UpstreamBytes(io.BytesIO):
    def __init__(self, data, declared_length=None):
        super().__init__(data)
        self.headers = {"Content-Length": str(len(data) if declared_length is None else declared_length)}


class AssetServerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.cache = self.root / "cache"
        self.key = "releases/25.12.5/packages/aarch64_cortex-a53/base/packages.adb"

    def get(self, port, path):
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        try:
            conn.request("GET", path)
            response = conn.getresponse()
            return response.status, response.read(), response.getheader("Content-Length")
        finally:
            conn.close()

    def test_release_paths_are_scoped_and_traversal_is_rejected(self):
        for key in (self.key, "releases/24.10.4/targets/mediatek/filogic/packages/libc_1-r0_aarch64.ipk",
                    "releases/24.10.4/packages/aarch64_cortex-a53/base/uhttpd_2025.07.06~7e64e8ba-r5_aarch64_cortex-a53.ipk"):
            self.assertEqual(compare.mirror_key("/openwrt/" + key), key)
        for path in ("/etc/passwd", "/openwrt/snapshots/packages/x", "/openwrt/releases/25.12.4/x",
                     "/openwrt/" + self.key + "?mirror=evil", "/openwrt/" + self.key + "#fragment",
                     "https://evil.invalid/openwrt/" + self.key, "//evil.invalid/openwrt/" + self.key,
                     "/openwrt/releases/25.12.5/../private", "/openwrt/releases/25.12.5/%2e%2e/private",
                     "/openwrt/releases/25.12.5/%252e%252e/private", "/openwrt/releases/25.12.5/a//b",
                     "/openwrt/releases/25.12.5/%2fetc/passwd", "/openwrt/releases/25.12.5/a%00b"):
            with self.subTest(path=path):
                self.assertIsNone(compare.mirror_key(path))

    def test_local_assets_have_exact_bytes_and_an_explicit_allowlist(self):
        asset = self.root / "fixture.apk"; payload = b"\x00signed-package\xff\n"; asset.write_bytes(payload)
        with patch.object(compare.urllib.request, "urlopen") as upstream, redirect_stdout(io.StringIO()), \
                compare.serve_assets({"/candidate.apk": asset}, self.cache) as port:
            status, body, length = self.get(port, "/candidate.apk")
            self.assertEqual((status, body, int(length)), (200, payload, len(payload)))
            self.assertEqual(self.get(port, "/candidate.apk?anything")[0], 404)
            self.assertEqual(self.get(port, "/fixture.apk")[0], 404)
            upstream.assert_not_called()

    def test_release_mirror_preserves_signed_bytes_and_caches_once(self):
        payload = b"ADB\x00\xffsigned-index\x00signature-bytes"
        with patch.object(compare.urllib.request, "urlopen", return_value=UpstreamBytes(payload)) as upstream, \
                redirect_stdout(io.StringIO()), compare.serve_assets({}, self.cache) as port:
            self.assertEqual(self.get(port, "/openwrt/" + self.key)[:2], (200, payload))
            self.assertEqual(self.get(port, "/openwrt/" + self.key)[:2], (200, payload))
            upstream.assert_called_once_with("https://downloads.openwrt.org/" + self.key, timeout=120)
        self.assertEqual((self.cache / self.key).read_bytes(), payload)

    def test_partial_download_is_not_cached_and_retry_can_succeed(self):
        payload = b"complete-signed-data"
        with patch.object(compare.urllib.request, "urlopen", side_effect=[UpstreamBytes(b"short", 100), UpstreamBytes(payload)]) as upstream, \
                redirect_stdout(io.StringIO()), compare.serve_assets({}, self.cache) as port:
            self.assertEqual(self.get(port, "/openwrt/" + self.key)[0], 502)
            self.assertFalse((self.cache / self.key).exists())
            self.assertEqual([p for p in self.cache.rglob("*") if p.is_file()], [])
            self.assertEqual(self.get(port, "/openwrt/" + self.key)[:2], (200, payload))
            self.assertEqual(upstream.call_count, 2)


class BinaryIdentityTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bin = self.root / "bin"; self.bin.mkdir()
        for name, data in (("baseline", b"old-executable"), ("candidate", b"new-executable"),
                           ("installed", b"new-executable"), ("running", b"new-executable")):
            (self.root / name).write_bytes(data)
        sha = shutil.which("sha256sum")
        self.assertIsNotNone(sha)
        script = '''#!/bin/sh
case "$1" in
 /tmp/mt-c-baseline) name=baseline;;
 /tmp/mt-c-candidate) name=candidate;;
 /usr/bin/magitrickled) name=installed;;
 /proc/42/exe) name=running;;
 *) exit 1;;
esac
test "$IDENTITY_FAIL" != "$name" || exit 1
exec "$REAL_SHA256SUM" "$IDENTITY_ROOT/$name"
'''
        (self.bin / "sha256sum").write_text(script)
        (self.bin / "pidof").write_text('#!/bin/sh\nprintf "%s\\n" "$IDENTITY_PID"\n')
        for path in self.bin.iterdir(): path.chmod(0o755)
        self.env = {**os.environ, "PATH": str(self.bin) + os.pathsep + os.defpath,
                    "IDENTITY_ROOT": str(self.root), "REAL_SHA256SUM": sha,
                    "IDENTITY_PID": "42", "IDENTITY_FAIL": ""}

    def verify(self, variant="candidate"):
        parent = self
        class ShellSerial:
            recent = b""
            def run(self, command, **kwargs):
                proc = subprocess.run(command, shell=True, env=parent.env, capture_output=True)
                self.recent = proc.stdout + proc.stderr
                if proc.returncode:
                    raise RuntimeError("identity command failed")
        return compare.verify_running_binary(ShellSerial(), variant)

    def test_identity_checks_installed_and_running_bytes(self):
        expected = hashlib.sha256(b"new-executable").hexdigest()
        self.assertEqual(self.verify(), dict(sha256=expected, pid=42))
        for name in ("installed", "running"):
            with self.subTest(file=name):
                (self.root / name).write_bytes(b"old-executable")
                with self.assertRaises(RuntimeError): self.verify()
                (self.root / name).write_bytes(b"new-executable")

    def test_missing_hash_or_process_cannot_self_validate(self):
        for name in ("candidate", "installed", "running"):
            with self.subTest(failed_hash=name):
                self.env["IDENTITY_FAIL"] = name
                with self.assertRaises(RuntimeError): self.verify()
        self.env["IDENTITY_FAIL"] = ""
        for pid in ("", "42 43", "0", "not-a-pid"):
            with self.subTest(pid=pid):
                self.env["IDENTITY_PID"] = pid
                with self.assertRaises(RuntimeError): self.verify()

    def test_unknown_variant_and_missing_identity_line_fail_closed(self):
        with self.assertRaises(ValueError): compare.identity_command("candidate; echo bypass")
        with self.assertRaises(RuntimeError): compare.verify_running_binary(SerialFixture("root@guest:#\n"), "candidate")


class CleanupTests(unittest.TestCase):
    def test_orphaned_emulator_group_is_killed_even_if_launcher_has_exited(self):
        process = MagicMock(); process.pid = 43210; process.wait.return_value = 0
        sock = MagicMock()
        sock.__enter__.return_value.connect.side_effect = FileNotFoundError
        with patch.object(compare.socket, "socket", return_value=sock), \
                patch.object(compare.os, "killpg", side_effect=[None, ProcessLookupError]) as kill:
            compare.stop_emulator(process, Path("/missing/owned-monitor.sock"))
        self.assertEqual([call.args for call in kill.call_args_list],
                         [(43210, signal.SIGTERM), (43210, signal.SIGKILL)])
        process.kill.assert_not_called()


class BaselineSmokeReuseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        digest = lambda value: hashlib.sha256(value.encode()).hexdigest()
        self.previous = dict(
            completed=True,
            parameters=dict(openwrt_version="25.12.5", preset="cudy-wr3000p-v1"),
            installed_daemon_sha256=dict(baseline=digest("baseline binary")),
            build_provenance=dict(baseline=dict(package_binary_sha256=digest("baseline binary"))),
            qemu_binary_sha256=digest("qemu"), preset_sha256=digest("preset"),
            official_firmware_sha256={"firmware/25.12.5/recovery.itb": digest("firmware")},
            helper_sha256={"/old/location/emulator.sh": digest("launcher")},
            asset_sha256={"/dhcp.sh": digest("DHCP fixture"), "/routing.sh": digest("routing fixture")},
        )
        self.current = copy.deepcopy(self.previous)
        self.current.pop("completed")
        self.current["installed_daemon_sha256"] = {}
        self.current["helper_sha256"] = {"/new/location/emulator.sh": digest("launcher")}
        self.current["build_provenance"]["candidate"] = dict(package_binary_sha256=digest("new candidate"))
        self.markers = {
            "dhcp": "PASS: device name rediscovered after closing and reopening the journal",
            "routing": "PASS: all six rule types forwarded real client traffic and stopped after deletion",
        }
        for name, marker in self.markers.items():
            (self.root / f"baseline-{name}.log").write_text("guest fixture output\n[mtci] " + marker + "\n")
        self.save_previous(self.previous)

    def save_previous(self, metadata):
        (self.root / "metadata.json").write_text(json.dumps(metadata) + "\n")

    def changed(self, path, value, current=False):
        metadata = copy.deepcopy(self.current if current else self.previous)
        target = metadata
        for key in path[:-1]: target = target[key]
        if value is None:
            target.pop(path[-1], None)
        else:
            target[path[-1]] = value
        return metadata

    def test_matching_completed_proof_is_read_only_and_returns_actual_hashes(self):
        before = {p.name: p.read_bytes() for p in self.root.iterdir()}
        with patch.object(compare, "guest") as guest, patch.object(compare.subprocess, "run") as run:
            proof = compare.validate_baseline_smoke_reuse(self.root, self.current)
            guest.assert_not_called(); run.assert_not_called()
        self.assertEqual(proof["directory"], str(self.root))
        self.assertEqual(proof["baseline_binary_sha256"], self.previous["installed_daemon_sha256"]["baseline"])
        self.assertEqual(proof["metadata_sha256"], hashlib.sha256(before["metadata.json"]).hexdigest())
        self.assertEqual(proof["log_sha256"], {
            str(self.root / f"baseline-{name}.log"): hashlib.sha256(before[f"baseline-{name}.log"]).hexdigest()
            for name in self.markers
        })
        self.assertEqual({p.name: p.read_bytes() for p in self.root.iterdir()}, before)

    def test_partial_or_unmarked_previous_run_is_not_reused(self):
        for value in (False, None, 0, 1, "true"):
            with self.subTest(completed=value):
                self.save_previous(self.changed(("completed",), value))
                with self.assertRaises(RuntimeError):
                    compare.validate_baseline_smoke_reuse(self.root, self.current)

    def test_both_previous_binary_identities_must_match_current_baseline(self):
        for path in (("installed_daemon_sha256", "baseline"),
                     ("build_provenance", "baseline", "package_binary_sha256")):
            for value in ("f" * 64, None):
                with self.subTest(path=path, value=value):
                    self.save_previous(self.changed(path, value))
                    with self.assertRaises(RuntimeError):
                        compare.validate_baseline_smoke_reuse(self.root, self.current)
        self.save_previous(self.previous)
        current = self.changed(("build_provenance", "baseline", "package_binary_sha256"), "e" * 64, current=True)
        with self.assertRaises(RuntimeError):
            compare.validate_baseline_smoke_reuse(self.root, current)

    def test_runtime_firmware_and_unique_launcher_must_match(self):
        differences = [
            (("parameters", "openwrt_version"), "24.10.4"),
            (("parameters", "preset"), "different-board"),
            (("qemu_binary_sha256",), "a" * 64), (("preset_sha256",), "b" * 64),
            (("official_firmware_sha256",), {"firmware/25.12.5/recovery.itb": "c" * 64}),
            (("official_firmware_sha256",), {}), (("qemu_binary_sha256",), ""),
            (("preset_sha256",), None),
            (("helper_sha256",), {}),
            (("helper_sha256",), {"/old/emulator.sh": "d" * 64}),
            (("helper_sha256",), {"/one/emulator.sh": "d" * 64, "/two/emulator.sh": "d" * 64}),
        ]
        for path, value in differences:
            with self.subTest(path=path, value=value):
                self.save_previous(self.changed(path, value))
                with self.assertRaises(RuntimeError):
                    compare.validate_baseline_smoke_reuse(self.root, self.current)
        # Equal absence is not evidence of a known runtime.
        for key in ("qemu_binary_sha256", "preset_sha256", "official_firmware_sha256"):
            with self.subTest(empty_both=key):
                previous = self.changed((key,), {})
                current = self.changed((key,), {}, current=True)
                self.save_previous(previous)
                with self.assertRaises(RuntimeError):
                    compare.validate_baseline_smoke_reuse(self.root, current)
        self.save_previous(self.previous)
        current = self.changed(("helper_sha256",), {}, current=True)
        with self.assertRaises(RuntimeError):
            compare.validate_baseline_smoke_reuse(self.root, current)

    def test_changed_or_missing_served_scripts_are_not_reused(self):
        for name in ("/dhcp.sh", "/routing.sh"):
            for value in ("a" * 64, None):
                with self.subTest(asset=name, value=value):
                    self.save_previous(self.changed(("asset_sha256", name), value))
                    with self.assertRaises(RuntimeError):
                        compare.validate_baseline_smoke_reuse(self.root, self.current)

    def test_each_baseline_log_requires_its_final_pass_marker(self):
        for name, marker in self.markers.items():
            path = self.root / f"baseline-{name}.log"
            original = path.read_bytes()
            for content in ("", "PASS: an earlier step succeeded\n", marker[:-1]):
                with self.subTest(log=name, content=content):
                    path.write_text(content)
                    with self.assertRaises(RuntimeError):
                        compare.validate_baseline_smoke_reuse(self.root, self.current)
            path.unlink()
            with self.subTest(missing=name), self.assertRaises(OSError):
                compare.validate_baseline_smoke_reuse(self.root, self.current)
            path.write_bytes(original)


class GuestShellPortabilityTests(unittest.TestCase):
    def test_prepare_rewrites_official_and_previous_mirrors_with_real_sed(self):
        source = (
            "src/gz official https://downloads.openwrt.org/releases/25.12.5/packages/base\n"
            "src/gz previous http://10.0.2.2:31234/openwrt/releases/25.12.5/packages/luci\n"
            "src/gz external https://packages.example.org/repository\n"
            "src/gz other-host http://10.0.2.3:31234/openwrt/releases/25.12.5/packages/base\n"
            "src/gz other-path http://10.0.2.2:31234/private/repository\n"
            "src/gz lookalike http://10x0x2x2:31234/openwrt/releases/25.12.5/packages/base\n"
        )
        for manager, version in (("opkg", "24.10.4"), ("apk", "25.12.5")):
            with self.subTest(manager=manager), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                paths = (["etc/opkg/distfeeds.conf"] if manager == "opkg" else
                         ["etc/apk/repositories", "etc/apk/repositories.d/distfeeds.list"])
                for name in paths:
                    path = root / name; path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(source)
                untouched = root / "etc/apk/repositories.d/not-a-list.txt"
                untouched.parent.mkdir(parents=True, exist_ok=True)
                untouched.write_text(source)
                # Execute the generated shell/sed, substituting only the fake
                # filesystem root. No guest date, route or package command runs.
                for port in (41337, 41338):
                    serial = SerialFixture("")
                    with patch.object(compare.SMOKE, "run_package_command"):
                        compare.prepare(serial, port, manager, version)
                    scripts = [compare.shlex.split(command) for command in serial.commands
                               if command.startswith("sh -ec ")]
                    self.assertEqual(len(scripts), 1)
                    script = scripts[0][2].replace("/etc/", str(root / "etc") + "/")
                    subprocess.run(["sh", "-ec", script], check=True, capture_output=True, timeout=5)
                    expected = (
                        f"src/gz official http://10.0.2.2:{port}/openwrt/releases/25.12.5/packages/base\n"
                        f"src/gz previous http://10.0.2.2:{port}/openwrt/releases/25.12.5/packages/luci\n"
                        "src/gz external https://packages.example.org/repository\n"
                        "src/gz other-host http://10.0.2.3:31234/openwrt/releases/25.12.5/packages/base\n"
                        "src/gz other-path http://10.0.2.2:31234/private/repository\n"
                        "src/gz lookalike http://10x0x2x2:31234/openwrt/releases/25.12.5/packages/base\n"
                    )
                    for name in paths:
                        self.assertEqual((root / name).read_text(), expected)
                    self.assertEqual(untouched.read_text(), source)

    def test_stop_wait_uses_integer_sleep_and_requires_the_process_to_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            commands = {
                "sleep": '''#!/bin/sh
test "$#" -eq 1 || exit 64
case "$1" in ''|*[!0-9]*) exit 65;; esac
printf '%s\\n' "$1" >> "$WAIT_ROOT/sleeps"
''',
                "pidof": '''#!/bin/sh
test "$1" = magitrickled || exit 64
count=$(cat "$WAIT_ROOT/polls")
count=$((count + 1))
printf '%s\\n' "$count" > "$WAIT_ROOT/polls"
if test "$count" -le "$WAIT_LIVE_POLLS"; then printf '42\\n'; exit 0; fi
exit 1
''',
                "magitrickle-init": '''#!/bin/sh
test "$1" = stop || exit 64
printf '%s\\n' stop >> "$WAIT_ROOT/stops"
''',
            }
            for name, content in commands.items():
                path = root / name; path.write_text(content); path.chmod(0o755)
            serial = SerialFixture("")
            compare.stop_service(serial)
            self.assertEqual(len(serial.commands), 1)
            command = serial.commands[0].replace("/etc/init.d/magitrickle", str(root / "magitrickle-init"))
            env = {**os.environ, "PATH": str(root) + os.pathsep + os.defpath,
                   "WAIT_ROOT": str(root), "WAIT_LIVE_POLLS": "2"}
            (root / "polls").write_text("0\n")
            subprocess.run(command, shell=True, env=env, check=True, capture_output=True, timeout=5)
            self.assertEqual((root / "stops").read_text(), "stop\n")
            sleeps = (root / "sleeps").read_text().splitlines()
            self.assertTrue(sleeps)
            self.assertTrue(all(value.isdecimal() and int(value) > 0 for value in sleeps))
            self.assertGreater(int((root / "polls").read_text()), 2)
            # A still-running process must make the stop gate fail. The fake
            # sleep validates BusyBox's integer-only contract without waiting.
            (root / "polls").write_text("0\n")
            env["WAIT_LIVE_POLLS"] = "1000000"
            result = subprocess.run(command, shell=True, env=env, capture_output=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
