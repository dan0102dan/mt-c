#!/usr/bin/env python3
"""Prepare an OpenWrt board using the selected Router Emulator release preset.

Router Emulator >=4.1 specifies flash-type/flash-id/dump-dir. Refuse unsupported
or unsafe presets rather than silently preparing a different flash chip.
"""
import argparse
import configparser
from pathlib import Path
import re
import subprocess

MIN_EMULATOR_VERSION = (4, 1)
VERSION_PATTERN = re.compile(r"[0-9]+(?:\.[0-9]+)+\Z")


def version_tuple(version):
    if not isinstance(version, str) or not VERSION_PATTERN.fullmatch(version):
        raise ValueError(f"invalid Router Emulator version: {version!r}")
    numbers = [int(part) for part in version.split(".")]
    while len(numbers) > 2 and numbers[-1] == 0:
        numbers.pop()
    return tuple(numbers)


def read_board_preset(emulator_dir, name, expected_profile=None, expected_version=None):
    root = Path(emulator_dir).resolve()
    version_file = root / "VERSION"
    if not version_file.is_file():
        raise ValueError(f"missing Router Emulator VERSION in {version_file}")
    emulator_version = version_file.read_text().strip()
    if version_tuple(emulator_version) < MIN_EMULATOR_VERSION:
        raise ValueError(f"Router Emulator >=4.1 is required for flash-type/dump-dir: {emulator_version}")
    if expected_version is not None and version_tuple(emulator_version) != version_tuple(expected_version):
        raise ValueError(f"Router Emulator version mismatch: downloaded {expected_version}, extracted {emulator_version}")
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]*", name):
        raise ValueError("preset must be a bundled filename, without path components")

    filename = root / "presets" / (name + ".ini")
    parser = configparser.ConfigParser(interpolation=None)
    if not parser.read(filename) or not parser.has_section("preset"):
        raise ValueError(f"missing [preset] in {filename}")
    preset = parser["preset"]
    needed = ("flash-type", "flash-id", "dump-dir", "openwrt")
    if any(not preset.get(key, "").strip() for key in needed):
        raise ValueError(f"{filename}: expected flash-type/flash-id/dump-dir/openwrt keys")
    if preset["flash-type"] != "nand":
        raise ValueError(f"{filename}: the mt-c OpenWrt fixture requires NAND")
    flash_id = preset["flash-id"].strip()
    if not re.fullmatch(r"[0-9a-fA-F]{6,16}", flash_id):
        raise ValueError(f"{filename}: invalid NAND chip ID")
    if expected_profile is not None and preset["openwrt"] != expected_profile:
        raise ValueError(f"{filename}: OpenWrt profile does not match {expected_profile}")

    relative = Path(preset["dump-dir"].strip())
    if relative.is_absolute() or any(part in (".", "..") for part in relative.parts):
        raise ValueError(f"{filename}: dump-dir must be within the emulator")
    dump_dir = (root / relative).resolve()
    if not dump_dir.is_relative_to(root) or dump_dir == root:
        raise ValueError(f"{filename}: dump-dir escapes the emulator")
    return {"flash_type": "nand", "flash_id": flash_id.lower(), "dump_dir": dump_dir,
            "dump_dir_relative": str(relative), "openwrt_profile": preset["openwrt"],
            "emulator_version": emulator_version}


def prepare_board(emulator_dir, preset, profile, openwrt_version, expected_version=None):
    root = Path(emulator_dir).resolve()
    board = read_board_preset(root, preset, expected_profile=profile, expected_version=expected_version)
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){2}", openwrt_version):
        raise ValueError("an explicit OpenWrt release version is required")
    command = [str(root / "tools/prepare-nand.sh"),
               "--flash-type", board["flash_type"], "--flash-id", board["flash_id"],
               profile, openwrt_version, board["dump_dir_relative"]]
    subprocess.run(command, cwd=root, check=True)
    if not board["dump_dir"].is_dir() or not any(board["dump_dir"].iterdir()):
        raise RuntimeError(f"prepare-nand did not create flash dumps: {board['dump_dir']}")
    return board["dump_dir"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator-dir", required=True, type=Path)
    parser.add_argument("--preset", required=True)
    parser.add_argument("--profile", required=True)
    parser.add_argument("--openwrt-version", required=True)
    parser.add_argument("--expected-version", help="version resolved via latest stable GitHub release")
    args = parser.parse_args()
    prepare_board(args.emulator_dir, args.preset, args.profile, args.openwrt_version, args.expected_version)


if __name__ == "__main__":
    main()
