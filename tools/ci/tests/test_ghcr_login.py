#!/usr/bin/env python3
"""Exercise GHCR login without credentials, Docker or network access."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / 'tools/ci/login-ghcr.sh'


class RegistryAuthentication(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='mtc ghcr test ')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        self.trace = self.root / 'calls.jsonl'
        self.token = 'dummy-regression-token-not-a-secret'
        self.env = {
            **os.environ,
            'PATH': f'{self.bin}:{os.environ["PATH"]}',
            'GHCR_TOKEN': self.token,
            'GITHUB_ACTOR': 'test-actor',
            'DOCKER_CONFIG': str(self.root / 'private docker config'),
            'TEST_TRACE': str(self.trace),
            'TEST_ROOT': str(self.root),
            'TEST_FAILURES': '0',
        }
        self.command('docker', '''#!/usr/bin/env python3
import json, os, pathlib, sys
root = pathlib.Path(os.environ['TEST_ROOT'])
if sys.argv[1:2] == ['pull']:
    authenticated = (pathlib.Path(os.environ['DOCKER_CONFIG']) / 'config.json').is_file()
    print('Image pulled' if authenticated else 'Error response from daemon: unauthorized')
    sys.exit(0 if authenticated else 1)
count = root / 'attempts'
n = int(count.read_text()) + 1 if count.exists() else 1
count.write_text(str(n))
received = sys.stdin.read()
with open(os.environ['TEST_TRACE'], 'a') as out:
    out.write(json.dumps({'argv': sys.argv[1:], 'attempt': n,
                          'stdin_matches': received == os.environ['GHCR_TOKEN']}) + '\\n')
config = pathlib.Path(os.environ['DOCKER_CONFIG']) / 'config.json'
config.write_text('dummy stored credential')
if n <= int(os.environ['TEST_FAILURES']):
    print('Error response from daemon: unauthorized', file=sys.stderr)
    sys.exit(1)
print('Login Succeeded')
''')
        self.command('sleep', '''#!/usr/bin/env python3
import os, pathlib, sys
with (pathlib.Path(os.environ['TEST_ROOT']) / 'delays').open('a') as out:
    out.write(sys.argv[1] + '\\n')
''')
        self.command('timeout', '''#!/usr/bin/env python3
import os, pathlib, sys
with (pathlib.Path(os.environ['TEST_ROOT']) / 'timeouts').open('a') as out:
    out.write(sys.argv[1] + '\\n')
os.execvp(sys.argv[2], sys.argv[2:])
''')

    def command(self, name, text):
        path = self.bin / name
        path.write_text(text)
        path.chmod(0o755)

    def run_login(self, **overrides):
        env = {**self.env, **overrides}
        result = subprocess.run(['bash', str(SCRIPT)], env=env, text=True,
                                capture_output=True, timeout=20)
        self.assertNotIn(self.token, result.stdout + result.stderr)
        return result

    def calls(self):
        return [json.loads(line) for line in self.trace.read_text().splitlines()]

    def test_token_uses_stdin_and_only_read_registry_login(self):
        result = self.run_login()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls(), [{
            'argv': ['login', 'ghcr.io', '--username', 'test-actor', '--password-stdin'],
            'attempt': 1, 'stdin_matches': True,
        }])
        self.assertEqual((self.root / 'timeouts').read_text(), '45s\n')
        self.assertFalse((self.root / 'delays').exists())

    def test_anonymous_pull_denial_is_not_repeated_after_login(self):
        command = ['docker', 'pull', 'ghcr.io/openwrt/buildbot/buildworker-v3.11.3:latest']
        before = subprocess.run(command, env=self.env, text=True,
                                capture_output=True, timeout=20)
        self.assertNotEqual(before.returncode, 0)
        self.assertIn('unauthorized', before.stdout)
        self.assertEqual(self.run_login().returncode, 0)
        after = subprocess.run(command, env=self.env, text=True,
                               capture_output=True, timeout=20)
        self.assertEqual(after.returncode, 0, after.stdout + after.stderr)

    def test_private_configuration_survives_for_sdk_step(self):
        self.assertEqual(self.run_login().returncode, 0)
        config = Path(self.env['DOCKER_CONFIG'])
        self.assertEqual(config.stat().st_mode & 0o777, 0o700)
        self.assertTrue((config / 'config.json').is_file())
        self.assertEqual((config / 'config.json').stat().st_mode & 0o777, 0o600)

    def test_transient_registry_errors_are_bounded_and_retried(self):
        result = self.run_login(TEST_FAILURES='2')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(self.calls()), 3)
        self.assertTrue(all(x['stdin_matches'] for x in self.calls()))
        self.assertEqual((self.root / 'delays').read_text(), '2\n4\n')
        self.assertEqual((self.root / 'timeouts').read_text(), '45s\n' * 3)

    def test_permanent_denial_fails_without_stored_credential(self):
        result = self.run_login(TEST_FAILURES='100')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(len(self.calls()), 3)
        self.assertIn('refusing to start the SDK', result.stderr)
        self.assertFalse((Path(self.env['DOCKER_CONFIG']) / 'config.json').exists())

    def test_missing_required_inputs_fail_before_docker(self):
        for name in ['GHCR_TOKEN', 'GITHUB_ACTOR', 'DOCKER_CONFIG']:
            with self.subTest(name=name):
                result = self.run_login(**{name: ''})
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.trace.exists())

    def test_shell_xtrace_does_not_expose_token(self):
        result = subprocess.run(['bash', '-x', str(SCRIPT)], env=self.env,
                                text=True, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn(self.token, result.stdout + result.stderr)

    def test_successful_login_does_not_mask_a_later_sdk_failure(self):
        result = subprocess.run(
            ['bash', '-e', '-c', 'bash "$1"; exit 42', 'test', str(SCRIPT)],
            env=self.env, text=True, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 42)


class WorkflowContract(unittest.TestCase):
    def setUp(self):
        self.workflow = (ROOT / '.github/workflows/build.yml').read_text()
        self.build = self.workflow.split('  build:\n', 1)[1]

    def test_auth_is_before_original_pinned_sdk_action(self):
        auth = self.build.index('      - name: Authenticate Entware builder registry\n')
        sdk = self.build.index('      - name: Build with prebuilt Entware SDK\n')
        self.assertLess(auth, sdk)
        self.assertIn("if: steps.toolchain.outputs.entware_config != ''", self.build[auth:sdk])
        self.assertIn('GHCR_TOKEN: ${{ github.token }}', self.build[auth:sdk])
        self.assertIn('run: bash tools/ci/login-ghcr.sh', self.build[auth:sdk])
        self.assertIn('uses: ownik/gh-action-entware-sdk@003ddc33bb3b12a2f96e0c4689c56351614cf01a', self.build)

    def test_packages_read_is_job_scoped_and_cleanup_is_unconditional(self):
        before_jobs = self.workflow.split('jobs:\n', 1)[0]
        self.assertNotIn('packages:', before_jobs)
        self.assertIn('    permissions:\n      contents: write\n      packages: read\n', self.build)
        # runner.temp cannot be used at jobs.<job_id>.env level.
        job_header = self.build.split('    strategy:\n', 1)[0]
        self.assertNotIn('DOCKER_CONFIG', job_header)
        auth_step = self.build.split('      - name: Authenticate Entware builder registry\n', 1)[1].split('\n      - name:', 1)[0]
        sdk_step = self.build.split('      - name: Build with prebuilt Entware SDK\n', 1)[1].split('\n      - name:', 1)[0]
        cleanup_step = self.build.split('      - name: Remove Entware registry credentials\n', 1)[1].split('\n      - name:', 1)[0]
        for step in (auth_step, sdk_step, cleanup_step):
            self.assertIn('DOCKER_CONFIG: ${{ runner.temp }}/mt-c-docker-auth', step)
        cleanup = self.build.split('      - name: Remove Entware registry credentials\n', 1)[1].split('\n      - name:', 1)[0]
        self.assertIn("if: always() && steps.toolchain.outputs.entware_config != ''", cleanup)
        self.assertIn('rm -f -- "${DOCKER_CONFIG:?}/config.json"', cleanup)

    def test_auth_regressions_run_in_regular_package_ci(self):
        contracts = (ROOT / '.github/workflows/check-packaging.yml').read_text()
        self.assertIn('run: python3 tools/ci/tests/test_ghcr_login.py', contracts)
        self.assertIn('  workflow_call:', contracts)
        # Event triggers now live in the CI orchestrator; the test is reusable.
        pipeline = (ROOT / '.github/workflows/ci.yml').read_text()
        self.assertIn('  pull_request:', pipeline)
        self.assertIn('  push:', pipeline)
        self.assertIn('uses: ./.github/workflows/check-packaging.yml', pipeline)


if __name__ == '__main__':
    unittest.main(verbosity=2)
