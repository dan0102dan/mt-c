#!/usr/bin/env python3
"""Offline contracts for the Entware SDK's authenticated GHCR image pull.

These inspect our workflow wiring, not Docker's authentication implementation.
The package matrix remains responsible for a real image pull and cross-build.
Only the standard library is required on the packaging-contract runner.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[3]


def build_job():
    text = (ROOT / ".github/workflows/build.yml").read_text()
    match = re.search(r"(?ms)^  build:\n.*?(?=^  [a-zA-Z_][\w-]*:|\Z)", text)
    if not match:
        raise AssertionError("Package build job not found")
    return match.group()


def step(job, name):
    for block in re.split(r"(?m)^      - ", job)[1:]:
        if block.startswith(f"name: {name}\n"):
            return block
    raise AssertionError(f"Missing package step: {name}")


class EntwareRegistryContract(unittest.TestCase):
    def test_login_precedes_sdk(self):
        job = build_job()
        auth = step(job, "Authenticate Entware builder registry")
        sdk = step(job, "Build with prebuilt Entware SDK")
        self.assertLess(job.index(auth), job.index(sdk))

    def test_only_build_job_gets_read_package_permission(self):
        job = build_job()
        match = re.search(r"(?m)^    permissions:\n((?:      [^\n]+\n)+)", job)
        self.assertIsNotNone(match)
        permissions = dict(line.strip().split(": ", 1) for line in match[1].splitlines())
        self.assertEqual(permissions, {"contents": "write", "packages": "read"})
        workflow = (ROOT / ".github/workflows/build.yml").read_text()
        self.assertEqual(workflow.count("packages: read"), 1)
        self.assertNotIn("packages: write", workflow)

    def test_login_scope_matches_all_entware_builds(self):
        job = build_job()
        condition = "if: steps.toolchain.outputs.entware_config != ''"
        for name in ("Authenticate Entware builder registry", "Build with prebuilt Entware SDK"):
            block = step(job, name)
            self.assertIn(condition, block)
            self.assertEqual(sum(line.lstrip().startswith("if:") for line in block.splitlines()), 1)

    def test_login_action_is_pinned_and_credentials_are_not_shell_arguments(self):
        auth = step(build_job(), "Authenticate Entware builder registry")
        self.assertRegex(auth, r"uses: docker/login-action@[0-9a-f]{40}(?:\s|$)")
        for entry in ("registry: ghcr.io", "username: ${{ github.actor }}",
                      "password: ${{ github.token }}", "logout: true"):
            self.assertIn(entry, auth)
        self.assertNotRegex(auth, r"(?m)^\s*(run|continue-on-error):")

    def test_existing_sdk_and_build_failure_gate_are_preserved(self):
        job = build_job()
        sdk = step(job, "Build with prebuilt Entware SDK")
        self.assertIn("uses: ownik/gh-action-entware-sdk@003ddc33bb3b12a2f96e0c4689c56351614cf01a", sdk)
        self.assertIn("PACKAGES: mt-c", sdk)
        failure = step(job, "Report Entware SDK build failure")
        self.assertIn("if: steps.entware_sdk.outcome == 'failure'", failure)
        self.assertIn("exit 1", failure)
        self.assertNotIn("continue-on-error", failure)

    def test_build_workflow_changes_trigger_the_contract(self):
        text = (ROOT / ".github/workflows/check-packaging.yml").read_text()
        self.assertEqual(text.count("- '.github/workflows/build.yml'"), 2)
        self.assertIn("run: python3 tools/ci/tests/test_entware_registry.py", text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
