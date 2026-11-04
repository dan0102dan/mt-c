#!/usr/bin/env python3
"""Package preservation checks; the emulator verifies the real upgrade lifecycle."""

import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "build-upgrade-fixture.py"
SPEC = importlib.util.spec_from_file_location("upgrade_fixture", SCRIPT)
fixture = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(fixture)

ELF = b"\x7fELF\x02\x01candidate daemon bytes"
INIT = b'#!/bin/sh /etc/rc.common\nprocd_set_param file "$PROG"\n'
HOOK = b'#!/bin/sh\n. /lib/functions.sh\ndefault_postinst "$@"\n'


def make_tar(entries):
    result = io.BytesIO()
    with tarfile.open(fileobj=result, mode="w:gz") as archive:
        for name, body, mode in entries:
            member = tarfile.TarInfo("./" + name)
            member.size = len(body)
            member.mode = mode
            member.uid = member.gid = 0
            member.uname = member.gname = "root"
            member.mtime = 1760000000
            archive.addfile(member, io.BytesIO(body))
    return result.getvalue()


def make_ar(entries):
    result = bytearray(b"!<arch>\n")
    for name, body, mode in entries:
        result.extend(f"{name + '/':<16}{1760000000:<12}{0:<6}{0:<6}{mode:<8o}{len(body):<10}`\n".encode())
        result.extend(body)
        if len(body) % 2:
            result.extend(b"\n")
    return bytes(result)


def unpack_tar(payload):
    return {fixture.member_name(member.name): (member, body)
            for member, body in fixture.tar_members(payload)}


class UpgradeFixtureTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)

    def create_ipk(self, container, binary=ELF):
        control = make_tar([
            ("control", b"Package: mt-c\nVersion: 0.8.6-r1\n"
             b"Architecture: aarch64_cortex-a53\n"
             b"Depends: libc, libcurl4 (>= 8.0)\nInstalled-Size: 1000\n", 0o644),
            ("postinst", HOOK, 0o755),
            ("prerm", b"#!/bin/sh\ndefault_prerm\n", 0o755),
            ("conffiles", b"/etc/config/magitrickle\n", 0o644),
            ("files-sha256sum", hashlib.sha256(binary).hexdigest().encode()
             + b"  /usr/bin/magitrickled\n", 0o644),
            ("md5sums", hashlib.md5(binary).hexdigest().encode()
             + b"  usr/bin/magitrickled\n", 0o644),
        ])
        data = make_tar([
            (fixture.ELF_PATH, binary, 0o755),
            ("etc/init.d/magitrickle", INIT, 0o755),
            ("etc/config/magitrickle", b"unchanged config\n", 0o600),
        ])
        package = self.directory / "candidate.ipk"
        package.write_bytes(container([
            ("debian-binary", b"2.0\n", 0o644),
            ("control.tar.gz", control, 0o644),
            ("data.tar.gz", data, 0o644),
        ]))
        return package

    def test_tar_and_ar_ipk_preserve_hooks_dependencies_and_payload(self):
        for container in (make_tar, make_ar):
            with self.subTest(container=container.__name__):
                package = self.create_ipk(container)
                original = package.read_bytes()
                output = self.directory / "baseline.ipk"
                fixture.build_fixture(package, output)
                self.assertEqual(original, package.read_bytes())
                if container is make_ar:
                    parts = {name: body for name, _, body in fixture.ar_members(output.read_bytes())}
                else:
                    parts = {name: body for name, (_, body) in unpack_tar(output.read_bytes()).items()}
                control = unpack_tar(parts["control.tar.gz"])
                data = unpack_tar(parts["data.tar.gz"])
                self.assertIn(b"Version: 0.0.0-0\n", control["control"][1])
                self.assertIn(b"Depends: libc, libcurl4 (>= 8.0)\n", control["control"][1])
                self.assertIn(b"Architecture: aarch64_cortex-a53\n", control["control"][1])
                self.assertIn(f"Installed-Size: {1000 + len(fixture.ELF_TRAILER)}\n".encode(),
                              control["control"][1])
                self.assertEqual(HOOK, control["postinst"][1])
                self.assertEqual(0o755, control["postinst"][0].mode)
                self.assertEqual(b"#!/bin/sh\ndefault_prerm\n", control["prerm"][1])
                self.assertEqual(b"/etc/config/magitrickle\n", control["conffiles"][1])
                self.assertEqual(ELF + fixture.ELF_TRAILER, data[fixture.ELF_PATH][1])
                self.assertEqual(INIT, data["etc/init.d/magitrickle"][1])
                self.assertEqual(b"unchanged config\n", data["etc/config/magitrickle"][1])
                self.assertEqual(0o600, data["etc/config/magitrickle"][0].mode)
                self.assertEqual((0, 0, "root", "root"),
                                 (data[fixture.ELF_PATH][0].uid, data[fixture.ELF_PATH][0].gid,
                                  data[fixture.ELF_PATH][0].uname, data[fixture.ELF_PATH][0].gname))
                self.assertEqual(hashlib.sha256(ELF + fixture.ELF_TRAILER).hexdigest().encode()
                                 + b"  /usr/bin/magitrickled\n", control["files-sha256sum"][1])
                self.assertEqual(hashlib.md5(ELF + fixture.ELF_TRAILER).hexdigest().encode()
                                 + b"  usr/bin/magitrickled\n", control["md5sums"][1])

    def test_candidate_cannot_be_overwritten_through_a_path_or_hardlink(self):
        package = self.create_ipk(make_tar)
        original = package.read_bytes()
        alias = self.directory / "alias.ipk"
        alias.hardlink_to(package)
        for output in (package, alias):
            with self.subTest(output=output), self.assertRaisesRegex(fixture.FixtureError, "overwrite"):
                fixture.build_fixture(package, output)
        self.assertEqual(original, package.read_bytes())

    def test_non_elf_payload_does_not_publish_an_invalid_fixture(self):
        package = self.create_ipk(make_tar, binary=b"not an executable")
        output = self.directory / "baseline.ipk"
        output.write_bytes(b"existing fixture")
        with self.assertRaisesRegex(fixture.FixtureError, "not an ELF"):
            fixture.build_fixture(package, output)
        self.assertEqual(b"existing fixture", output.read_bytes())

    def test_apk_reuses_full_metadata_and_scripts_with_new_version_and_binary(self):
        package = self.directory / "candidate.apk"
        package.write_bytes(b"original APK")
        output = self.directory / "baseline.apk"
        metadata = {
            "info": {"name": "mt-c", "version": "0.8.6-r1", "hashes": "old-hash",
                     "arch": "aarch64_cortex-a53", "description": "Routing daemon",
                     "installed-size": 1000, "file-size": 500,
                     "depends": ["libc", "libcurl4>=8.0"], "provides": ["mt-c-any"],
                     "license": "GPL-3.0-or-later", "tags": ["openwrt:test=value"]},
            "scripts": {"post-install": HOOK.decode(),
                        "post-upgrade": "#!/bin/sh\nexport PKG_UPGRADE=1\ndefault_postinst\n",
                        "pre-deinstall": "#!/bin/sh\ndefault_prerm\n"},
            "paths": [], "triggers": ["/etc/test/*"], "replaces-priority": 3,
        }
        seen = []

        def run(apk_tool, *args):
            self.assertEqual(Path("/sdk/bin/apk"), apk_tool)
            seen.append(args[0])
            if args[0] == "adbdump":
                return json.dumps(metadata)
            if args[0] == "version":
                self.assertEqual(("version", "-t", "0.0.0-r0", "0.8.6-r1"), args)
                return "<\n"
            if args[0] == "extract":
                self.assertIn("--allow-untrusted", args)
                destination = Path(args[args.index("--destination") + 1])
                for name, body in ((fixture.ELF_PATH, ELF), ("etc/init.d/magitrickle", INIT)):
                    file = destination / name
                    file.parent.mkdir(parents=True, exist_ok=True)
                    file.write_bytes(body)
                return ""
            self.assertEqual("mkpkg", args[0])
            pairs = list(zip(args[1::2], args[2::2]))
            info = dict(value.split(":", 1) for flag, value in pairs if flag == "--info")
            self.assertEqual("0.0.0-r0", info.pop("version"))
            self.assertEqual("3", info.pop("replaces-priority"))
            expected = {key: " ".join(value) if isinstance(value, list) else str(value)
                        for key, value in metadata["info"].items()
                        if key not in ("version", "hashes", "file-size", "installed-size")}
            self.assertEqual(expected, info)
            scripts = dict(value.split(":", 1) for flag, value in pairs if flag == "--script")
            self.assertEqual(metadata["scripts"], {name: Path(path).read_text()
                                                    for name, path in scripts.items()})
            self.assertEqual(metadata["triggers"], [value for flag, value in pairs if flag == "--trigger"])
            payload = Path(args[args.index("--files") + 1])
            self.assertEqual(ELF + fixture.ELF_TRAILER, (payload / fixture.ELF_PATH).read_bytes())
            self.assertEqual(INIT, (payload / "etc/init.d/magitrickle").read_bytes())
            Path(args[args.index("--output") + 1]).write_bytes(b"fixture APK")
            return ""

        with patch.object(fixture, "run_apk", side_effect=run), patch.object(fixture.os, "geteuid", return_value=0):
            fixture.build_fixture(package, output, Path("/sdk/bin/apk"))
        self.assertEqual(["adbdump", "version", "extract", "mkpkg"], seen)
        self.assertEqual(b"fixture APK", output.read_bytes())
        self.assertEqual(b"original APK", package.read_bytes())

    def test_apk_requires_fakeroot_and_preserves_existing_output_on_tool_failure(self):
        package = self.directory / "candidate.apk"
        package.write_bytes(b"original APK")
        output = self.directory / "baseline.apk"
        output.write_bytes(b"existing fixture")
        with patch.object(fixture.os, "geteuid", return_value=1000), patch.object(fixture, "run_apk") as run:
            with self.assertRaisesRegex(fixture.FixtureError, "root or fakeroot"):
                fixture.build_fixture(package, output, Path("/sdk/bin/apk"))
            run.assert_not_called()
        with patch.object(fixture.os, "geteuid", return_value=0), \
                patch.object(fixture, "run_apk", side_effect=subprocess.CalledProcessError(1, ["apk"])):
            with self.assertRaises(subprocess.CalledProcessError):
                fixture.build_fixture(package, output, Path("/sdk/bin/apk"))
        self.assertEqual(b"existing fixture", output.read_bytes())


if __name__ == "__main__":
    unittest.main()
