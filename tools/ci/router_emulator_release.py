#!/usr/bin/env python3
"""Fetch the latest stable Router Emulator for Linux CI, verifying release SHA256 digests.

Uses GitHub's /releases/latest endpoint (which excludes drafts and prereleases).
Assets and their SHA256 digests must both be published by that *same* release.
Never silently fall back to a previous release if the new one is incompatible.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile
import time
import urllib.error
import urllib.request

REPOSITORY = "stavultras/router-emulator"
RELEASE_API = f"https://api.github.com/repos/{REPOSITORY}/releases/latest"
RELEASE_URL = f"https://github.com/{REPOSITORY}/releases/download/"
TAG = re.compile(r"v([0-9]+(?:\.[0-9]+){1,3})\Z")
SHA256 = re.compile(r"sha256:([0-9a-f]{64})\Z")


class ReleaseError(RuntimeError):
    """Missing, untrusted or incompatible release metadata/artifact."""


def parse_release(data):
    """Validate *one* stable release and derive exact asset URLs and hashes."""
    if not isinstance(data, dict) or data.get("draft") is not False or data.get("prerelease") is not False:
        raise ReleaseError("latest release is missing or is not a published stable release")
    tag = data.get("tag_name")
    if not isinstance(tag, str) or not (match := TAG.fullmatch(tag)):
        raise ReleaseError("unexpected router-emulator release tag")
    version = match[1]
    if not isinstance(data.get("assets"), list):
        raise ReleaseError(f"{tag}: release assets are missing")

    selected = {}
    for key, suffix in (("emulator", "linux-x86_64.tar.gz"), ("presets", "presets.zip")):
        name = f"Router-Emulator-{version}-{suffix}"
        matches = [a for a in data["assets"] if isinstance(a, dict) and a.get("name") == name]
        if len(matches) != 1:
            raise ReleaseError(f"{tag}: expected exactly one asset {name}")
        asset = matches[0]
        expected_url = RELEASE_URL + tag + "/" + name
        if asset.get("browser_download_url") != expected_url:
            raise ReleaseError(f"{tag}: unexpected download URL for {name}")
        digest = asset.get("digest")
        if not isinstance(digest, str) or not (sha := SHA256.fullmatch(digest)):
            raise ReleaseError(f"{tag}: {name} has no valid published SHA256 digest")
        size = asset.get("size")
        if type(size) is not int or not 0 < size <= 512 * 1024 * 1024:
            raise ReleaseError(f"{tag}: invalid asset size for {name}")
        if asset.get("state") != "uploaded":
            raise ReleaseError(f"{tag}: {name} is not an uploaded release asset")
        selected[key] = {"name": name, "url": expected_url, "sha256": sha[1], "size": size}
    return {"release_tag": tag, "version": version, "release_id": data.get("id"), "assets": selected}


def urlopen_with_retries(request, *, timeout=90, attempts=3):
    for attempt in range(attempts):
        try:
            return urllib.request.urlopen(request, timeout=timeout)
        except (OSError, urllib.error.URLError) as exc:
            if attempt + 1 == attempts:
                raise ReleaseError(f"failed to access Router Emulator release: {exc}") from exc
            time.sleep(2 ** attempt)


def latest_release(token=None):
    headers = {"Accept": "application/vnd.github+json", "User-Agent": "mt-c-openwrt-ci",
               "X-GitHub-Api-Version": "2022-11-28"}
    if token:
        headers["Authorization"] = "Bearer " + token
    request = urllib.request.Request(RELEASE_API, headers=headers)
    with urlopen_with_retries(request, timeout=30) as response:
        data = json.load(response)
    return parse_release(data)


def download_verified(asset, path):
    """Stream a release asset to a staging path and require exact size/hash."""
    digest = hashlib.sha256()
    count = 0
    request = urllib.request.Request(asset["url"], headers={"User-Agent": "mt-c-openwrt-ci"})
    with urlopen_with_retries(request) as response, path.open("wb") as output:
        while block := response.read(1024 * 1024):
            count += len(block)
            if count > asset["size"]:
                raise ReleaseError(f"{asset['name']}: larger than published release size")
            digest.update(block)
            output.write(block)
    if count != asset["size"] or digest.hexdigest() != asset["sha256"]:
        raise ReleaseError(f"{asset['name']}: size/SHA256 verification failed")


def fetch_into(destination, *, token=None):
    """Resolve once, then publish a pair of fully verified assets and evidence."""
    release = latest_release(token)
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".router-emulator-", dir=destination) as staging:
        staging = Path(staging)
        for key, local_name in (("emulator", "emulator.tar.gz"), ("presets", "presets.zip")):
            download_verified(release["assets"][key], staging / local_name)
        (staging / "router-emulator-release.json").write_text(json.dumps(release, indent=2) + "\n")
        for name in ("emulator.tar.gz", "presets.zip", "router-emulator-release.json"):
            os.replace(staging / name, destination / name)
    return release


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--github-output", type=Path, help="GitHub Actions step output file")
    args = parser.parse_args()
    release = fetch_into(args.destination, token=os.environ.get("GITHUB_TOKEN"))
    if args.github_output is not None:
        with args.github_output.open("a", encoding="utf-8") as output:
            output.write("version=" + release["version"] + "\n")
            output.write("tag=" + release["release_tag"] + "\n")
    print(f"Verified latest stable Router Emulator {release['release_tag']} ")
    for key in ("emulator", "presets"):
        print(f"  {release['assets'][key]['name']}: SHA256 {release['assets'][key]['sha256']}")


if __name__ == "__main__":
    main()
