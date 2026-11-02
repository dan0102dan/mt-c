#!/usr/bin/env python3
"""Offline tests for Router Emulator >=4.1 preset/flash schema."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "router_emulator_preset.py"
spec = importlib.util.spec_from_file_location("router_emulator_preset", SCRIPT)
preset = importlib.util.module_from_spec(spec)
spec.loader.exec_module(preset)


class RouterEmulatorPresetTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / "presets").mkdir()
        (self.root / "tools").mkdir()
        (self.root / "VERSION").write_text("4.1\n")
        self.write_preset("cudy-tr3000-v1", "cudy_tr3000-v1", "efaa21", "nand-tr3000")
        self.write_preset("cudy-wr3000p-v1", "cudy_wr3000p-v1", "c8017f7f7f", "nand-wr3000p")

    def write_preset(self, name, profile, chip, dump_dir, flash_type="nand"):
        (self.root / "presets" / (name + ".ini")).write_text(
            "[preset]\n"
            f"openwrt={profile}\n"
            f"flash-type={flash_type}\n"
            f"flash-id={chip}\n"
            f"dump-dir={dump_dir}\n"
        )

    def test_both_openwrt_fixtures_use_board_specific_chip_id_and_dump_dir(self):
        expected = [
            ("cudy-tr3000-v1", "cudy_tr3000-v1", "efaa21", "nand-tr3000"),
            ("cudy-wr3000p-v1", "cudy_wr3000p-v1", "c8017f7f7f", "nand-wr3000p"),
        ]
        for name, profile, chip, directory in expected:
            with self.subTest(name=name):
                board = preset.read_board_preset(self.root, name, expected_profile=profile)
                self.assertEqual(board["flash_type"], "nand")
                self.assertEqual(board["flash_id"], chip)
                self.assertEqual(board["flash_mb"], 128)
                self.assertEqual(board["dump_dir"], self.root / directory)
                self.assertEqual(board["dump_dir_relative"], directory)
                self.assertEqual(board["emulator_version"], "4.1")

    def test_prepare_uses_chip_identity_from_selected_board_not_a_default(self):
        for name, profile, chip, directory in [
            ("cudy-tr3000-v1", "cudy_tr3000-v1", "efaa21", "nand-tr3000"),
            ("cudy-wr3000p-v1", "cudy_wr3000p-v1", "c8017f7f7f", "nand-wr3000p"),
        ]:
            with self.subTest(name=name):
                def build(command, **kwargs):
                    self.assertEqual(command, [
                        str(self.root / "tools/prepare-nand.sh"), "--flash-type", "nand",
                        "--flash-id", chip, "--flash-mb", "128", profile, "25.12.5", directory,
                    ])
                    self.assertEqual(kwargs, {"cwd": self.root, "check": True})
                    folder = self.root / directory
                    folder.mkdir(exist_ok=True)
                    (folder / "mt7981.mtd0.BL2.bin").write_bytes(b"boot")
                with patch.object(preset.subprocess, "run", side_effect=build) as runner:
                    self.assertEqual(preset.prepare_board(self.root, name, profile, "25.12.5"),
                                     self.root / directory)
                    runner.assert_called_once()

    def test_missing_legacy_or_unsafe_values_fail_before_preparation(self):
        modifications = [
            ("cudy-tr3000-v1", "[preset]\nopenwrt=cudy_tr3000-v1\nnand=128\nnand-dir=nand-tr3000\n"),
            ("cudy-tr3000-v1", "[preset]\nopenwrt=cudy_tr3000-v1\nflash-type=nor\nflash-id=efaa21\ndump-dir=nand-tr3000\n"),
            ("cudy-tr3000-v1", "[preset]\nopenwrt=cudy_tr3000-v1\nflash-type=nand\nflash-id=invalid\ndump-dir=nand-tr3000\n"),
            ("cudy-tr3000-v1", "[preset]\nopenwrt=cudy_tr3000-v1\nflash-type=nand\nflash-id=efaa21\ndump-dir=../escape\n"),
            ("cudy-tr3000-v1", "[preset]\nopenwrt=cudy_tr3000-v1\nflash-type=nand\nflash-id=efaa21\ndump-dir=/tmp/outside\n"),
        ]
        original = (self.root / "presets/cudy-tr3000-v1.ini").read_text()
        for name, altered in modifications:
            with self.subTest(altered=altered):
                (self.root / "presets" / (name + ".ini")).write_text(altered)
                with patch.object(preset.subprocess, "run") as runner:
                    with self.assertRaises(ValueError):
                        preset.prepare_board(self.root, name, "cudy_tr3000-v1", "24.10.4")
                    runner.assert_not_called()
        (self.root / "presets/cudy-tr3000-v1.ini").write_text(original)

    def test_incorrect_profile_unsupported_version_and_preset_paths_are_rejected(self):
        for version in ("3.7", "4.0", "", "invalid"):
            with self.subTest(version=version):
                (self.root / "VERSION").write_text(version)
                with self.assertRaises(ValueError):
                    preset.read_board_preset(self.root, "cudy-tr3000-v1")
        (self.root / "VERSION").write_text("4.1")
        with self.assertRaises(ValueError):
            preset.read_board_preset(self.root, "cudy-tr3000-v1", "wrong_profile")
        for name in ("../../etc/passwd", "../cudy-tr3000-v1", "/tmp/injected", "cudy-tr3000-v1.ini"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                preset.read_board_preset(self.root, name)
        with self.assertRaises(ValueError):
            preset.prepare_board(self.root, "cudy-tr3000-v1", "cudy_tr3000-v1", "snapshot")

    def test_later_versions_use_same_flash_schema_without_manual_bump(self):
        for version in ("4.1", "4.1.0", "4.2", "5.0"):
            with self.subTest(version=version):
                (self.root / "VERSION").write_text(version + "\n")
                board = preset.read_board_preset(self.root, "cudy-tr3000-v1", expected_version=version)
                self.assertEqual(board["emulator_version"], version)
        (self.root / "VERSION").write_text("4.1\n")
        self.assertEqual(preset.read_board_preset(self.root, "cudy-tr3000-v1",
                                                 expected_version="4.1.0")["emulator_version"], "4.1")
        with self.assertRaisesRegex(ValueError, "version mismatch"):
            preset.read_board_preset(self.root, "cudy-tr3000-v1", expected_version="4.2")
        with self.assertRaises(ValueError):
            preset.read_board_preset(self.root, "cudy-tr3000-v1", expected_version="evil")

    def test_prebuilt_release_without_chip_table_uses_verified_fixture_size(self):
        # A release tarball has neither src/qemu/.../flash_chips.h nor
        # qemu-patches/*.patch. An actual executable stub reproduces the
        # upstream prepare-nand.sh contract: an explicit flash size is needed.
        script = self.root / "tools/prepare-nand.sh"
        script.write_text("""#!/bin/sh
