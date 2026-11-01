#!/usr/bin/env python3
"""Source staging/verification gates only: no SDK compiler or network."""
import hashlib
import importlib.util
import io
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "build-emulator-package.py"
SPEC = importlib.util.spec_from_file_location("emulator_package", SCRIPT)
package = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(package)

BENCH = SCRIPT.parents[1] / "bench"
HOST_SPEC = importlib.util.spec_from_file_location("dns_host_compare", BENCH / "run_dns_host_compare.py")
host = importlib.util.module_from_spec(HOST_SPEC)
with patch.object(sys, "path", [str(BENCH), *sys.path]):
    HOST_SPEC.loader.exec_module(host)


class PreparedSourceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.sdk = self.root / "sdk"; self.sdk.mkdir()
        self.archive = self.root / "fresh.tar.gz"; self.archive.write_bytes(b"fresh candidate source archive")
        self.cache = self.sdk / "dl/mt-c-source.tar.gz"
        self.cache.parent.mkdir(); self.cache.write_bytes(b"previous baseline source archive")
        self.version = "0.8.5.3"
        self.unpacked = self.sdk / "build_dir/target-aarch64_musl" / ("mt-c-" + self.version)
        self.files = {
            "src/backend-c/src/main/main.c": b"int main(void) { return 0; }\n",
            "src/backend-c/src/netfilter/ruleset.c": b"/* current candidate */\n",
            "src/backend-c/include/magitrickle/ruleset.h": b"/* matching header */\n",
            "src/backend-c/Makefile": b"CFLAGS += -O2\n",
        }
        self.expected = {name: hashlib.sha256(data).hexdigest() for name, data in self.files.items()}
        self.calls = []
        self.log, self.env = io.BytesIO(), {"BUILD_FIXTURE": "1"}

    def write_unpacked(self, files=None, root=None):
        for name, data in (self.files if files is None else files).items():
            path = (self.unpacked if root is None else root) / name
            path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data)

    def fake_run(self, prepare):
        def run(command, cwd, log, env):
            self.assertEqual(cwd, self.sdk); self.assertIs(log, self.log); self.assertIs(env, self.env)
            self.calls.append(list(command))
            if "package/mt-c/clean" in command:
                # Simulate the observed SDK behavior: clean restores the old
                # fixed-name cached archive, even if a caller removed it first.
                self.cache.write_bytes(b"stale cache recreated by clean")
            elif "package/mt-c/prepare" in command:
                self.assertEqual(self.cache.read_bytes(), self.archive.read_bytes())
                prepare()
            else:
                self.fail("unexpected SDK operation: " + repr(command))
        return run

    def prepare(self):
        return package.prepare_sources(self.sdk, self.archive, self.version,
                                       self.expected, self.log, self.env)

    def test_clean_recreated_cache_is_replaced_before_prepare(self):
        with patch.object(package, "run", side_effect=self.fake_run(self.write_unpacked)):
            unpacked, cache = self.prepare()
        self.assertEqual((unpacked, cache), (self.unpacked, self.cache))
        self.assertEqual(self.cache.read_bytes(), self.archive.read_bytes())
        self.assertEqual(package.source_hashes(unpacked), self.expected)
        self.assertEqual([command[1] for command in self.calls],
                         ["package/mt-c/clean", "package/mt-c/prepare"])

    def test_corrupt_copy_fails_before_sdk_prepare(self):
        def stale_copy(source, destination):
            Path(destination).write_bytes(b"copy did not stage fresh bytes")
        with patch.object(package, "run", side_effect=self.fake_run(self.write_unpacked)), \
                patch.object(package.shutil, "copyfile", side_effect=stale_copy):
            with self.assertRaisesRegex(ValueError, "archive differs"):
                self.prepare()
        self.assertEqual([command[1] for command in self.calls], ["package/mt-c/clean"])
        self.assertFalse(self.unpacked.exists())

    def test_source_header_makefile_missing_and_extra_changes_fail_hash_gate(self):
        mutations = []
        for name in self.files:
            changed = dict(self.files); changed[name] += b"changed\n"; mutations.append(changed)
        missing = dict(self.files); del missing["src/backend-c/include/magitrickle/ruleset.h"]
        mutations.append(missing)
        extra = dict(self.files); extra["src/backend-c/src/unexpected.c"] = b"/* stale object source */\n"
        mutations.append(extra)
        for files in mutations:
            with self.subTest(files=files):
                shutil.rmtree(self.sdk / "build_dir", ignore_errors=True)
                self.calls.clear()
                stale = self.unpacked / "src/backend-c/build/generic/src/platform/loop.o"
                def prepare_mismatch():
                    self.write_unpacked(files)
                    stale.parent.mkdir(parents=True, exist_ok=True)
                    stale.write_bytes(b"unverified existing output")
                with patch.object(package, "run", side_effect=self.fake_run(prepare_mismatch)):
                    with self.assertRaisesRegex(ValueError, "unpacked sources"):
                        self.prepare()
                self.assertEqual([command[1] for command in self.calls],
                                 ["package/mt-c/clean", "package/mt-c/prepare"])
                self.assertEqual(stale.read_bytes(), b"unverified existing output")

    def test_missing_or_ambiguous_unpack_root_is_rejected(self):
        def duplicate():
            self.write_unpacked()
            self.write_unpacked(root=self.sdk / "build_dir/target-other_musl" / ("mt-c-" + self.version))
        for action in (lambda: None, duplicate):
            with self.subTest(action=action):
                shutil.rmtree(self.sdk / "build_dir", ignore_errors=True)
                with patch.object(package, "run", side_effect=self.fake_run(action)):
                    with self.assertRaisesRegex(ValueError, "unpacked sources"):
                        self.prepare()

    def test_verified_prepare_clears_stale_objects_but_preserves_other_files(self):
        build = self.unpacked / "src/backend-c/build"
        source_sibling = self.unpacked / "src/backend-c/keep.txt"
        target_sibling = self.unpacked.parent / "other-package/keep.o"
        def prepare_outputs():
            self.write_unpacked()
            stale = build / "openwrt/src/platform/loop.o"
            stale.parent.mkdir(parents=True); stale.write_bytes(b"stale baseline object")
            source_sibling.write_bytes(b"source sibling")
            target_sibling.parent.mkdir(); target_sibling.write_bytes(b"other package output")
        with patch.object(package, "run", side_effect=self.fake_run(prepare_outputs)):
            self.assertEqual(self.prepare(), (self.unpacked, self.cache))
        self.assertFalse(build.exists())
        self.assertEqual(package.source_hashes(self.unpacked), self.expected)
        self.assertEqual(source_sibling.read_bytes(), b"source sibling")
        self.assertEqual(target_sibling.read_bytes(), b"other package output")

    def test_build_leaf_symlink_is_unlinked_without_touching_its_target(self):
        external = self.root / "external-output"; external.mkdir()
        sentinel = external / "loop.o"; sentinel.write_bytes(b"external output")
        build = self.unpacked / "src/backend-c/build"
        def prepare_link():
            self.write_unpacked(); build.symlink_to(external, target_is_directory=True)
        with patch.object(package, "run", side_effect=self.fake_run(prepare_link)):
            self.prepare()
        self.assertFalse(build.is_symlink())
        self.assertEqual(sentinel.read_bytes(), b"external output")

    def test_unpack_root_and_backend_parent_cannot_escape_sdk_build_tree(self):
        # Both trees contain the exact expected C/H/Makefile bytes. A source
        # hash match must not authorize removal outside the disposable tree.
        external = self.root / "external-source"
        self.write_unpacked(root=external)
        sentinel = external / "src/backend-c/build/loop.o"
        sentinel.parent.mkdir(); sentinel.write_bytes(b"external source build")
        for escape in ("sdk-build-root", "unpack-root", "backend-parent"):
            with self.subTest(escape=escape):
                sdk_build = self.sdk / "build_dir"
                if sdk_build.is_symlink():
                    sdk_build.unlink()
                else:
                    shutil.rmtree(sdk_build, ignore_errors=True)
                def prepare_link():
                    if escape == "sdk-build-root":
                        outside = self.root / "external-build-tree"
                        target = outside / "target-aarch64_musl" / ("mt-c-" + self.version)
                        target.parent.mkdir(parents=True)
                        target.symlink_to(external, target_is_directory=True)
                        sdk_build.symlink_to(outside, target_is_directory=True)
                    elif escape == "unpack-root":
                        self.unpacked.parent.mkdir(parents=True)
                        self.unpacked.symlink_to(external, target_is_directory=True)
                    else:
                        (self.unpacked / "src").mkdir(parents=True)
                        (self.unpacked / "src/backend-c").symlink_to(
                            external / "src/backend-c", target_is_directory=True)
                    self.assertEqual(package.source_hashes(self.unpacked), self.expected)
                with patch.object(package, "run", side_effect=self.fake_run(prepare_link)):
                    with self.assertRaisesRegex(ValueError, "escapes|disposable"):
                        self.prepare()
                self.assertEqual(sentinel.read_bytes(), b"external source build")


class HostBuildCleanupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.backend = self.root / "source/src/backend-c"
        self.backend.mkdir(parents=True)

    def test_object_selection_excludes_sync_copies_and_program_entrypoints(self):
        output = self.backend / "build/perf-compare/src"
        wanted = ("dns/proxy.o", "platform/loop.o", "rules/nested/match.v1.o")
        unwanted = (".sync/proxy.o", "dns/.sync-123/proxy.o", "platform/.loop.o",
                    "main/main.o", "tools/cli.o", "dns/proxy.d", "dns/proxy.o.tmp",
                    "dns/unrelated.o")
        for name in (*reversed(wanted), *unwanted):
            path = output / name
            path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(b"fixture output")
        other = self.backend / "build/generic/src/dns/proxy.o"
        other.parent.mkdir(parents=True); other.write_bytes(b"other build")
        (self.backend / "Makefile").write_text("LIB_OBJS = " + " ".join(
            "build/$(BUILD)/src/" + name for name in reversed(wanted)) + "\n")
        self.assertEqual(host.production_objects(self.backend, "perf-compare"),
                         sorted(output / name for name in wanted))

    def test_makefile_objects_must_exist_and_stay_inside_expected_variant(self):
        valid = "build/perf-compare/src/dns/proxy.o"
        outside = "build/generic/src/dns/proxy.o"
        entrypoint = "build/perf-compare/src/main/main.o"
        for name in (valid, outside, entrypoint):
            path = self.backend / name
            path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(b"fixture object")
        for objects in ("build/perf-compare/src/missing.o", outside, entrypoint,
                        valid + " " + valid, ""):
            with self.subTest(objects=objects):
                (self.backend / "Makefile").write_text("LIB_OBJS := " + objects + "\n")
                with self.assertRaises(ValueError):
                    host.production_objects(self.backend, "perf-compare")

    def test_removes_only_owned_outputs_and_is_idempotent(self):
        directory = self.backend / "build/perf-compare"
        stale = directory / "src/platform/loop.o"
        stale.parent.mkdir(parents=True); stale.write_bytes(b"old variant")
        sibling = self.backend / "build/generic/keep.o"
        sibling.parent.mkdir(); sibling.write_bytes(b"unrelated build")
        source = self.backend / "Makefile"; source.write_bytes(b"unchanged build recipe")
        self.assertEqual(host.clear_host_build(self.backend, "perf-compare"), directory)
        self.assertFalse(directory.exists())
        self.assertEqual(host.clear_host_build(self.backend, "perf-compare"), directory)
        self.assertEqual(sibling.read_bytes(), b"unrelated build")
        self.assertEqual(source.read_bytes(), b"unchanged build recipe")

    def test_output_leaf_symlink_never_removes_external_target(self):
        external = self.root / "external-output"; external.mkdir()
        sentinel = external / "loop.o"; sentinel.write_bytes(b"external object")
        directory = self.backend / "build/perf-compare"
        directory.parent.mkdir(); directory.symlink_to(external, target_is_directory=True)
        host.clear_host_build(self.backend, "perf-compare")
        self.assertFalse(directory.is_symlink())
        self.assertEqual(sentinel.read_bytes(), b"external object")

    def test_non_owned_names_and_symlinked_build_parent_are_rejected(self):
        external = self.root / "external-build"
        sentinel = external / "perf-compare/loop.o"
        sentinel.parent.mkdir(parents=True); sentinel.write_bytes(b"external object")
        for name in ("generic", "../outside", "/tmp", "perf-compare/../generic", ""):
            with self.subTest(name=name):
                with self.assertRaisesRegex(ValueError, "non-owned"):
                    host.clear_host_build(self.backend, name)
        (self.backend / "build").symlink_to(external, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "non-owned"):
            host.clear_host_build(self.backend, "perf-compare")
        self.assertEqual(sentinel.read_bytes(), b"external object")
        self.assertTrue((self.backend / "build").is_symlink())


if __name__ == "__main__":
    unittest.main()
