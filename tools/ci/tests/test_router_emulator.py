#!/usr/bin/env python3
"""Host orchestration checks; these do not boot OpenWrt or test routing."""

from contextlib import ExitStack, nullcontext, redirect_stdout
import importlib.util
import io
import os
from pathlib import Path
import subprocess
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

    def run_guest(self, version, fail_update=False, fail_install=False, fail_clock_times=0,
                  baseline=False, fail_upgrade=False, fail_replacement=False):
        serial = MagicMock()
        clock_calls = 0
        baseline_package = None
        if baseline:
            baseline_package = self.directory / ("baseline" + self.package.suffix)
            baseline_package.write_bytes(b"lower-version fixture with a different executable")

        def run(command, **kwargs):
            nonlocal clock_calls
            if command.startswith("date -u -s "):
                clock_calls += 1
                return 1 if clock_calls <= fail_clock_times else 0
            if command in ("opkg update", "apk update"):
                return 1 if fail_update else 0
            if fail_install and command.startswith("opkg install /tmp/"):
                return 1
            if fail_upgrade and command in ("opkg install /tmp/mt-c-ci.ipk",
                                             "apk add --allow-untrusted /tmp/mt-c-ci.apk"):
                return 1
            if fail_replacement and command.startswith("old_pid="):
                raise smoke.SmokeFailure("running executable was not replaced")
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
                               self.directory / "logs", version, baseline_package)
            finally:
                self.assertTrue(stop.called)
                serial.close.assert_called_once()
                self.commands = [call.args[0] for call in serial.run.call_args_list]
                self.api_called = api.called
                self.api_calls = api.call_count

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
        self.assertNotIn("/etc/init.d/magitrickle restart", self.commands)

    def test_apk_still_installs_with_apk(self):
        self.package = self.package.rename(self.package.with_suffix(".apk"))
        self.run_guest("25.12.5")
        self.assertIn("apk add --allow-untrusted /tmp/mt-c-ci.apk", self.commands)
        self.assertIn("apk info -e mt-c", self.commands)
        self.assertFalse(any(command.startswith("opkg ") for command in self.commands))
        self.assertTrue(self.api_called)
        self.assertNotIn("/etc/init.d/magitrickle restart", self.commands)

    def test_both_managers_upgrade_without_removing_or_restarting_service(self):
        for version, suffix, install in (
            ("24.10.4", ".ipk", "opkg install"),
            ("25.12.5", ".apk", "apk add --allow-untrusted"),
        ):
            with self.subTest(version=version):
                self.package = self.package.rename(self.package.with_suffix(suffix))
                self.run_guest(version, baseline=True)
                first = self.commands.index(f"{install} /tmp/mt-c-baseline{suffix}")
                upgrade = self.commands.index(f"{install} /tmp/mt-c-ci{suffix}")
                record = next(i for i, command in enumerate(self.commands)
                              if "> /tmp/mtci-upgrade.pid" in command)
                verify = next(i for i, command in enumerate(self.commands)
                              if command.startswith("old_pid="))
                dhcp = next(i for i, command in enumerate(self.commands)
                            if "wget -T 30 -O /tmp/mtci-dhcp.sh" in command)
                self.assertLess(first, record)
                self.assertLess(record, upgrade)
                self.assertLess(upgrade, verify)
                self.assertLess(verify, dhcp)
                self.assertEqual(2, self.api_calls)
                self.assertIn("sha256sum /proc/$1/exe", self.commands[verify])
                self.assertIn("sha256sum /usr/bin/magitrickled", self.commands[verify])
                self.assertIn("sha256sum -c /tmp/mtci-upgrade.config", self.commands[verify])
                self.assertFalse(any("/etc/init.d/magitrickle restart" in cmd or
                                     "opkg remove" in cmd or "apk del" in cmd
                                     for cmd in self.commands))

    def test_failed_upgrade_does_not_proceed_to_routing_checks(self):
        self.package = self.package.rename(self.package.with_suffix(".apk"))
        with self.assertRaisesRegex(smoke.SmokeFailure, "apk add .* failed after 4 attempts"):
            self.run_guest("25.12.5", baseline=True, fail_upgrade=True)
        self.assertEqual(1, self.api_calls)
        self.assertFalse(any(command.startswith("old_pid=") for command in self.commands))
        self.assertFalse(any("wget -T 30 -O /tmp/mtci-dhcp.sh" in cmd for cmd in self.commands))

    def test_stale_process_fails_before_api_and_routing_can_hide_it(self):
        with self.assertRaisesRegex(smoke.SmokeFailure, "running executable was not replaced"):
            self.run_guest("24.10.4", baseline=True, fail_replacement=True)
        self.assertEqual(1, self.api_calls)
        self.assertFalse(any("wget -T 30 -O /tmp/mtci-dhcp.sh" in cmd for cmd in self.commands))

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
        baseline = self.directory / "baseline.ipk"
        baseline.write_bytes(b"baseline package bytes")
        with redirect_stdout(io.StringIO()), smoke.serve_package(self.package, baseline) as port:
            with urllib.request.urlopen(f"http://127.0.0.1:{port}/mt-c.ipk") as response:
                self.assertEqual(response.read(), self.package.read_bytes())
            with urllib.request.urlopen(f"http://127.0.0.1:{port}/mt-c-baseline.ipk") as response:
                self.assertEqual(response.read(), baseline.read_bytes())
            with self.assertRaises(urllib.error.HTTPError) as error:
                urllib.request.urlopen(f"http://127.0.0.1:{port}/mt-c.apk")
            self.assertEqual(error.exception.code, 404)


    def test_ipv6_gateway_preflight_retries_nondisruptively_and_fails_closed(self):
        # Exercise the real POSIX-shell preflight with a fake client-side ping.
        # No root, network namespaces, QEMU or network access required.
        routing = smoke.ROUTING_SCRIPT.read_text()
        prefix = "wait_ipv6_gateway() {\n"
        begin = routing.index(prefix)
        end = routing.index("\n}\nwait_ipv6_gateway", begin) + 2
        helper = routing[begin:end]
        self.assertLess(routing.index("\nwait_ipv6_gateway\n", end),
                        routing.index("# Main-table blackholes"))
        self.assertIn('ip -n "$CLIENT" -6 neigh show dev mtci-client0', helper)
        self.assertIn('ip -n "$CLIENT" -6 route get "$TARGET"', routing)
        self.assertNotIn('ipset add', helper)
        self.assertNotIn('ip -6 route add', helper)

        bin_dir = self.directory / "bin"
        bin_dir.mkdir()
        ping_count = self.directory / "ping-count"
        fake_ip = bin_dir / "ip"
        fake_ip.write_text("""#!/bin/sh
if [ "$1" = netns ] && [ "$2" = exec ] && [ "$4" = ping ]; then
    count=$(cat "$CI_PING_COUNT" 2>/dev/null || echo 0)
    count=$((count + 1))
    echo "$count" > "$CI_PING_COUNT"
    [ "$CI_SUCCEED_AT" -gt 0 ] && [ "$count" -ge "$CI_SUCCEED_AT" ]
    exit $?
fi
exit 0
""")
        fake_ip.chmod(0o755)
        fake_sleep = bin_dir / "sleep"
        fake_sleep.write_text("#!/bin/sh\nexit 0\n")
        fake_sleep.chmod(0o755)
        shell = ("""set -eu
CLIENT=mtci-client
LAN=br-lan
log() { echo "mt-c routing smoke: $*" >&2; }
die() { log "FAIL: $*"; exit 23; }
""" + helper + "\nwait_ipv6_gateway\n")
        env = dict(os.environ, PATH=f"{bin_dir}:{os.environ['PATH']}",
                   CI_PING_COUNT=str(ping_count))
        env["CI_SUCCEED_AT"] = "3"
        success = subprocess.run(["sh", "-c", shell], env=env,
                                 text=True, capture_output=True, timeout=5)
        self.assertEqual(0, success.returncode, success.stderr)
        self.assertEqual("3", ping_count.read_text().strip())
        self.assertIn("reachable after 3 attempts", success.stderr)

        ping_count.unlink()
        env["CI_SUCCEED_AT"] = "0"
        failed = subprocess.run(["sh", "-c", shell], env=env,
                                text=True, capture_output=True, timeout=5)
        self.assertEqual(23, failed.returncode, failed.stderr)
        self.assertEqual("10", ping_count.read_text().strip())
        self.assertIn("IPv6 LAN gateway unreachable before subnet6 routing test",
                      failed.stderr)


if __name__ == "__main__":
    unittest.main()
