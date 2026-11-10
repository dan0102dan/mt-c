#!/usr/bin/env python3
"""Validate an SDK-built mt-c Entware IPK without modifying it.

The SDK recipe must supply the repository's canonical, self-contained hook.
Execute it with /opt paths rebound to a temporary fixture; no host service
is started. Both tar.gz and ar IPK containers are supported.
"""

import argparse
import copy
import io
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tarfile
import tempfile


class PackageError(RuntimeError):
    pass


def safe_name(name):
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts or any(ord(c) < 32 for c in name):
        raise PackageError(f"unsafe archive path: {name!r}")
    return "" if str(path) == "." else str(path)


def read_tar(payload, label):
    entries, names = [], set()
    with tarfile.open(fileobj=io.BytesIO(payload), mode="r:gz") as archive:
        for entry in archive:
            name = safe_name(entry.name)
            if name in names:
                raise PackageError(f"duplicate {label} member: {name}")
            if not (entry.isfile() or entry.isdir()) or (not name and not entry.isdir()):
                raise PackageError(f"unsupported {label} member type: {name}")
            names.add(name)
            body = archive.extractfile(entry).read() if entry.isfile() else None
            entries.append((copy.copy(entry), body))
    return entries


def read_ar(payload):
    entries, names = [], set()
    offset = 8
    while offset < len(payload):
        header = payload[offset:offset + 60]
        if len(header) != 60 or header[58:] != b"`\n":
            raise PackageError("invalid IPK ar header")
        name = header[:16].decode("ascii").strip().removesuffix("/")
        if not name or name.startswith(("/", "#1/")):
            raise PackageError("IPK ar members must use short filenames")
        name = safe_name(name)
        if name in names:
            raise PackageError(f"duplicate IPK member: {name}")
        size = int(header[48:58])
        end = offset + 60 + size
        if size < 0 or end + size % 2 > len(payload):
            raise PackageError("truncated IPK ar member")
        if size % 2 and payload[end:end + 1] != b"\n":
            raise PackageError("invalid IPK ar padding")
        entries.append((name, header, payload[offset + 60:end]))
        names.add(name)
        offset = end + size % 2
    return entries


def read_package(package):
    original = package.read_bytes()
    is_ar = original.startswith(b"!<arch>\n")
    entries = read_ar(original) if is_ar else read_tar(original, "IPK")
    if is_ar:
        parts = {name: body for name, _, body in entries}
    else:
        if any(body is None and safe_name(entry.name) for entry, body in entries):
            raise PackageError("unexpected directory in IPK container")
        parts = {safe_name(entry.name): body for entry, body in entries if body is not None}
    if set(parts) != {"debian-binary", "control.tar.gz", "data.tar.gz"} or parts["debian-binary"] != b"2.0\n":
        raise PackageError("IPK must contain debian-binary 2.0, control.tar.gz, and data.tar.gz")

    control = read_tar(parts["control.tar.gz"], "control")
    files = {safe_name(entry.name): (entry, body) for entry, body in control}
    if "control" not in files or files["control"][1] is None:
        raise PackageError("IPK has no regular control file")
    fields = files["control"][1].decode("utf-8")
    if re.findall(r"^Package:[ \t]*(.*)$", fields, re.MULTILINE) != ["mt-c"]:
        raise PackageError("checker only accepts the mt-c package")
    for field in ("Version", "Architecture"):
        values = re.findall(rf"^{field}:[ \t]*(.*)$", fields, re.MULTILINE)
        if len(values) != 1 or not values[0].strip():
            raise PackageError(f"control must contain exactly one {field} field")
    if "postinst" not in files or files["postinst"][1] is None:
        raise PackageError("IPK has no regular postinst")

    data = read_tar(parts["data.tar.gz"], "payload")
    payload = {safe_name(entry.name): (entry, body) for entry, body in data}
    if any(name and name != "opt" and not name.startswith("opt/") for name in payload):
        raise PackageError("checker only accepts an Entware /opt payload")
    for name in ("opt/bin/magitrickled", "opt/etc/init.d/S99magitrickle"):
        if name not in payload or payload[name][1] is None or not payload[name][0].mode & 0o111:
            raise PackageError(f"Entware payload is missing executable {name}")
    if not payload["opt/bin/magitrickled"][1].startswith(b"\x7fELF"):
        raise PackageError("Entware magitrickled is not an ELF executable")
    return original, is_ar, entries, parts, control


