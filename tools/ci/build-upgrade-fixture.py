#!/usr/bin/env python3
"""Make an older mt-c package for the emulator's real package-upgrade test.

The candidate is never modified. Its payload, dependencies, and lifecycle
scripts are reused; only the package version and an inert ELF trailer change.
The resulting package is a CI fixture, not a release artifact. APK creation
requires the SDK's host apk (with mkpkg) and root or fakeroot for file ownership.
"""

import argparse
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile


ELF_PATH = "usr/bin/magitrickled"
ELF_TRAILER = b"\nmt-c upgrade-test baseline; not a release\n"
IPK_VERSION = "0.0.0-0"
APK_VERSION = "0.0.0-r0"


class FixtureError(RuntimeError):
    pass


def mark_binary(payload):
    if not payload.startswith(b"\x7fELF"):
        raise FixtureError(f"{ELF_PATH} is not an ELF executable")
    # The loader ignores bytes after the ELF's existing sections/segments.
    # This changes its hash without recompiling or changing daemon behavior.
    return payload + ELF_TRAILER


def tar_members(payload):
    with tarfile.open(fileobj=io.BytesIO(payload), mode="r:*") as archive:
        return [(copy.copy(member), archive.extractfile(member).read()
                 if member.isfile() else None) for member in archive]


def member_name(name):
    return name.removeprefix("./")


def write_tar(original, members):
    mode = "w"
    for magic, compression in ((b"\x1f\x8b", "gz"), (b"\xfd7zXZ\x00", "xz"),
                               (b"BZh", "bz2")):
        if original.startswith(magic):
            mode += ":" + compression
            break
    result = io.BytesIO()
    with tarfile.open(fileobj=result, mode=mode) as archive:
        for member, payload in members:
            if payload is not None:
                member.size = len(payload)
                member.pax_headers = dict(member.pax_headers)
                member.pax_headers.pop("size", None)
            archive.addfile(member, io.BytesIO(payload) if payload is not None else None)
    return result.getvalue()


def ar_members(payload):
    """Read the short member names used by Debian-style IPK containers."""
    members = []
    offset = 8
    while offset < len(payload):
        header = payload[offset:offset + 60]
        if len(header) != 60 or header[58:] != b"`\n":
            raise FixtureError("invalid IPK ar member header")
        size = int(header[48:58])
        body = payload[offset + 60:offset + 60 + size]
        if size < 0 or len(body) != size:
            raise FixtureError("truncated IPK ar member")
        name = header[:16].decode("ascii").strip().removesuffix("/")
        if not name or name.startswith(("/", "#1/")):
            raise FixtureError("IPK ar container must use short member names")
        members.append((name, header, body))
        offset += 60 + size + size % 2
    return members


def build_ipk(package, output):
    original = package.read_bytes()
    is_ar = original.startswith(b"!<arch>\n")
    members = ar_members(original) if is_ar else tar_members(original)
    if is_ar:
        parts = {name: body for name, _, body in members}
    else:
        parts = {member_name(member.name): body for member, body in members}
    if parts.get("debian-binary") != b"2.0\n":
        raise FixtureError("IPK is missing debian-binary version 2.0")
    if "data.tar.gz" not in parts or "control.tar.gz" not in parts:
        raise FixtureError("IPK must contain data.tar.gz and control.tar.gz")

    data = tar_members(parts["data.tar.gz"])
    binaries = [i for i, (member, _) in enumerate(data)
                if member_name(member.name) == ELF_PATH]
    if len(binaries) != 1 or data[binaries[0]][1] is None:
        raise FixtureError(f"IPK must contain one regular {ELF_PATH}")
    index = binaries[0]
    binary = mark_binary(data[index][1])
    data[index] = (data[index][0], binary)

    control = tar_members(parts["control.tar.gz"])
    control_count = 0
    for i, (member, payload) in enumerate(control):
        name = member_name(member.name)
        if name == "control" and payload is not None:
            text = payload.decode("utf-8")
            if not re.search(r"^Package: mt-c$", text, re.MULTILINE):
                raise FixtureError("upgrade fixture requires an mt-c package")
            text, count = re.subn(r"^Version: .+$", f"Version: {IPK_VERSION}",
                                  text, flags=re.MULTILINE)
            if count != 1:
                raise FixtureError("IPK control must contain one Version field")
            text = re.sub(r"^Installed-Size: ([0-9]+)$",
                          lambda match: f"Installed-Size: {int(match[1]) + len(ELF_TRAILER)}",
                          text, flags=re.MULTILINE)
            control[i] = (member, text.encode("utf-8"))
            control_count += 1
        elif name in ("files-sha256sum", "md5sums") and payload is not None:
            digest = (hashlib.sha256(binary).hexdigest() if name == "files-sha256sum"
                      else hashlib.md5(binary).hexdigest())
            pattern = rb"^[a-fA-F0-9]+([ \t]+(?:\./|/)?usr/bin/magitrickled)$"
            control[i] = (member, re.sub(pattern, digest.encode() + rb"\1",
                                         payload, flags=re.MULTILINE))
    if control_count != 1:
        raise FixtureError("IPK must contain one control file")
    parts["data.tar.gz"] = write_tar(parts["data.tar.gz"], data)
    parts["control.tar.gz"] = write_tar(parts["control.tar.gz"], control)
    if is_ar:
        result = bytearray(b"!<arch>\n")
        for name, header, _ in members:
            body = parts[name]
            result.extend(header[:48] + f"{len(body):<10}".encode() + header[58:])
            result.extend(body)
            if len(body) % 2:
                result.extend(b"\n")
        output.write_bytes(result)
    else:
        output.write_bytes(write_tar(original, [
            (member, parts[member_name(member.name)]) for member, _ in members
        ]))


