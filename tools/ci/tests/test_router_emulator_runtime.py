#!/usr/bin/env python3
"""Host-runtime regressions: real child processes, no network/QEMU required."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "router_emulator_runtime.py"
SPEC = importlib.util.spec_from_file_location("router_runtime", SCRIPT)
runtime = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runtime)
COMMIT = "1" * 40
COMPATIBLE = '''#!/bin/sh
case "$*" in
 --version) echo 'QEMU emulator version fixture';;
 '-machine help') echo 'mt7981-router Router Emulator fixture';;
 *) exit 64;;
esac
'''
INCOMPATIBLE = '''#!/bin/sh
echo "libm.so.6: version GLIBC_2.43 not found" >&2
exit 1
'''


def executable(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content)
    path.chmod(0o755)


class RuntimeTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        (self.root / "VERSION").write_text("4.1\n")
        self.binary = self.root / "qemu/qemu-system-aarch64"
        executable(self.binary, COMPATIBLE)
        self.env = patch.dict(os.environ, {"QEMU_BIN": ""})
        self.env.start()
        self.addCleanup(self.env.stop)

    def source_fixture(self, source, tag, log, *, version="4.1", fail=False, bad_binary=False):
        self.assertEqual(tag, "v4.1")
        (source / "VERSION").write_text(version + "\n")
        # The build runs as an actual child and enforces its intended env.
        contents = '''#!/bin/sh
set -eu
test "$JOBS" = 2
test -z "$PGO_NAND"
printf '%s' "$CONFIGURE_ARGS" | grep -q -- '--disable-gtk'
'''
        if fail:
            contents += 'echo build-failed >&2; exit 22\n'
        else:
            contents += 'mkdir -p src/qemu/build\n'
            contents += "cat > src/qemu/build/qemu-system-aarch64 <<'EOF'\n"
            contents += INCOMPATIBLE if bad_binary else COMPATIBLE
            contents += 'EOF\nchmod +x src/qemu/build/qemu-system-aarch64\n'
        (source / "build.sh").write_text(contents)
        return COMMIT

    def evidence(self):
        return json.loads((self.root / "work/ci-emulator-runtime.log").read_text())

    def test_compatible_release_uses_exact_binary_without_checkout_or_install(self):
        with patch.object(runtime, "checkout_source") as checkout, \
                patch.object(runtime, "checked") as checked:
            selected = runtime.ensure_runtime(self.root, "4.1", install_build_deps=True)
        self.assertEqual(selected, self.binary)
        self.assertEqual(self.evidence()["origin"], "release")
        self.assertEqual(self.evidence()["status"], "ready")
        self.assertEqual(self.evidence()["qemu_sha256"], runtime.sha256(self.binary))
        checkout.assert_not_called()
        checked.assert_not_called()

    def test_loader_failure_rebuilds_same_release_and_launch_path_uses_new_binary(self):
        executable(self.binary, INCOMPATIBLE)
        with patch.object(runtime, "checkout_source", side_effect=self.source_fixture) as checkout:
            selected = runtime.ensure_runtime(self.root, "4.1")
        checkout.assert_called_once()
        self.assertEqual(selected, self.root / "src/qemu/build/qemu-system-aarch64")
        self.assertTrue((self.root / "src/qemu").is_symlink())
        self.assertEqual(self.evidence()["origin"], "source")
        self.assertEqual(self.evidence()["source_commit"], COMMIT)
        self.assertEqual(self.evidence()["release_tag"], "v4.1")
        log = (self.root / "work/ci-emulator-runtime-build.log").read_text()
        self.assertIn("GLIBC_2.43", log)
        self.assertIn("mt7981-router", log)
        # Actual child execution, using the same default path as emulator.sh.
        subprocess.run([str(selected), "-machine", "help"], check=True, capture_output=True)
        self.assertEqual(self.evidence()["qemu_sha256"], runtime.sha256(selected))

    def test_verified_source_reuse_keeps_provenance_and_rechecks_executable(self):
        executable(self.binary, INCOMPATIBLE)
        with patch.object(runtime, "checkout_source", side_effect=self.source_fixture):
            selected = runtime.ensure_runtime(self.root, "4.1")
        before = self.evidence()
        with patch.object(runtime, "checkout_source") as checkout:
            self.assertEqual(selected, runtime.ensure_runtime(self.root, "4.1"))
        self.assertEqual(before, self.evidence())
        checkout.assert_not_called()

    def test_modified_or_untracked_native_binary_is_not_reused(self):
        executable(self.binary, INCOMPATIBLE)
        with patch.object(runtime, "checkout_source", side_effect=self.source_fixture):
            selected = runtime.ensure_runtime(self.root, "4.1")
        executable(selected, COMPATIBLE + "# altered\n")
        with patch.object(runtime, "checkout_source") as checkout:
            with self.assertRaisesRegex(runtime.RuntimeFailure, "unverified local"):
                runtime.ensure_runtime(self.root, "4.1")
        checkout.assert_not_called()
        self.assertEqual(self.evidence()["status"], "failed")

    def test_wrong_release_and_invalid_tag_never_launch_or_install(self):
        for version in ("4.2", "4.1; touch bad", "4.1\nX=Y", "../4.1"):
            with self.subTest(version=version), patch.object(runtime, "checkout_source") as checkout:
                with self.assertRaises(runtime.RuntimeFailure):
                    runtime.ensure_runtime(self.root, version)
                checkout.assert_not_called()
        self.assertFalse((self.root / "work").exists())

    def test_wrong_source_version_fails_before_build_or_symlink(self):
        executable(self.binary, INCOMPATIBLE)
        def wrong(source, tag, log):
            return self.source_fixture(source, tag, log, version="4.2")
        with patch.object(runtime, "checkout_source", side_effect=wrong):
            with self.assertRaisesRegex(runtime.RuntimeFailure, "source VERSION"):
                runtime.ensure_runtime(self.root, "4.1")
        self.assertFalse((self.root / "src/qemu").exists())
        self.assertEqual(list((self.root / "work").glob("runtime-source-*")), [])
        self.assertEqual(self.evidence()["status"], "failed")

    def test_failed_build_never_publishes_or_falls_back_to_incompatible_release(self):
        executable(self.binary, INCOMPATIBLE)
        def fail(source, tag, log):
            return self.source_fixture(source, tag, log, fail=True)
        with patch.object(runtime, "checkout_source", side_effect=fail):
            with self.assertRaisesRegex(runtime.RuntimeFailure, "command failed"):
                runtime.ensure_runtime(self.root, "4.1")
        self.assertFalse((self.root / "src/qemu").exists())
        self.assertEqual(list((self.root / "work").glob("runtime-source-*")), [])
        self.assertIn("build-failed", (self.root / "work/ci-emulator-runtime-build.log").read_text())

    def test_rebuilt_binary_must_really_run(self):
        executable(self.binary, INCOMPATIBLE)
        def bad(source, tag, log):
            return self.source_fixture(source, tag, log, bad_binary=True)
        with patch.object(runtime, "checkout_source", side_effect=bad):
            with self.assertRaisesRegex(runtime.RuntimeFailure, "runtime preflight"):
                runtime.ensure_runtime(self.root, "4.1")
        self.assertFalse((self.root / "src/qemu").exists())

    def test_unpatched_system_qemu_cannot_pass_probe(self):
        executable(self.binary, COMPATIBLE.replace("mt7981-router", "virt"))
        with (self.root / "probe.log").open("w") as log:
            self.assertFalse(runtime.probe(self.binary, log))
        self.assertIn("lacks the required", (self.root / "probe.log").read_text())

    def test_source_checkout_uses_the_release_tag_and_checks_its_commit(self):
        source = self.root / "checkout"
        with (self.root / "checkout.log").open("w") as log, \
                patch.object(runtime, "checked") as checked, \
                patch.object(runtime.subprocess, "check_output", return_value=COMMIT + "\n") as output:
            self.assertEqual(COMMIT, runtime.checkout_source(source, "v4.1", log))
            checked.assert_called_once_with(
                ["git", "clone", "--depth", "1", "--single-branch", "--branch", "v4.1",
                 runtime.UPSTREAM, str(source)], log, timeout=300)
            self.assertIn("refs/tags/v4.1^{commit}", output.call_args.args[0])
        with (self.root / "checkout.log").open("w") as log, \
                patch.object(runtime, "checked"), \
                patch.object(runtime.subprocess, "check_output", side_effect=[COMMIT, "2" * 40]):
            with self.assertRaisesRegex(runtime.RuntimeFailure, "does not match"):
                runtime.checkout_source(source, "v4.1", log)

    def test_invalid_jobs_and_external_qemu_override_fail_before_build(self):
        for jobs in (0, -1, 33, True):
            with self.subTest(jobs=jobs), self.assertRaises(runtime.RuntimeFailure):
                runtime.ensure_runtime(self.root, jobs=jobs)
        with patch.dict(os.environ, {"QEMU_BIN": "/unverified/qemu"}):
            with self.assertRaisesRegex(runtime.RuntimeFailure, "unset QEMU_BIN"):
                runtime.ensure_runtime(self.root, "4.1")

    def test_preset_cli_checks_runtime_before_flash_preparation(self):
        # Execute the real CLI with a compatible child, so this test catches a
        # missing import/integration without any network or package installation.
        (self.root / "presets").mkdir()
        (self.root / "presets/cudy-tr3000-v1.ini").write_text(
            "[preset]\nflash-type=nand\nflash-id=efaa21\n"
            "dump-dir=nand-tr3000\nopenwrt=cudy_tr3000-v1\n")
        executable(self.root / "tools/prepare-nand.sh", '''#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
grep -q '"status": "ready"' work/ci-emulator-runtime.log
mkdir -p nand-tr3000
printf x > nand-tr3000/mt7981.mtd0.BL2.bin
''')
        command = [sys.executable, str(SCRIPT.with_name("router_emulator_preset.py")),
                   "--emulator-dir", str(self.root), "--preset", "cudy-tr3000-v1",
                   "--profile", "cudy_tr3000-v1", "--openwrt-version", "24.10.4",
                   "--expected-version", "4.1", "--no-install-build-deps"]
        subprocess.run(command, check=True, capture_output=True, timeout=10)
        self.assertTrue((self.root / "nand-tr3000/mt7981.mtd0.BL2.bin").is_file())


if __name__ == "__main__":
    unittest.main()
