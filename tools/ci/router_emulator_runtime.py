#!/usr/bin/env python3
"""Run the selected Router Emulator on the host without replacing host glibc.

The x86-64 release bundles non-libc libraries, but not glibc. Probe the actual
QEMU executable, not emulator.sh -V. If it cannot run, build the SAME release
with its upstream build.sh on this host. Never select an older release or an
unpatched system QEMU. Called by router_emulator_preset.py before flash setup.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

UPSTREAM = "https://github.com/stavultras/router-emulator.git"
BUILD_PACKAGES = (
    "build-essential", "git", "ninja-build", "meson", "pkg-config",
    "python3", "python3-venv", "libglib2.0-dev", "libpixman-1-dev",
    "libslirp-dev", "libfdt-dev", "libusb-1.0-0-dev", "zlib1g-dev",
)
CONFIGURE_ARGS = (
    "--disable-gtk --disable-sdl --disable-opengl --disable-vnc "
    "--disable-spice --disable-curl --audio-drv-list="
)


class RuntimeFailure(RuntimeError):
    pass


def version_numbers(value):
    if not isinstance(value, str) or not re.fullmatch(r"[0-9]+(?:\.[0-9]+)+", value):
        raise RuntimeFailure(f"invalid Router Emulator version: {value!r}")
    numbers = [int(n) for n in value.split(".")]
    while len(numbers) > 2 and numbers[-1] == 0:
        numbers.pop()
    return tuple(numbers)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while block := source.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def checked(command, log, *, cwd=None, env=None, timeout=2400):
    log.write("$ " + " ".join(str(word) for word in command) + "\n")
    log.flush()
    try:
        subprocess.run(command, cwd=cwd, env=env, stdout=log,
                       stderr=subprocess.STDOUT, check=True, timeout=timeout)
    except (OSError, subprocess.SubprocessError) as exc:
        raise RuntimeFailure(f"command failed: {command[0]}; see {log.name}: {exc}") from exc


def probe(binary, log):
    """Check dynamic loader AND the patched machine; -V checks neither."""
    if not binary.is_file() or not os.access(binary, os.X_OK):
        return False
    for args in (("--version",), ("-machine", "help")):
        try:
            result = subprocess.run([str(binary), *args], capture_output=True,
                                    text=True, errors="replace", timeout=30)
        except (OSError, subprocess.SubprocessError) as exc:
            log.write(f"QEMU probe failed: {exc}\n"); log.flush()
            return False
        log.write(f"$ {binary} {' '.join(args)}\n{result.stdout}{result.stderr}\n")
        log.flush()
        if result.returncode != 0:
            return False
        if args[0] == "-machine" and not re.search(r"(?m)^mt7981-router\s", result.stdout):
            log.write("QEMU lacks the required mt7981-router machine\n"); log.flush()
            return False
    return True


def checkout_source(source, tag, log):
    checked(["git", "clone", "--depth", "1", "--single-branch", "--branch", tag,
             UPSTREAM, str(source)], log, timeout=300)
    commit = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"],
                                     text=True, timeout=30).strip()
    tagged = subprocess.check_output(["git", "-C", str(source), "rev-parse",
                                     f"refs/tags/{tag}^{{commit}}"], text=True, timeout=30).strip()
    if not re.fullmatch(r"[0-9a-f]{40}", commit) or commit != tagged:
        raise RuntimeFailure("source checkout does not match the selected release tag")
    return commit


def ensure_runtime(emulator_dir, expected_version=None, *, install_build_deps=False, jobs=2):
    root = Path(emulator_dir).resolve()
    version = (root / "VERSION").read_text().strip()
    if expected_version is None:
        expected_version = version
    if version_numbers(version) != version_numbers(expected_version):
        raise RuntimeFailure("release VERSION differs from the selected release")
    if type(jobs) is not int or not 1 <= jobs <= 32:
        raise RuntimeFailure("build jobs must be an integer in 1..32")
    if os.environ.get("QEMU_BIN"):
        raise RuntimeFailure("unset QEMU_BIN: the CI runtime must belong to the selected release")
    tag = "v" + expected_version
    work = root / "work"
    work.mkdir(parents=True, exist_ok=True)
    metadata_path = work / "ci-emulator-runtime.log"  # existing CI *.log artifact glob
    log_path = work / "ci-emulator-runtime-build.log"
    link = root / "src/qemu"
    binary = root / "qemu/qemu-system-aarch64"
    metadata = {"release_tag": tag, "version": version, "status": "checking"}
    source = None
    published = False
    with log_path.open("a", encoding="utf-8") as log:
        try:
            if link.exists() or link.is_symlink():
                # Never silently run an unrelated/stale local source build.
                previous = json.loads(metadata_path.read_text()) if metadata_path.is_file() else {}
                built = link / "build/qemu-system-aarch64"
                if (previous.get("status") != "ready" or previous.get("release_tag") != tag or
                        previous.get("origin") != "source" or not built.is_file() or
                        sha256(built) != previous.get("qemu_sha256") or not probe(built, log)):
                    raise RuntimeFailure("unverified local QEMU build; use a fresh emulator directory")
                return built
            if probe(binary, log):
                metadata.update(origin="release", status="ready", qemu_sha256=sha256(binary))
                print(f"Router Emulator {tag}: release QEMU is compatible with this host", flush=True)
                return binary
            print(f"Router Emulator {tag}: prebuilt QEMU cannot run here; "
                  "building the same release against host libraries", flush=True)
            if install_build_deps:
                prefix = [] if os.geteuid() == 0 else ["sudo", "-n"]
                checked([*prefix, "apt-get", "update"], log, timeout=300)
                checked([*prefix, "apt-get", "install", "-y", "--no-install-recommends",
                         *BUILD_PACKAGES], log, timeout=600)
            source = Path(tempfile.mkdtemp(prefix="runtime-source-", dir=work))
            commit = checkout_source(source, tag, log)
            if version_numbers((source / "VERSION").read_text().strip()) != version_numbers(version):
                raise RuntimeFailure("source VERSION differs from the selected release")
            env = dict(os.environ, JOBS=str(jobs), CONFIGURE_ARGS=CONFIGURE_ARGS, PGO_NAND="")
            checked(["bash", str(source / "build.sh")], log, cwd=source, env=env)
            built = source / "src/qemu/build/qemu-system-aarch64"
            if not probe(built, log):
                raise RuntimeFailure(f"rebuilt QEMU failed runtime preflight; see {log_path}")
            metadata.update(origin="source", source_commit=commit, source_directory=str(source),
                            configure_args=CONFIGURE_ARGS, qemu_sha256=sha256(built))
            # Upstream emulator.sh prefers this exact source-build path. Do not
            # copy its too-new bundled libs or globally change LD_LIBRARY_PATH.
            link.parent.mkdir(parents=True, exist_ok=True)
            link.symlink_to(source / "src/qemu", target_is_directory=True)
            published = True
            metadata["status"] = "ready"
            print(f"Router Emulator {tag}: host-native QEMU ready (source {commit})", flush=True)
            return link / "build/qemu-system-aarch64"
        except Exception:
            metadata["status"] = "failed"
            raise
        finally:
            if source is not None and not published:
                shutil.rmtree(source)
            # Do not overwrite valid provenance when reusing a verified build.
            if metadata["status"] != "checking":
                metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
