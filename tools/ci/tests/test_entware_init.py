#!/usr/bin/env python3
"""Run the Keenetic init script with isolated process-command fixtures.

Only its PATH is redirected. The pidof/killall/daemon fixtures model process
state and record service actions; they never inspect or signal host services.
The sleep and uptime fixtures make the production polling loops run quickly.
"""

import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
INIT = ROOT / "files/entware_kn/etc/init.d/S99magitrickle"


class EntwareInitTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="mt-c-kn-init-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.addCleanup((self.directory / "finished").touch)
        self.bin = self.directory / "bin"
        self.bin.mkdir()
        scripts = {
            "pidof": '''#!/bin/sh
[ "$1" = magitrickled ] || exit 2
[ -f "$ENTWARE_INIT_TEST_ROOT/running" ] || exit 1
printf '4242\\n'
''',
            "killall": '''#!/bin/sh
[ "$1" = magitrickled ] || exit 2
printf 'stop\\n' >> "$ENTWARE_INIT_TEST_ROOT/actions"
[ "${ENTWARE_INIT_STOP_FAIL:-0}" = 1 ] && exit 1
rm -f "$ENTWARE_INIT_TEST_ROOT/running"
''',
            "magitrickled": '''#!/bin/sh
printf 'start\\n' >> "$ENTWARE_INIT_TEST_ROOT/actions"
[ "${ENTWARE_INIT_DAEMON_FAIL:-0}" = 1 ] && exit 1
touch "$ENTWARE_INIT_TEST_ROOT/running"
while [ -d "$ENTWARE_INIT_TEST_ROOT" ] && [ ! -f "$ENTWARE_INIT_TEST_ROOT/finished" ]; do
    /bin/sleep 0.05
done
''',
            "cut": '''#!/bin/sh
clock="$ENTWARE_INIT_TEST_ROOT/clock"
uptime=$(cat "$clock")
printf '%s\\n' "$uptime"
''',
            "socat": '''#!/bin/sh
request=$(mktemp "$ENTWARE_INIT_TEST_ROOT/request.XXXXXX") || exit 2
cat > "$request"
[ -f "$ENTWARE_INIT_TEST_ROOT/running" ] || exit 1
printf 'probe\\n' >> "$ENTWARE_INIT_TEST_ROOT/actions"
uptime=$(cat "$ENTWARE_INIT_TEST_ROOT/clock")
if [ "$uptime" -lt "${ENTWARE_INIT_SOCKET_AT:-0}" ]; then
    exit 1
elif [ "$uptime" -lt "${ENTWARE_INIT_READY_AT:-301}" ]; then
    printf 'HTTP/1.1 503 Service Unavailable\\r\\n'
else
    printf 'HTTP/1.1 %s\\r\\n' "${ENTWARE_INIT_HTTP_STATUS:-204 No Content}"
fi
''',
            "logger": "#!/bin/sh\nexit 0\n",
            "sleep": '''#!/bin/sh
clock="$ENTWARE_INIT_TEST_ROOT/clock"
printf '%s\\n' "$(( $(cat "$clock") + ${ENTWARE_INIT_CLOCK_STEP:-1} ))" > "$clock"
exec /bin/sleep 0.02
''',
        }
        for name, body in scripts.items():
            path = self.bin / name
            path.write_text(body)
            path.chmod(0o755)

        path_assignment = "PATH=" + shlex.quote(str(self.bin) + ":/usr/bin:/bin")
        script, count = re.subn(r"^PATH=.*$", path_assignment,
                                INIT.read_text(), count=1, flags=re.MULTILINE)
        self.assertEqual(1, count, "test must isolate the init script's process commands")
        self.script = self.directory / "S99magitrickle"
        self.script.write_text(script)
        (self.directory / "clock").write_text("301\n")
        self.env = dict(os.environ, ENTWARE_INIT_TEST_ROOT=str(self.directory))
        self.env.pop("ENTWARE_INIT_STOP_FAIL", None)
        self.env.pop("ENTWARE_INIT_READY_AT", None)
        self.env.pop("ENTWARE_INIT_CLOCK_STEP", None)
        self.env.pop("ENTWARE_INIT_DAEMON_FAIL", None)
        self.env.pop("ENTWARE_INIT_SOCKET_AT", None)
        self.env.pop("ENTWARE_INIT_HTTP_STATUS", None)

    def service_action(self, action, *, running, stop_fails=False):
        if running:
            (self.directory / "running").touch()
        self.env["ENTWARE_INIT_STOP_FAIL"] = "1" if stop_fails else "0"
        result = subprocess.run(["/bin/sh", str(self.script), action],
                                env=self.env, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=20)
        for request in self.directory.glob("request.*"):
            self.assert_netfilter_status_request(request.read_bytes())
        actions = (self.directory / "actions").read_text().splitlines()
        return result, actions

    def assert_netfilter_status_request(self, request):
        head, separator, body = request.partition(b"\r\n\r\n")
        self.assertEqual(b"\r\n\r\n", separator, "HTTP headers must end with CRLF CRLF")
        lines = head.split(b"\r\n")
        self.assertEqual(b"POST /api/v1/system/hooks/netfilterd HTTP/1.1", lines[0])
        headers = {}
        for line in lines[1:]:
            self.assertNotIn(b"\r", line, "HTTP headers must use CRLF line endings")
            self.assertNotIn(b"\n", line, "HTTP headers must use CRLF line endings")
            name, colon, value = line.partition(b":")
            self.assertEqual(b":", colon, "HTTP header is missing its colon")
            name = name.lower()
            self.assertNotIn(name, headers, "duplicate HTTP header")
            headers[name] = value.strip()
        self.assertEqual(b"localhost", headers.get(b"host"))
        self.assertEqual(b"application/json", headers.get(b"content-type"))
        self.assertEqual(b"close", headers.get(b"connection"))
        self.assertNotIn(b"transfer-encoding", headers)
        length = headers.get(b"content-length", b"")
        self.assertTrue(length.isdigit(), "Content-Length must be a byte count")
        self.assertEqual(len(body), int(length), "Content-Length must match the JSON bytes")
        self.assertEqual({"action": "status"}, json.loads(body))

    def restart(self, **kwargs):
        return self.service_action("restart", **kwargs)

    def test_failed_stop_cannot_report_restart_success_or_attempt_start(self):
        result, actions = self.restart(running=True, stop_fails=True)
        self.assertNotEqual(0, result.returncode, result.stdout)
        self.assertEqual(["stop"], actions)
        self.assertTrue((self.directory / "running").exists())
        self.assertIn("failed", result.stdout)
        self.assertNotIn("already running", result.stdout)

    def test_successful_restart_stops_before_starting(self):
        result, actions = self.restart(running=True)
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertEqual(["stop", "start", "probe"], actions)
        self.assertTrue((self.directory / "running").exists())

    def test_restart_of_stopped_service_starts_without_stop(self):
        result, actions = self.restart(running=False)
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertEqual(["start", "probe"], actions)
        self.assertTrue((self.directory / "running").exists())

    def test_restart_waits_for_routing_after_process_appears(self):
        self.env["ENTWARE_INIT_READY_AT"] = "304"
        result, actions = self.restart(running=True)
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertEqual(["stop", "start"], actions[:2])
        self.assertIn("probe", actions[2:])
        self.assertGreaterEqual(int((self.directory / "clock").read_text()), 304)

    def test_foreground_start_waits_for_routing(self):
        self.env["ENTWARE_INIT_READY_AT"] = "304"
        result, actions = self.service_action("start", running=False)
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertEqual("start", actions[0])
        self.assertIn("probe", actions)
        self.assertNotIn("stop", actions)
        self.assertGreaterEqual(int((self.directory / "clock").read_text()), 304)

    def test_start_waits_for_existing_process_without_launching_another(self):
        self.env["ENTWARE_INIT_READY_AT"] = "304"
        result, actions = self.service_action("start", running=True)
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertEqual({"probe"}, set(actions))
        self.assertIn("already running", result.stdout)
        self.assertGreaterEqual(int((self.directory / "clock").read_text()), 304)

    def test_explicit_restart_waits_during_early_boot(self):
        (self.directory / "clock").write_text("20\n")
        self.env["ENTWARE_INIT_READY_AT"] = "23"
        result, actions = self.restart(running=False)
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertEqual("start", actions[0])
        self.assertIn("probe", actions)
        self.assertGreaterEqual(int((self.directory / "clock").read_text()), 23)

    def test_restart_reports_timeout_when_process_never_serves_routing_api(self):
        self.env["ENTWARE_INIT_READY_AT"] = "1000"
        self.env["ENTWARE_INIT_CLOCK_STEP"] = "15"
        result, actions = self.restart(running=False)
        self.assertNotEqual(0, result.returncode, result.stdout)
        self.assertEqual("start", actions[0])
        self.assertIn("probe", actions)
        self.assertIn("failed", result.stdout)
        self.assertIn("timed out after 60s", result.stdout)
        self.assertTrue((self.directory / "running").exists())
        self.assertGreaterEqual(int((self.directory / "clock").read_text()), 361)

    def assert_readiness_response_is_rejected(self, status):
        self.env["ENTWARE_INIT_HTTP_STATUS"] = status
        self.env["ENTWARE_INIT_CLOCK_STEP"] = "15"
        result, actions = self.restart(running=False)
        self.assertEqual(255, result.returncode, result.stdout)
        self.assertIn("probe", actions)
        self.assertIn("timed out after 60s", result.stdout)
        self.assertTrue((self.directory / "running").exists())

    def test_settings_only_recovery_cannot_report_routing_ready(self):
        self.assert_readiness_response_is_rejected("404 Not Found")

    def test_readiness_server_error_cannot_report_success(self):
        self.assert_readiness_response_is_rejected("500 Internal Server Error")

    def test_malformed_success_status_cannot_report_success(self):
        self.assert_readiness_response_is_rejected("2040 Invalid Status")

    def test_legacy_daemon_200_is_rejected_after_one_probe(self):
        self.env["ENTWARE_INIT_HTTP_STATUS"] = "200 OK"
        result, actions = self.service_action("start", running=True)
        self.assertEqual(255, result.returncode, result.stdout)
        self.assertEqual(["probe"], actions)
        self.assertEqual(301, int((self.directory / "clock").read_text()))
        self.assertIn("does not support netfilter status checks", result.stdout)
        self.assertIn("update and restart required", result.stdout)
        self.assertNotIn("already running", result.stdout)

    def test_existing_process_without_readiness_cannot_report_start_success(self):
        self.env["ENTWARE_INIT_READY_AT"] = "1000"
        self.env["ENTWARE_INIT_CLOCK_STEP"] = "15"
        result, actions = self.service_action("start", running=True)
        self.assertEqual(255, result.returncode, result.stdout)
        self.assertEqual({"probe"}, set(actions))
        self.assertNotIn("already running", result.stdout)

    def test_restart_retries_when_socket_cannot_be_reached(self):
        self.env["ENTWARE_INIT_SOCKET_AT"] = "304"
        result, actions = self.restart(running=False)
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertEqual("start", actions[0])
        self.assertGreaterEqual(int((self.directory / "clock").read_text()), 304)

    def test_failed_daemon_exits_before_readiness_deadline(self):
        self.env["ENTWARE_INIT_DAEMON_FAIL"] = "1"
        result, actions = self.restart(running=False)
        self.assertNotEqual(0, result.returncode, result.stdout)
        self.assertEqual(["start"], actions)
        self.assertIn("daemon exited before routing was ready", result.stdout)
        self.assertLess(int((self.directory / "clock").read_text()), 361)


if __name__ == "__main__":
    unittest.main()
