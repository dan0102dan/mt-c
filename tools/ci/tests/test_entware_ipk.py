#!/usr/bin/env python3
"""Exercise Entware package finalization and the packaged postinst contract."""

from contextlib import redirect_stdout
import importlib.util
import io
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "finalize-entware-ipk.py"
POSTINST = SCRIPT.parents[2] / "files/entware/_ipk/control/postinst"
SPEC = importlib.util.spec_from_file_location("entware_ipk", SCRIPT)
package = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(package)

SDK_POSTINST = (b'#!/bin/sh\n[ "${IPKG_NO_SCRIPT}" = "1" ] && exit 0\n'
                b'[ -s /opt/lib/functions.sh ] || exit 0\n'
                b'. /opt/lib/functions.sh\ndefault_postinst $0 $@\n')
SDK_PRERM = (b'#!/bin/sh\n[ -s /opt/lib/functions.sh ] || exit 0\n'
             b'. /opt/lib/functions.sh\ndefault_prerm $0 $@\n')
OLD_RAW_POSTINST = b'#!/bin/sh\n/opt/etc/init.d/S99magitrickle restart\nexit 0\n'


def make_tar(entries):
    result = io.BytesIO()
    with tarfile.open(fileobj=result, mode="w:gz") as archive:
        for name, body, mode in entries:
            member = tarfile.TarInfo(name if name.startswith("/") else "./" + name)
            member.size = len(body)
            member.mode = mode
            member.uid, member.gid = 7, 8
            member.uname, member.gname = "test-owner", "test-group"
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


def member_snapshot(entries):
    return {package.safe_name(entry.name): (
        body, entry.mode, entry.uid, entry.gid, entry.uname, entry.gname,
        entry.mtime, entry.type, entry.linkname, entry.pax_headers,
    ) for entry, body in entries}


class EntwareIPKTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.input = self.directory / "sdk.ipk"
        self.output = self.directory / "final.ipk"

    def create_package(self, container=make_tar, nested=True, hook=SDK_POSTINST,
                       name="mt-c", prefix="opt", extra_control=(), extra_data=(),
                       hook_mode=0o600):
        control = [
            ("control", (f"Package: {name}\nVersion: 0.8.6-r1\n"
                         "Architecture: mipsel-3.4\nDepends: libc, socat\n"
                         "Provides: mt-c-any\nInstalled-Size: 12345\n"
                         "Description: DNS routing\n").encode(), 0o644),
        ]
        control.extend([
            ("postinst", hook, hook_mode),
            ("prerm", SDK_PRERM, 0o751),
            ("conffiles", b"/opt/var/lib/magitrickle/config.yaml\n", 0o644),
            ("postrm", b"#!/bin/sh\nexit 0\n", 0o755),
            ("files-sha256sum", b"existing checksums stay unchanged\n", 0o644),
        ])
        if nested:
            control.append(("postinst-pkg", OLD_RAW_POSTINST, 0o755))
        control.extend(extra_control)
        data = [
            (f"{prefix}/bin/magitrickled", b"\x7fELFactual package bytes", 0o755),
            (f"{prefix}/etc/init.d/S99magitrickle", b"#!/bin/sh\nexit 0\n", 0o755),
            (f"{prefix}/var/lib/magitrickle/config.yaml", b"preserved: true\n", 0o600),
        ]
        data.extend(extra_data)
        self.input.write_bytes(container([
            ("debian-binary", b"2.0\n", 0o644),
            ("control.tar.gz", make_tar(control), 0o644),
            ("data.tar.gz", make_tar(data), 0o644),
        ]))

    def test_both_formats_preserve_payload_and_all_unrelated_control_metadata(self):
        for container in (make_tar, make_ar):
            for nested in (True, False):
                with self.subTest(container=container.__name__, nested=nested):
                    self.create_package(container, nested)
                    original = self.input.read_bytes()
                    _, _, _, original_parts, original_control = package.read_package(self.input)
                    package.finalize_package(self.input, self.output, POSTINST)
                    _, is_ar, _, final_parts, final_control = package.read_package(self.output)
                    self.assertEqual(container is make_ar, is_ar)
                    self.assertEqual(original, self.input.read_bytes())
                    self.assertEqual(original_parts["data.tar.gz"], final_parts["data.tar.gz"])
                    before, after = member_snapshot(original_control), member_snapshot(final_control)
                    self.assertNotIn("postinst-pkg", after)
                    self.assertEqual(POSTINST.read_bytes(), after["postinst"][0])
                    self.assertEqual(0o755, after["postinst"][1])
                    self.assertEqual(before["postinst"][2:], after["postinst"][2:])
                    for name in set(before) - {"postinst", "postinst-pkg"}:
                        self.assertEqual(before[name], after[name], name)
                    package.check_package(self.output, POSTINST)

    def test_canonical_hook_runs_install_upgrade_failure_and_skip_contracts(self):
        package.check_postinst_runtime(POSTINST.read_bytes())

    def test_original_sdk_wrapper_fails_runtime_check_without_functions_sh(self):
        with self.assertRaisesRegex(package.PackageError, r"postinst install:.*init calls \[\]"):
            package.check_postinst_runtime(SDK_POSTINST)

    def test_runtime_check_rejects_hook_which_hides_a_restart_failure(self):
        with self.assertRaisesRegex(package.PackageError, "postinst restart failure: exit 0"):
            package.check_postinst_runtime(OLD_RAW_POSTINST)

    def test_already_finalized_package_can_be_finalized_in_place(self):
        self.create_package(nested=False, hook=POSTINST.read_bytes(), hook_mode=0o755)
        package.check_package(self.input, POSTINST)
        payload = package.read_package(self.input)[3]["data.tar.gz"]
        package.finalize_package(self.input, self.input, POSTINST)
        package.check_package(self.input, POSTINST)
        self.assertEqual(payload, package.read_package(self.input)[3]["data.tar.gz"])

    def test_check_only_cli_validates_final_artifact_without_rewriting_it(self):
        self.create_package()
        package.finalize_package(self.input, self.output, POSTINST)
        original = self.output.read_bytes()
        with patch("sys.argv", [str(SCRIPT), "--package", str(self.output),
                                "--postinst", str(POSTINST), "--check-only"]), \
                redirect_stdout(io.StringIO()) as stdout:
            self.assertEqual(0, package.main())
        self.assertIn("runtime hook checks passed", stdout.getvalue())
        self.assertEqual(original, self.output.read_bytes())

    def test_check_rejects_generated_wrapper_and_retained_nested_hook(self):
        for hook, nested in ((SDK_POSTINST, False), (POSTINST.read_bytes(), True)):
            with self.subTest(nested=nested):
                self.create_package(hook=hook, nested=nested, hook_mode=0o755)
                with self.assertRaises(package.PackageError):
                    package.check_package(self.input, POSTINST)

    def test_wrong_package_and_openwrt_payload_are_rejected_before_output_changes(self):
        for changes in ({"name": "other"}, {"prefix": "usr"}):
            with self.subTest(changes=changes):
                self.create_package(**changes)
                self.output.write_bytes(b"existing output")
                with self.assertRaises(package.PackageError):
                    package.finalize_package(self.input, self.output, POSTINST)
                self.assertEqual(b"existing output", self.output.read_bytes())

    def test_unsafe_paths_and_duplicate_members_are_rejected(self):
        for changes in (
            {"extra_control": [("../postinst", b"bad", 0o755)]},
            {"extra_control": [("postinst", b"duplicate", 0o755)]},
            {"extra_data": [("opt/../../etc/passwd", b"bad", 0o644)]},
            {"extra_data": [("/opt/absolute", b"bad", 0o644)]},
        ):
            with self.subTest(changes=changes):
                self.create_package(**changes)
                with self.assertRaises(package.PackageError):
                    package.finalize_package(self.input, self.output, POSTINST)
                self.assertFalse(self.output.exists())

    def test_truncated_ar_and_payload_links_are_rejected(self):
        self.create_package(make_ar)
        self.input.write_bytes(self.input.read_bytes()[:-10])
        with self.assertRaisesRegex(package.PackageError, "truncated"):
            package.finalize_package(self.input, self.output, POSTINST)
        archive = io.BytesIO()
        with tarfile.open(fileobj=archive, mode="w:gz") as tar:
            link = tarfile.TarInfo("opt/bin/magitrickled")
            link.type = tarfile.SYMTYPE
            link.linkname = "/outside"
            tar.addfile(link)
        with self.assertRaisesRegex(package.PackageError, "member type"):
            package.read_tar(archive.getvalue(), "payload")


if __name__ == "__main__":
    unittest.main()
