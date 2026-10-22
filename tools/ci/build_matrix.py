#!/usr/bin/env python3
"""One source of truth for full, PR, and on-demand package build matrices.

PR packages must match an existing config/<platform>/<architecture>.config
and (for OpenWrt) an actually supported release/format combination.
"""
import json
import os
from pathlib import Path
import sys

SNAPSHOT_ONLY_ARCHS = {"mips64_mips64r2", "mips64el_mips64r2"}
OPENWRT_RELEASES = (("24.10.4", "ipk"), ("25.12.5", "apk"))


def collect_configs(root: Path):
    result = []
    for config in sorted((root / "config").glob("*/*.config")):
        platform = config.parent.name
        if platform not in ("openwrt", "entware"):
            raise ValueError(f"Unknown config platform: {platform}")
        item = {
            "config": config.relative_to(root).as_posix(),
            "name": config.stem,
            "label": config.stem,
            "openwrt_version": "",
            "package_ext": "ipk",
        }
        if platform == "openwrt":
            versions = (("snapshot", "apk"),) if config.stem in SNAPSHOT_ONLY_ARCHS else OPENWRT_RELEASES
            for version, package_ext in versions:
                result.append({
                    **item,
                    "label": f"{config.stem} / OpenWrt {version}",
                    "openwrt_version": version,
                    "package_ext": package_ext,
                })
        else:
            result.append(item)
    if not result:
        raise ValueError("No build configs found")
    return result


def select_configs(matrix, *, event_name="", pr_number="", platform="", architecture="", openwrt_version=""):
    if pr_number:
        if not pr_number.isascii() or not pr_number.isdecimal() or int(pr_number) < 1:
            raise ValueError("Invalid PR number")
        if platform not in ("openwrt", "entware"):
            raise ValueError("Choose platform: openwrt or entware")
        # Match the filename from the actual branch's config tree. Never
        # interpolate an arbitrary path from a manual workflow input.
        if not architecture or not all(c.isascii() and (c.isalnum() or c in "._+-") for c in architecture):
            raise ValueError("Invalid architecture")
        expected = f"config/{platform}/{architecture}.config"
        selected = [
            item for item in matrix
            if item["config"] == expected
            and (platform == "entware" or item["openwrt_version"] == openwrt_version)
        ]
        if len(selected) != 1:
            raise ValueError(
                f"No build target for {expected} / {openwrt_version}. "
                "Select an architecture under config/ for this PR and a supported OpenWrt release."
            )
        return selected
    if event_name == "pull_request":
        selected = [
            item for item in matrix
            if item["config"] == "config/openwrt/aarch64_cortex-a53.config"
            and item["openwrt_version"] == "25.12.5"
        ]
        if len(selected) != 1:
            raise ValueError("Missing OpenWrt 25.12.5 A53 emulator test target")
        return selected
    return matrix


def main():
    try:
        matrix = select_configs(
            collect_configs(Path.cwd()),
            event_name=os.environ.get("GITHUB_EVENT_NAME", ""),
            pr_number=os.environ.get("MANUAL_PR_NUMBER", ""),
            platform=os.environ.get("MANUAL_PLATFORM", ""),
            architecture=os.environ.get("MANUAL_ARCHITECTURE", ""),
            openwrt_version=os.environ.get("MANUAL_OPENWRT_VERSION", ""),
        )
    except ValueError as exc:
        print(f"::error::{exc}", file=sys.stderr)
        return 1
    print(f"configs={json.dumps(matrix, separators=(',', ':'))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
