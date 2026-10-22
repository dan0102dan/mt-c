#!/usr/bin/env python3
"""Contract tests for manual PR target selection (no SDK download)."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "tools/ci/build_matrix.py"
spec = importlib.util.spec_from_file_location("build_matrix", SCRIPT)
matrix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(matrix)


class MatrixTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for platform, arch in (
            ("entware", "mipsel-3.4_kn"),
            ("entware", "aarch64-3.10_kn"),
            ("openwrt", "aarch64_cortex-a53"),
            ("openwrt", "mips64_mips64r2"),
        ):
            f = self.root / "config" / platform / (arch + ".config")
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_text("PLATFORM=" + platform + "\nTARGET=" + arch + "\n")
        self.all_configs = matrix.collect_configs(self.root)

    def pick(self, **kw):
        args = dict(pr_number="26", platform="entware", architecture="mipsel-3.4_kn",
                    openwrt_version="25.12.5")
        args.update(kw)
        return matrix.select_configs(self.all_configs, **args)

    def test_full_matrix_and_pr_emulator_target(self):
        self.assertEqual(len(self.all_configs), 5)
        self.assertEqual(len(matrix.select_configs(self.all_configs, event_name="push")), 5)
        self.assertEqual(
            [r["openwrt_version"] for r in matrix.select_configs(self.all_configs, event_name="pull_request")],
            ["25.12.5"],
        )

    def test_keenetic_build_is_entware_ipk(self):
        self.assertEqual(self.pick()[0]["config"], "config/entware/mipsel-3.4_kn.config")
        self.assertEqual(self.pick()[0]["package_ext"], "ipk")

    def test_openwrt_versions_keep_correct_package_managers(self):
        for version, ext in (("24.10.4", "ipk"), ("25.12.5", "apk")):
            with self.subTest(version=version):
                out = self.pick(platform="openwrt", architecture="aarch64_cortex-a53",
                                openwrt_version=version)
                self.assertEqual(out[0]["package_ext"], ext)

    def test_snapshot_only_target(self):
        out = self.pick(platform="openwrt", architecture="mips64_mips64r2",
                        openwrt_version="snapshot")
        self.assertEqual(out[0]["package_ext"], "apk")
        with self.assertRaises(ValueError):
            self.pick(platform="openwrt", architecture="mips64_mips64r2",
                      openwrt_version="24.10.4")

    def test_unsupported_and_unsafe_requests_fail_closed(self):
        for changed in (
            {"architecture": "../aarch64-3.10_kn"},
            {"architecture": "not-in-config"},
            {"platform": "bad"},
            {"pr_number": "2; echo foo"},
            {"pr_number": "0"},
            {"platform": "openwrt", "architecture": "aarch64_cortex-a53",
             "openwrt_version": "99.9.9"},
        ):
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                self.pick(**changed)

    def test_cli_produces_exact_single_target(self):
        env = {**os.environ, "GITHUB_EVENT_NAME": "workflow_dispatch",
               "MANUAL_PR_NUMBER": "26", "MANUAL_PLATFORM": "entware",
               "MANUAL_ARCHITECTURE": "mipsel-3.4_kn", "MANUAL_OPENWRT_VERSION": "25.12.5"}
        result = subprocess.run([sys.executable, str(SCRIPT)], cwd=self.root, env=env,
                                capture_output=True, text=True, check=True)
        self.assertEqual(
            json.loads(result.stdout.partition("=")[2])[0]["config"],
            "config/entware/mipsel-3.4_kn.config",
        )

    def test_reusable_workflow_and_release_guards(self):
        build = (ROOT / ".github/workflows/build.yml").read_text()
        manual = (ROOT / ".github/workflows/build-pr.yml").read_text()
        self.assertIn("workflow_call:", build)
        self.assertIn("inputs.pr_number == '' && (github.event_name == 'release'", build)
        self.assertIn("uses: ./.github/workflows/build.yml", manual)
        self.assertIn("permissions:\n      contents: read", manual)
        self.assertIn("gh release create", manual)
        self.assertIn("if: " + chr(36) + "{{ inputs.publish_prerelease", manual)


if __name__ == "__main__":
    unittest.main()
