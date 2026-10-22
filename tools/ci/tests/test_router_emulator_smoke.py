#!/usr/bin/env python3
"""Host-side contract tests for the OpenWrt router-emulator serial runner."""
import importlib.util
from pathlib import Path
import re
import subprocess
import socket
import tempfile
import threading
import unittest
import urllib.error
import urllib.request

SCRIPT = Path(__file__).resolve().parents[1] / 'router-emulator-smoke.py'
spec = importlib.util.spec_from_file_location('router_emulator_smoke', SCRIPT)
smoke = importlib.util.module_from_spec(spec)
spec.loader.exec_module(smoke)


class FakeProcess:
    returncode = None
    def poll(self):
        return None


class SerialTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        guest, host = socket.socketpair()
        self.guest, self.host = guest, host
        self.addCleanup(guest.close)
        self.addCleanup(host.close)
        self.console = smoke.SerialConsole(host, FakeProcess(), Path(self.tmp.name) / 'serial.log')
        self.addCleanup(self.console.close)
        self.console.ready = True

    def simulate_shell(self, status):
        def worker():
            pending = b''
            while True:
                incoming = self.guest.recv(4096)
                if not incoming:
                    return
                pending += incoming
                while b'\r' in pending:
                    cmd, pending = pending.split(b'\r', 1)
                    self.guest.sendall(cmd + b'\r\n')
                    if cmd.startswith(b'printf '):
                        marker = re.search(rb'(__MTC_CI_DONE_\d{4}__)', cmd).group(1)
                        self.guest.sendall(marker + b':' + str(status).encode() + b'\r\nroot@OpenWrt:~# ')
        t = threading.Thread(target=worker, daemon=True)
        t.start()
        return t

    def test_success_and_multiple_commands(self):
        self.simulate_shell(0)
        self.assertEqual(self.console.run('true', timeout=3), 0)
        self.assertEqual(self.console.run('echo second', timeout=3), 0)

    def test_failed_guest_command_fails_smoke(self):
        self.simulate_shell(7)
        with self.assertRaisesRegex(smoke.SmokeFailure, 'exit 7'):
            self.console.run('false', timeout=3)


class GuestPollTests(unittest.TestCase):
    def test_existing_file_keeps_interactive_shell_alive(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "ready"
            path.touch()
            # Test the same shell construct used by the guest runner.
            command = smoke.wait_for_guest_file_command("/tmp/mtci-ready", 2)
            command = command.replace("/tmp/mtci-ready", str(path))
            result = subprocess.run(
                ["sh", "-c", command + "; result=$?; echo SHELL_ALIVE; exit $result"],
                capture_output=True, text=True, timeout=5,
            )
            self.assertEqual(result.returncode, 0)
            self.assertIn("SHELL_ALIVE", result.stdout)

    def test_missing_file_fails_without_closing_shell(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "missing"
            command = smoke.wait_for_guest_file_command("/tmp/mtci-status", 1)
            command = command.replace("/tmp/mtci-status", str(path))
            result = subprocess.run(
                ["sh", "-c", command + "; result=$?; echo SHELL_ALIVE; exit $result"],
                capture_output=True, text=True, timeout=5,
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn("SHELL_ALIVE", result.stdout)


class PackageServerTests(unittest.TestCase):
    def test_serves_exact_apk_and_denies_unrelated_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / 'mt-c.apk'
            package.write_bytes(b'package-payload')
            with smoke.serve_package(package) as port:
                with urllib.request.urlopen(f'http://127.0.0.1:{port}/mt-c.apk') as response:
                    self.assertEqual(response.read(), b'package-payload')
                with self.assertRaises(urllib.error.HTTPError) as err:
                    urllib.request.urlopen(f'http://127.0.0.1:{port}/not-allowed')
                self.assertEqual(err.exception.code, 404)


if __name__ == '__main__':
    unittest.main()