set -eu
test "$1" = "--flash-type" && test "$2" = "nand"; shift 2
test "$1" = "--flash-id"; shift 2
test "$1" = "--flash-mb" && test "$2" = "128"; shift 2
test "$1" = "cudy_tr3000-v1" && test "$2" = "24.10.4" && test "$3" = "nand-tr3000"
mkdir -p "$3"
printf x > "$3/mt7981.mtd0.BL2.bin"
""")
        script.chmod(0o755)
        self.assertFalse((self.root / "src/qemu/hw/arm/filogic/flash_chips.h").exists())
        self.assertFalse((self.root / "qemu-patches").exists())
        self.assertEqual(preset.prepare_board(self.root, "cudy-tr3000-v1",
                                              "cudy_tr3000-v1", "24.10.4"),
                         self.root / "nand-tr3000")

    def test_wrong_flash_size_or_unknown_board_fails_before_execution(self):
        path = self.root / "presets/cudy-tr3000-v1.ini"
        original = path.read_text()
        path.write_text(original + "flash-mb=256\n")
        with patch.object(preset.subprocess, "run") as runner:
            with self.assertRaisesRegex(ValueError, "flash-mb conflicts"):
                preset.prepare_board(self.root, "cudy-tr3000-v1",
                                     "cudy_tr3000-v1", "24.10.4")
            runner.assert_not_called()
        path.write_text(original + "flash-mb=128\n")
        board = preset.read_board_preset(self.root, "cudy-tr3000-v1")
        self.assertEqual(board["flash_mb"], 128)
        self.write_preset("unknown-board", "unknown_board-v1", "efaa21", "nand-unknown")
        with patch.object(preset.subprocess, "run") as runner:
            with self.assertRaisesRegex(ValueError, "unsupported NAND fixture"):
                preset.prepare_board(self.root, "unknown-board",
                                     "unknown_board-v1", "24.10.4")
            runner.assert_not_called()

    def test_success_exit_requires_nonempty_generated_dump_directory(self):
        with patch.object(preset.subprocess, "run") as runner:
            with self.assertRaisesRegex(RuntimeError, "did not create flash dumps"):
                preset.prepare_board(self.root, "cudy-tr3000-v1", "cudy_tr3000-v1", "24.10.4")
            runner.assert_called_once()


if __name__ == "__main__":
    unittest.main()