def run_apk(apk_tool, *args):
    return subprocess.run([str(apk_tool), *map(str, args)], check=True,
                          stdout=subprocess.PIPE, text=True).stdout


def build_apk(package, output, apk_tool, work):
    if os.geteuid() != 0:
        raise FixtureError("run APK fixture generation under root or fakeroot to preserve ownership")
    metadata = json.loads(run_apk(apk_tool, "adbdump", "--format", "json", package))
    info = metadata["info"]
    if info.get("name") != "mt-c":
        raise FixtureError("upgrade fixture requires an mt-c package")
    if run_apk(apk_tool, "version", "-t", APK_VERSION, info["version"]).strip() != "<":
        raise FixtureError("candidate APK version must be newer than the fixture")
    unknown = set(metadata) - {"info", "paths", "scripts", "triggers", "replaces-priority"}
    if unknown:
        raise FixtureError(f"cannot preserve unknown APK metadata: {sorted(unknown)}")
    payload = work / "payload"
    payload.mkdir()
    run_apk(apk_tool, "extract", "--allow-untrusted", "--destination", payload, package)
    binary = payload / ELF_PATH
    if binary.is_symlink() or not binary.is_file():
        raise FixtureError(f"APK must contain a regular {ELF_PATH}")
    binary.write_bytes(mark_binary(binary.read_bytes()))

    command = ["mkpkg", "--files", payload, "--output", output]
    # mkpkg recomputes content hashes and sizes. All other info fields,
    # including dependency constraints and virtual provides, are retained.
    for key, value in info.items():
        if key in ("hashes", "file-size", "installed-size"):
            continue
        if key == "version":
            value = APK_VERSION
        elif isinstance(value, list):
            value = " ".join(value)
        command.extend(("--info", f"{key}:{value}"))
    if "replaces-priority" in metadata:
        command.extend(("--info", f"replaces-priority:{metadata['replaces-priority']}"))
    scripts = work / "scripts"
    scripts.mkdir()
    for number, (name, contents) in enumerate(metadata.get("scripts", {}).items()):
        script = scripts / str(number)
        script.write_text(contents, encoding="utf-8")
        command.extend(("--script", f"{name}:{script}"))
    for trigger in metadata.get("triggers", []):
        command.extend(("--trigger", trigger))
    run_apk(apk_tool, *command)


def build_fixture(package, output, apk_tool=None):
    package = package.resolve()
    output = output.absolute()
    if package == output.resolve() or (output.exists() and package.samefile(output)):
        raise FixtureError("fixture output must not overwrite the candidate package")
    if not package.is_file() or package.suffix not in (".ipk", ".apk"):
        raise FixtureError("--package must be an existing IPK or APK")
    if output.suffix != package.suffix:
        raise FixtureError("fixture and candidate must have the same package format")
    if package.suffix == ".apk" and apk_tool is None:
        raise FixtureError("--apk-tool must point to the SDK host apk for APK fixtures")
    output.parent.mkdir(parents=True, exist_ok=True)
    # Keep incomplete fixtures private and publish the result atomically.
    with tempfile.TemporaryDirectory(prefix="mt-c-upgrade-", dir=output.parent) as temporary:
        work = Path(temporary)
        result = work / output.name
        if package.suffix == ".ipk":
            build_ipk(package, result)
        else:
            build_apk(package, result, apk_tool, work)
        result.replace(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--apk-tool", type=Path)
    args = parser.parse_args()
    try:
        build_fixture(args.package, args.output, args.apk_tool)
    except (FixtureError, OSError, ValueError, KeyError, tarfile.TarError,
            subprocess.CalledProcessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print(f"Created upgrade fixture: {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
