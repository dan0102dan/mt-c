#!/usr/bin/env python3
"""Guard the tests -> SDK builds -> emulator CI contract from regressions."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]
WORKFLOWS = ROOT / ".github" / "workflows"


def workflow(name):
    return (WORKFLOWS / name).read_text(encoding="utf-8")


class TestCIOrchestration(unittest.TestCase):
    def test_all_four_checks_gate_packaging(self):
        ci = workflow("ci.yml")
        for job, source in (
            ("frontend", "check.yml"),
            ("backend", "check-c.yml"),
            ("netfilter", "check-netfilter.yml"),
            ("package_contracts", "check-packaging.yml"),
        ):
            self.assertIn(f"  {job}:\n", ci)
            self.assertIn(f"uses: ./.github/workflows/{source}", ci)
        self.assertIn(
            "needs: [frontend, backend, netfilter, package_contracts]", ci
        )
        for job in ("frontend", "backend", "netfilter", "package_contracts"):
            self.assertIn(f"needs.{job}.result == 'success'", ci)
        self.assertIn("uses: ./.github/workflows/build.yml", ci)
        self.assertIn("  pull_request:", ci)
        self.assertIn("  release:", ci)
        self.assertIn("  workflow_dispatch:", ci)

    def test_children_do_not_race_or_cancel_their_caller(self):
        ci = workflow("ci.yml")
        self.assertIn("cancel-in-progress: true", ci)
        for name in (
            "check.yml", "check-c.yml", "check-netfilter.yml",
            "check-packaging.yml", "build.yml",
        ):
            content = workflow(name)
            self.assertIn("  workflow_call:", content)
            self.assertNotIn("  pull_request:", content)
            self.assertNotIn("  push:", content)
            self.assertNotIn("cancel-in-progress: true", content)
        self.assertNotIn("  release:", workflow("build.yml"))
        self.assertNotIn("  workflow_dispatch:", workflow("build.yml"))

    def test_packages_remain_downloadable_from_pr(self):
        build = workflow("build.yml")
        self.assertIn("Upload package artifact 1", build)
        self.assertIn("actions/upload-artifact@v6", build)
        self.assertIn("github.event_name != 'release'", build)
        self.assertIn("github.event_name != 'workflow_dispatch'", build)
        self.assertIn("if-no-files-found: error", build)

    def test_real_emulator_runs_for_pr_and_push(self):
        build = workflow("build.yml")
        mode = "(github.event_name == 'push' || github.event_name == 'pull_request')"
        self.assertEqual(build.count(mode), 4)
        self.assertIn("--baseline-package", build)
        self.assertIn("tools/ci/router-emulator-smoke.py", build)
        self.assertIn("Prepare lower-version package for upgrade test", build)
        for version in ("24.10.4", "25.12.5"):
            self.assertIn(f"matrix.openwrt_version == '{version}'", build)
        self.assertIn("Upload router-emulator smoke logs", build)

    def test_entware_registry_contract_is_still_checked(self):
        self.assertIn("test_ghcr_login.py", workflow("check-packaging.yml"))
        self.assertIn("Authenticate Entware builder registry", workflow("build.yml"))

    def test_release_uploads_still_available(self):
        ci = workflow("ci.yml")
        build = workflow("build.yml")
        self.assertIn("Existing release tag to rebuild and populate", ci)
        self.assertIn("inputs.release_tag", ci)
        self.assertIn("gh release upload", build)
        self.assertIn("github.event.release.tag_name || inputs.release_tag", build)


if __name__ == "__main__":
    unittest.main()
