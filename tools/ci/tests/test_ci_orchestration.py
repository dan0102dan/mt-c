#!/usr/bin/env python3
"""Guard the tests -> SDK builds -> emulator CI contract from regressions."""
import os
from pathlib import Path
import subprocess
import tempfile
import textwrap
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


    def test_diagnostics_are_failure_only_but_packages_remain_downloadable(self):
        netfilter = workflow("check-netfilter.yml")
        nf_upload = netfilter.split(
            "      - name: Preserve failing test logs\n", 1
        )[1]
        self.assertIn("        if: failure()\n", nf_upload)
        self.assertIn("name: netfilter-recovery-legacy-nft", nf_upload)
        self.assertIn("path: src/backend-c/recovery-*.log", nf_upload)

        build = workflow("build.yml")
        em_upload = build.split(
            "      - name: Upload router-emulator smoke logs on failure\n", 1
        )[1].split("\n      - name:", 1)[0]
        self.assertIn("        if: failure() &&", em_upload)
        self.assertIn("router-emulator-smoke-", em_upload)
        self.assertIn("retention-days: 7", em_upload)

        package_upload = build.split(
            "      - name: Upload package artifact 1\n", 1
        )[1].split("\n      - name:", 1)[0]
        self.assertNotIn("if: failure()", package_upload)
        self.assertIn("actions/upload-artifact@v6", package_upload)
        self.assertIn("if-no-files-found: error", package_upload)

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


    def test_release_upload_retries_only_http_server_errors(self):
        step = workflow("build.yml").split(
            "      - name: Upload package assets to release\n", 1
        )[1].split("\n      - name:", 1)[0]
        script = textwrap.dedent(step.split("        run: |\n", 1)[1])
        for error, failures, package, status, attempts, delays in (
            ("HTTP 502: Error creating policy", 1, "test package.apk", 0, 2, ["5"]),
            ("HTTP 503: Service unavailable", 2, "test package.apk", 0, 3, ["5", "10"]),
            ("HTTP 504: Gateway timeout", 3, "test package.apk", 17, 3, ["5", "10"]),
            ("HTTP 403: Forbidden", 3, "test package.apk", 17, 1, []),
            ("HTTP 404: Not Found", 3, "test package.apk", 17, 1, []),
            ("", 0, "test package.apk", 0, 1, []),
            ("", 0, "", 0, 0, []),
        ):
            with self.subTest(error=error, failures=failures, package=package):
                with tempfile.TemporaryDirectory() as tmp:
                    root = Path(tmp)
                    (root / "gh").write_text('''#!/usr/bin/env bash
printf '%s\\n' "$#" "$@" >> "$TRACE"
n=$(grep -c '^7$' "$TRACE")
if [ "$n" -le "$FAILURES" ]; then
    printf '%s\\n' "$ERROR" >&2
    exit 17
fi
''')
                    (root / "sleep").write_text('''#!/usr/bin/env bash
printf '%s\\n' "$1" >> "$DELAYS"
''')
                    for command in ("gh", "sleep"):
                        (root / command).chmod(0o755)
                    trace, delay_log = root / "trace", root / "delays"
                    trace.touch()
                    delay_log.touch()
                    env = {
                        **os.environ, "PATH": f'{root}:{os.environ["PATH"]}',
                        "TRACE": str(trace), "DELAYS": str(delay_log),
                        "ERROR": error, "FAILURES": str(failures),
                        "PACKAGE_1": package, "RELEASE_TAG": "0.8.7.3",
                        "GITHUB_REPOSITORY": "owner/repo",
                    }
                    result = subprocess.run(
                        ["bash", "-e", "-c", script], env=env,
                        capture_output=True, text=True, timeout=10,
                    )
                    self.assertEqual(result.returncode, status, result.stderr)
                    self.assertEqual(
                        trace.read_text().splitlines(),
                        ["7", "release", "upload", "0.8.7.3", package,
                         "--repo", "owner/repo", "--clobber"] * attempts,
                    )
                    self.assertEqual(delay_log.read_text().splitlines(), delays)


if __name__ == "__main__":
    unittest.main()
