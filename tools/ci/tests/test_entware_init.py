#!/usr/bin/env python3
"""Run the Keenetic init script with isolated process-command fixtures.

Only its PATH is redirected. The pidof/killall/daemon fixtures model process
state and record service actions; they never inspect or signal host services.
The sleep fixture makes the production stop/start polling loops run quickly.
"""

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
touch "$ENTWARE_INIT_TEST_ROOT/running"
''',
            "cut": "#!/bin/sh\nprintf '301\\n'\n",
            "logger": "#!/bin/sh\nexit 0\n",
            "sleep": '''#!/bin/sh
[ "${ENTWARE_INIT_STOP_FAIL:-0}" = 1 ] && exit 0
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
        self.env = dict(os.environ, ENTWARE_INIT_TEST_ROOT=str(self.directory))
        self.env.pop("ENTWARE_INIT_STOP_FAIL", None)

    def restart(self, *, running, stop_fails=False):
        if running:
            (self.directory / "running").touch()
        self.env["ENTWARE_INIT_STOP_FAIL"] = "1" if stop_fails else "0"
        result = subprocess.run(["/bin/sh", str(self.script), "restart"],
                                env=self.env, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=5)
        actions = (self.directory / "actions").read_text().splitlines()
        return result, actions

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
        self.assertEqual(["stop", "start"], actions)
        self.assertTrue((self.directory / "running").exists())

    def test_restart_of_stopped_service_starts_without_stop(self):
        result, actions = self.restart(running=False)
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertEqual(["start"], actions)
        self.assertTrue((self.directory / "running").exists())


if __name__ == "__main__":
    unittest.main()
