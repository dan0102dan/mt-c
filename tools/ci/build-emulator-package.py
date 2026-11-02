#!/usr/bin/env python3
"""Build a comparison package with the normal OpenWrt SDK recipe.

The caller supplies an official SDK acquired by download-openwrt-sdk.sh.
--prepare-only stages its normal feed dependencies once; subsequent builds
use NO_DEPS=1 so swapping baseline/candidate does not rebuild the kernel.
No compiler flags, runtime dependency list or package layout are substituted.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile


REPO = Path(__file__).resolve().parents[2]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_hashes(source):
    root = source / "src/backend-c"
    return {str(p.relative_to(source)): sha256(p) for p in sorted(root.rglob("*"))
            if p.is_file() and "build" not in p.relative_to(root).parts
            and (p.suffix in (".c", ".h") or p.name == "Makefile")}


def packaged_binary(sdk, package):
    """Hash the actual stripped package payload independently of SDK staging."""
    if package.suffix == ".apk":
        with tempfile.TemporaryDirectory(prefix="mtc-apk-") as directory:
            subprocess.run([str(sdk / "staging_dir/host/bin/apk"), "extract", "--allow-untrusted", "--no-chown",
                            "--destination", directory, str(package)], check=True, stdout=subprocess.DEVNULL)
            return (Path(directory) / "usr/bin/magitrickled").read_bytes()
    if package.read_bytes().startswith(b"!<arch>\n"):
        data = subprocess.check_output(["ar", "p", str(package), "data.tar.gz"])
    else:
        with tarfile.open(package) as archive:
            name = next(n for n in archive.getnames() if n.lstrip("./") == "data.tar.gz")
            data = archive.extractfile(name).read()
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        name = next(n for n in archive.getnames() if n.lstrip("./") == "usr/bin/magitrickled")
        return archive.extractfile(name).read()


def run(command, cwd, log, env):
    print("+ " + " ".join(map(str, command)), flush=True)
    log.write(("\n+ " + " ".join(map(str, command)) + "\n").encode())
    log.flush()
    subprocess.run(command, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)


def prepare(sdk, log, env):
    lines = []
    for line in (sdk / "feeds.conf.default").read_text().splitlines():
        parts = line.split()
        name_index = 1
        while name_index < len(parts) and parts[name_index].startswith("--"):
            name_index += 1
        if len(parts) == name_index + 2 and parts[0] in ("src-git", "src-git-full") and parts[name_index] in ("base", "packages", "luci"):
            if "^" not in parts[name_index + 1]:
                parts[0] = "src-git"
            lines.append(" ".join(parts))
    if len(lines) != 3:
        raise ValueError("SDK feed layout differs from CI; inspect feeds.conf.default")
    (sdk / "feeds.conf").write_text("\n".join(lines) + "\n")
    run(["./scripts/feeds", "update", "-a"], sdk, log, env)
    # Only disposable, freshly downloaded SDK feed checkouts are touched.
    # A feed update can leave files from its initial branch after pin checkout.
    for name in ("base", "packages", "luci"):
        feed = sdk / "feeds" / name
        top = Path(subprocess.check_output(["git", "rev-parse", "--show-toplevel"], cwd=feed, text=True).strip()).resolve()
        allowed = {sdk / "feeds" / name, sdk / "feeds" / (name + "_root")}
        if top not in allowed or not (top / ".git").is_dir():
            raise ValueError(f"refusing to clean a non-root feed checkout: {feed}")
        run(["git", "clean", "-fd"], top, log, env)
    run(["./scripts/feeds", "install", "libyaml", "pcre2", "libmnl", "libcurl", "libubus", "libubox", "rpcd-mod-luci",
         "iptables-nft", "iptables-mod-conntrack-extra", "kmod-ipt-nat", "kmod-ipt-ipset", "ip6tables-nft", "libatomic"], sdk, log, env)
    run(["make", "defconfig"], sdk, log, env)
    run(["make", "-j2", "package/libubox/compile", "package/ubus/compile", "package/yaml/compile", "package/pcre2/compile",
         "package/libmnl/compile", "package/curl/compile", "package/libcjson/compile", "V=s"], sdk, log, env)



def prepare_sources(sdk, archive, version, expected_hashes, log, env):
    """Clean, replace the fixed-name cache, then verify SDK-unpacked inputs."""
    run(["make", "package/mt-c/clean", "NO_DEPS=1", "V=s"], sdk, log, env)
    # PKG_HASH=skip and a fixed source filename require explicit cache replacement.
    # Do this after clean; removing a stale entry earlier proved insufficient.
    cached_archive = sdk / "dl/mt-c-source.tar.gz"
    cached_archive.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(archive, cached_archive)
    if sha256(cached_archive) != sha256(archive):
        raise ValueError("SDK download archive differs from fresh source archive")
    run(["make", "package/mt-c/prepare", "NO_DEPS=1", "V=s"], sdk, log, env)
    unpacked = list((sdk / "build_dir").glob(f"target-*/mt-c-{version}"))
    if len(unpacked) != 1:
        raise ValueError("SDK unpacked sources do not match input sources")
    build_root = (sdk / "build_dir").resolve()
    if not build_root.is_relative_to(sdk.resolve()) or not unpacked[0].resolve().is_relative_to(build_root):
        raise ValueError("unpacked source root escapes the disposable SDK build tree")
    if source_hashes(unpacked[0]) != expected_hashes:
        raise ValueError("SDK unpacked sources do not match input sources")
    # Source hashes alone cannot validate cached object files from an earlier variant.
    build_dir = unpacked[0] / "src/backend-c/build"
    if not build_dir.parent.resolve().is_relative_to(unpacked[0].resolve()):
        raise ValueError("backend build directory escapes the disposable SDK tree")
    if build_dir.is_symlink():
        build_dir.unlink()  # Never traverse an unexpected output symlink.
    elif build_dir.exists():
        shutil.rmtree(build_dir)
    return unpacked[0], cached_archive


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--sdk", type=Path, required=True)
    p.add_argument("--source", type=Path, required=True)
    p.add_argument("--frontend", type=Path)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--manifest", type=Path)
    p.add_argument("--version", default="0.8.5.3")
    p.add_argument("--revision", default="1")
    p.add_argument("--prepare-only", action="store_true")
    a = p.parse_args()
    sdk, source, output = a.sdk.resolve(), a.source.resolve(), a.output.resolve()
    if not (sdk / "Makefile").is_file() or not (sdk / "staging_dir/target-aarch64_cortex-a53_musl").is_dir():
        p.error("expected a verified aarch64_cortex-a53 musl SDK")
    if not re.fullmatch(r"[0-9][A-Za-z0-9.+_-]*", a.version) or not re.fullmatch(r"[0-9]+", a.revision):
        p.error("invalid package version/revision")
    if not a.prepare_only and (a.frontend is None or not (a.frontend / "index.html").is_file()):
        p.error("--frontend must contain the same built index.html for both variants")
    output.mkdir(parents=True, exist_ok=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith("PKG_") and k not in
           ("PLATFORM", "TARGET", "CROSS_COMPILE", "SYSROOT", "STAGING_DIR", "MAKEFLAGS", "MFLAGS", "MAKEOVERRIDES")}
    env["FORCE_UNSAFE_CONFIGURE"] = "1"
    package = sdk / "package/net/mt-c"
    package.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(REPO / "tools/ci/openwrt-package/Makefile", package / "Makefile")
    (package / "version.mk").write_text(f"MAGITRICKLE_VERSION:={a.version}\nMAGITRICKLE_RELEASE:={a.revision}\n")
    cjson = sdk / "package/libs/libcjson"
    cjson.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(REPO / "tools/ci/openwrt-package-libcjson/Makefile", cjson / "Makefile")
    with (output / "build.log").open("wb") as log:
        if a.prepare_only:
            prepare(sdk, log, env)
            return
        hashes = source_hashes(source)
        if a.manifest:
            expected = json.loads(a.manifest.read_text())["production_sha256"]
            if any(hashes.get(name) != digest for name, digest in expected.items()):
                raise ValueError("source differs from the requested production manifest")
        archive = package / "mt-c-source.tar.gz"
        prefix = f"mt-c-{a.version}"

        def include(info):
            return None if "build" in Path(info.name).parts else info

        with tarfile.open(archive, "w:gz") as tar:
            for name in ("LICENSE", "files", "src/backend-c"):
                tar.add(source / name, arcname=f"{prefix}/{name}", filter=include)
            tar.add(a.frontend.resolve(), arcname=f"{prefix}/src/frontend/dist")
        run(["make", "defconfig"], sdk, log, env)
        unpacked, cached_archive = prepare_sources(sdk, archive, a.version, hashes, log, env)
        run(["make", "-j2", "package/mt-c/compile", "NO_DEPS=1", "V=s"], sdk, log, env)
    artifacts = list((sdk / "bin").rglob("mt-c*.apk")) + list((sdk / "bin").rglob("mt-c*.ipk"))
    if len(artifacts) != 1:
        raise ValueError(f"expected one built package, found {artifacts}")
    binary = unpacked / ".pkgdir/mt-c/usr/bin/magitrickled"
    shutil.copy2(artifacts[0], output / artifacts[0].name)
    shutil.copy2(binary, output / "magitrickled")
    payload = packaged_binary(sdk, artifacts[0])
    (output / "packaged-magitrickled").write_bytes(payload)
    elf = subprocess.check_output(["readelf", "-h", "-l", "-d", str(binary)], text=True)
    if "AArch64" not in elf or "/lib/ld-musl-aarch64.so.1" not in elf:
        raise ValueError("unexpected daemon architecture/libc")
    (output / "readelf.txt").write_text(elf)
    metadata = dict(source_commit=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip(),
                    source_sha256=hashes, sdk_unpacked_verified=True, backend_build_cleared=True, manifest_verified=bool(a.manifest),
                    package=artifacts[0].name, package_sha256=sha256(artifacts[0]), binary_sha256=sha256(binary),
                    package_binary_sha256=hashlib.sha256(payload).hexdigest(),
                    sdk=str(sdk), version=a.version, revision=a.revision,
                    recipe_sha256=sha256(package / "Makefile"), source_archive_sha256=sha256(archive),
                    download_archive_sha256=sha256(cached_archive),
                    feeds_conf=(sdk / "feeds.conf").read_text(),
                    feed_commits={n:subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=sdk/"feeds"/n, text=True).strip() for n in ("base","packages","luci")})
    (output / "build-metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(f"PASS: {artifacts[0].name}, AArch64 musl, unpacked sources verified")


if __name__ == "__main__":
    main()