def canonical_postinst(path):
    contents = path.read_bytes()
    if not contents.startswith(b"#!/bin/sh\n") or b"\x00" in contents:
        raise PackageError("canonical postinst must be a /bin/sh script")
    contents.decode("utf-8")
    return contents


def check_postinst_runtime(contents):
    """Execute the real hook logic with only its /opt paths relocated.

    The init fixture records calls and supplies a known result. No functions.sh
    is provided: a generated SDK wrapper must fail this check by skipping init.
    This checks the maintainer-script contract, not the router daemon itself.
    """
    with tempfile.TemporaryDirectory(prefix="mt-c-postinst-") as temporary:
        root = Path(temporary)
        init = root / "opt/etc/init.d/S99magitrickle"
        init.parent.mkdir(parents=True)
        init.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$MTC_INIT_TRACE"\nexit "$MTC_INIT_RESULT"\n')
        init.chmod(0o755)
        hook = root / "mt-c.postinst"
        hook.write_text(contents.decode("utf-8").replace("/opt/", f"{root}/opt/"))
        trace = root / "init-calls"
        cases = (
            ("install", {"PKG_ROOT": "/", "PKG_UPGRADE": "0"}, 0, ["restart"]),
            ("upgrade", {"PKG_ROOT": "/", "PKG_UPGRADE": "1"}, 0, ["restart"]),
            ("restart failure", {"PKG_ROOT": "/", "PKG_UPGRADE": "1", "MTC_INIT_RESULT": "23"}, 23, ["restart"]),
            ("scripts disabled", {"IPKG_NO_SCRIPT": "1"}, 0, []),
            ("offline install", {"IPKG_INSTROOT": str(root / "offline")}, 0, []),
        )
        for name, settings, expected_status, expected_calls in cases:
            trace.unlink(missing_ok=True)
            env = {key: value for key, value in os.environ.items()
                   if key not in ("IPKG_NO_SCRIPT", "IPKG_INSTROOT", "PKG_ROOT", "PKG_UPGRADE")}
            env.update(MTC_INIT_TRACE=str(trace), MTC_INIT_RESULT="0")
            env.update(settings)
            result = subprocess.run(["/bin/sh", str(hook), "configure"], env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, timeout=10)
            calls = trace.read_text().splitlines() if trace.exists() else []
            if result.returncode != expected_status or calls != expected_calls:
                raise PackageError(f"postinst {name}: exit {result.returncode}, init calls {calls}; "
                                   f"expected exit {expected_status}, calls {expected_calls}")


def check_package(package, postinst):
    _, _, _, _, control = read_package(package)
    canonical = canonical_postinst(postinst)
    packaged_hook = None
    for entry, body in control:
        name = safe_name(entry.name)
        if name == "postinst-pkg":
            raise PackageError("final Entware IPK must not contain postinst-pkg")
        if name == "postinst":
            if body != canonical or entry.mode != 0o755:
                raise PackageError("final Entware postinst must match the canonical hook with mode 0755")
            packaged_hook = body
    check_postinst_runtime(packaged_hook)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--postinst", type=Path, required=True)
    args = parser.parse_args()
    try:
        check_package(args.package, args.postinst)
        print(f"PASS: {args.package} has a self-contained Entware postinst; runtime hook checks passed")
    except (PackageError, OSError, ValueError, tarfile.TarError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
