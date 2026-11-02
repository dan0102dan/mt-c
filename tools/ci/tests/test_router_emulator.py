#!/usr/bin/env python3
"""Host orchestration checks; these do not boot OpenWrt or test routing."""

from contextlib import ExitStack, nullcontext, redirect_stdout
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import MagicMock, patch
import urllib.error
import urllib.request


SCRIPT = Path(__file__).resolve().parents[1] / "router-emulator-smoke.py"
SPEC = importlib.util.spec_from_file_location("router_smoke", SCRIPT)
smoke = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(smoke)


class RouterEmulatorTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.package = self.directory / "mt-c.ipk"
        self.package.write_bytes(b"the actual cross-built package")
        self.emulator = self.directory / "emulator"
        self.emulator.mkdir()
        (self.emulator / "emulator.sh").touch()

    def run_guest(self, version, fail_update=False, fail_install=False, fail_clock_times=0):
        serial = MagicMock()
        clock_calls = 0

        def run(command, **kwargs):
            nonlocal clock_calls
            if command.startswith("date -u -s "):
                clock_calls += 1
                return 1 if clock_calls <= fail_clock_times else 0
            if command in ("opkg update", "apk update"):
                return 1 if fail_update else 0
            if fail_install and command.startswith("opkg install /tmp/"):
                return 1
            return 0

        serial.run.side_effect = run
        with ExitStack() as stack:
            stack.enter_context(redirect_stdout(io.StringIO()))
            stack.enter_context(patch.object(smoke, "serve_package", return_value=nullcontext(12345)))
            stack.enter_context(patch.object(smoke.subprocess, "Popen"))
            stack.enter_context(patch.object(smoke, "connect_console"))
            stack.enter_context(patch.object(smoke, "SerialConsole", return_value=serial))
            api = stack.enter_context(patch.object(smoke, "check_http_api"))
            stop = stack.enter_context(patch.object(smoke, "stop_emulator"))
            stack.enter_context(patch.object(smoke.time, "sleep"))
            try:
                smoke.run_test(self.emulator, self.package, "cudy-wr3000p-v1",
                               self.directory / "logs", version)
            finally:
                self.assertTrue(stop.called)
                serial.close.assert_called_once()
                self.commands = [call.args[0] for call in serial.run.call_args_list]
                self.api_called = api.called

    def test_ipk_installs_with_opkg_before_testing_backend(self):
        self.run_guest("24.10.4")
        self.assertIn("opkg install /tmp/mt-c-ci.ipk", self.commands)
        self.assertIn("opkg list-installed mt-c | grep -q '^mt-c - '", self.commands)
        self.assertFalse(any(command.startswith("apk ") for command in self.commands))
        self.assertTrue(any("/mt-c.ipk" in command for command in self.commands))
        self.assertTrue(any("DISTRIB_RELEASE='24.10.4'" in command for command in self.commands))
        self.assertTrue(self.api_called)
        self.assertIn('test "$(cat /tmp/mtci-dhcp-status)" = 0', self.commands)
        self.assertIn('test "$(cat /tmp/mtci-status)" = 0', self.commands)

    def test_apk_still_installs_with_apk(self):
        self.package = self.package.rename(self.package.with_suffix(".apk"))
        self.run_guest("25.12.5")
        self.assertIn("apk add --allow-untrusted /tmp/mt-c-ci.apk", self.commands)
        self.assertIn("apk info -e mt-c", self.commands)
        self.assertFalse(any(command.startswith("opkg ") for command in self.commands))
        self.assertTrue(self.api_called)

    def test_guest_boot_readiness_precedes_clock_and_https(self):
        self.package = self.package.rename(self.package.with_suffix(".apk"))
        self.run_guest("25.12.5")
        route_index = next(i for i, cmd in enumerate(self.commands)
                           if cmd.startswith("for i in $(seq 1 30)"))
        boot_index = next(i for i, cmd in enumerate(self.commands)
                          if "ubus list log | grep -Fxq log" in cmd)
        clock_index = next(i for i, cmd in enumerate(self.commands)
                           if cmd.startswith("date -u -s "))
        update_index = self.commands.index("apk update")
        self.assertLess(route_index, boot_index)
        self.assertLess(boot_index, clock_index)
        self.assertLess(clock_index, update_index)
        self.assertIn('test "$(hostname)" != \'(none)\'',
                      self.commands[boot_index])

    def test_transient_clock_reset_is_reverified_before_package_download(self):
        self.package = self.package.rename(self.package.with_suffix(".apk"))
        self.run_guest("25.12.5", fail_clock_times=2)
        clocks = [i for i, cmd in enumerate(self.commands)
                  if cmd.startswith("date -u -s ")]
        self.assertEqual(3, len(clocks))
        self.assertTrue(any(cmd.startswith("wget -T 30 -O /tmp/mt-c-ci.apk")
                            for cmd in self.commands))
        self.assertLess(max(clocks), self.commands.index("apk update"))
        self.assertTrue(self.api_called)

    def test_persistent_bad_guest_clock_blocks_package_install(self):
        self.package = self.package.rename(self.package.with_suffix(".apk"))
        with self.assertRaisesRegex(smoke.SmokeFailure,
                                    "guest UTC clock failed verification"):
            self.run_guest("25.12.5", fail_clock_times=5)
        self.assertEqual(4, sum(cmd.startswith("date -u -s ")
                                for cmd in self.commands))
        self.assertFalse(any(cmd.startswith("apk update") for cmd in self.commands))
        self.assertFalse(any("wget -T 30 -O /tmp/mt-c-ci.apk" in cmd
                             for cmd in self.commands))
        self.assertFalse(self.api_called)

    def test_failed_feed_update_never_proceeds_to_install(self):
        with self.assertRaisesRegex(smoke.SmokeFailure, "opkg update failed"):
            self.run_guest("24.10.4", fail_update=True)
        self.assertEqual(self.commands.count("opkg update"), 4)
        self.assertFalse(any(command.startswith("opkg install") for command in self.commands))
        self.assertFalse(self.api_called)

    def test_failed_install_never_passes_backend_checks(self):
        with self.assertRaisesRegex(smoke.SmokeFailure, "opkg install .* failed after 4 attempts"):
            self.run_guest("24.10.4", fail_install=True)
        self.assertEqual(self.commands.count("opkg install /tmp/mt-c-ci.ipk"), 4)
        self.assertFalse(self.api_called)
        self.assertNotIn("/etc/init.d/magitrickle restart", self.commands)

    def test_wrong_release_package_format_is_rejected_before_boot(self):
        with patch("sys.argv", [str(SCRIPT), "--emulator-dir", str(self.emulator),
                                "--package", str(self.package), "--openwrt-version", "25.12.5"]), \
                patch.object(smoke, "run_test") as boot, redirect_stdout(io.StringIO()), \
                patch("sys.stderr", new_callable=io.StringIO):
            with self.assertRaises(SystemExit) as error:
                smoke.main()
            self.assertEqual(error.exception.code, 2)
            boot.assert_not_called()

    def test_package_server_serves_ipk_bytes_and_rejects_apk_path(self):
        with redirect_stdout(io.StringIO()), smoke.serve_package(self.package) as port:
            with urllib.request.urlopen(f"http://127.0.0.1:{port}/mt-c.ipk") as response:
                self.assertEqual(response.read(), self.package.read_bytes())
            with self.assertRaises(urllib.error.HTTPError) as error:
                urllib.request.urlopen(f"http://127.0.0.1:{port}/mt-c.apk")
            self.assertEqual(error.exception.code, 404)


if __name__ == "__main__":
    unittest.main()
