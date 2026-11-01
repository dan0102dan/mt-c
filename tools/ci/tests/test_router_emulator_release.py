#!/usr/bin/env python3
"""Hermetic release-selection/download tests; no GitHub network or QEMU needed."""
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "router_emulator_release.py"
spec = importlib.util.spec_from_file_location("router_emulator_release", SCRIPT)
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


def asset(version, suffix, data):
    name = f"Router-Emulator-{version}-{suffix}"
    return {"name": name, "browser_download_url":
            f"https://github.com/stavultras/router-emulator/releases/download/v{version}/{name}",
            "digest": "sha256:" + hashlib.sha256(data).hexdigest(), "size": len(data), "state": "uploaded"}


def example(version="4.2", binary=b"qemu-binary", presets=b"preset-data"):
    return {"tag_name": "v" + version, "draft": False, "prerelease": False, "id": 42,
            "assets": [asset(version, "linux-x86_64.tar.gz", binary),
                       asset(version, "presets.zip", presets)]}


class RouterEmulatorReleaseTests(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.root = Path(folder.name)

    def test_future_stable_release_auto_selects_its_exact_pair(self):
        info = release.parse_release(example(version="4.2.1"))
        self.assertEqual(info["release_tag"], "v4.2.1")
        self.assertEqual(info["version"], "4.2.1")
        self.assertEqual(set(info["assets"]), {"emulator", "presets"})
        self.assertTrue(info["assets"]["emulator"]["url"].endswith(
            "/v4.2.1/Router-Emulator-4.2.1-linux-x86_64.tar.gz"))
        self.assertEqual(info["assets"]["presets"]["size"], len(b"preset-data"))

    def test_never_use_drafts_prereleases_or_unexpected_tags(self):
        for field, value in (("draft", True), ("prerelease", True),
                             ("tag_name", "v4.2-rc1"), ("tag_name", "4.2"),
                             ("tag_name", "v4.2; echo hacked")):
            with self.subTest(field=field, value=value):
                data = example()
                data[field] = value
                with self.assertRaises(release.ReleaseError):
                    release.parse_release(data)
        for data in (None, {}, [], {"tag_name": "v4.1"}):
            with self.subTest(data=data), self.assertRaises(release.ReleaseError):
                release.parse_release(data)

    def test_missing_or_duplicate_or_unverified_assets_are_fatal(self):
        mutations = []
        data = example(); data["assets"].pop(); mutations.append(data)
        data = example(); data["assets"].append(copy.deepcopy(data["assets"][0])); mutations.append(data)
        for change in (
            ("digest", None), ("digest", "md5:bad"), ("digest", "sha256:" + "Z" * 64),
            ("browser_download_url", "https://evil.example.org/file"),
            ("browser_download_url", "https://github.com/stavultras/router-emulator/releases/download/v4.1/old.tar.gz"),
            ("state", "new"), ("size", -1), ("size", True), ("size", 1024 * 1024 * 1024),
        ):
            data = example(); data["assets"][0][change[0]] = change[1]; mutations.append(data)
        for data in mutations:
            with self.subTest(assets=data["assets"]), self.assertRaises(release.ReleaseError):
                release.parse_release(data)

    def test_downloaded_pair_is_hashed_and_release_provenance_recorded(self):
        binary, presets = b"router-emulator-v4.2", b"presets-v4.2"
        data = example(binary=binary, presets=presets)
        requested = []

        def urlopen(request, timeout):
            requested.append((request.full_url, dict(request.header_items()), timeout))
            if request.full_url == release.RELEASE_API:
                return io.BytesIO(json.dumps(data).encode("utf-8"))
            if request.full_url.endswith("linux-x86_64.tar.gz"):
                return io.BytesIO(binary)
            if request.full_url.endswith("presets.zip"):
                return io.BytesIO(presets)
            raise AssertionError("unexpected external URL")

        with patch.object(release.urllib.request, "urlopen", side_effect=urlopen):
            chosen = release.fetch_into(self.root, token="test-private-token")
        self.assertEqual(chosen["version"], "4.2")
        self.assertEqual((self.root / "emulator.tar.gz").read_bytes(), binary)
        self.assertEqual((self.root / "presets.zip").read_bytes(), presets)
        self.assertEqual(json.loads((self.root / "router-emulator-release.json").read_text()), chosen)
        self.assertEqual(len(requested), 3)
        self.assertEqual(requested[0][0], release.RELEASE_API)
        self.assertEqual(requested[0][1].get("Authorization"), "Bearer test-private-token")
        self.assertTrue(all("Authorization" not in headers for _, headers, _ in requested[1:]))

    def test_bad_hash_or_size_does_not_publish_partial_assets(self):
        original = example()
        (self.root / "emulator.tar.gz").write_bytes(b"previous-verified")
        (self.root / "presets.zip").write_bytes(b"previous-presets")
        bad_payloads = ((b"qemu-binarY", b"preset-data"), (b"qemu-binary", b"preset-older"),
                        (b"qemu-binary-too-large", b"preset-data"))
        for binary, presets in bad_payloads:
            with self.subTest(binary=binary, presets=presets):
                def urlopen(request, timeout):
                    if request.full_url == release.RELEASE_API:
                        return io.BytesIO(json.dumps(original).encode())
                    return io.BytesIO(binary if request.full_url.endswith(".tar.gz") else presets)
                with patch.object(release.urllib.request, "urlopen", side_effect=urlopen):
                    with self.assertRaises(release.ReleaseError):
                        release.fetch_into(self.root)
                self.assertEqual((self.root / "emulator.tar.gz").read_bytes(), b"previous-verified")
                self.assertEqual((self.root / "presets.zip").read_bytes(), b"previous-presets")
                self.assertFalse((self.root / "router-emulator-release.json").exists())
                self.assertFalse(list(self.root.glob(".router-emulator-*")))

    def test_cli_passes_selected_version_to_following_actions_step(self):
        choice = release.parse_release(example(version="4.3"))
        output = self.root / "github-output"
        with patch.object(release, "fetch_into", return_value=choice) as downloader, \
                patch("sys.argv", [str(SCRIPT), "--destination", str(self.root),
                                   "--github-output", str(output)]):
            release.main()
        self.assertEqual(output.read_text(), "version=4.3\ntag=v4.3\n")
        downloader.assert_called_once_with(self.root, token=None)

    def test_api_error_retries_then_fails_without_fallback(self):
        with patch.object(release.urllib.request, "urlopen", side_effect=OSError("offline")) as net, \
                patch.object(release.time, "sleep") as sleep:
            with self.assertRaisesRegex(release.ReleaseError, "failed to access"):
                release.fetch_into(self.root)
        self.assertEqual(net.call_count, 3)
        self.assertEqual([call.args[0] for call in sleep.call_args_list], [1, 2])
        self.assertEqual(list(self.root.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
