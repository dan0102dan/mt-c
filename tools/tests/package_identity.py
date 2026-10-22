#!/usr/bin/env python3
"""Offline packaging-contract smoke test, not a cross-build or router install.

Build real IPK archives from an explicitly synthetic payload. For APK, capture
mkpkg's arguments and inspect the staged inventories/hooks (no fake APK is
claimed to be installable). SDK recipes are evaluated with minimal make stubs.
"""
from __future__ import annotations

import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import socket
import threading
import time

REPO = Path(__file__).resolve().parents[2]
NDM_HOOKS = ("iflayerchanged.d", "ifstatechanged.d", "ifcreated.d", "ifdestroyed.d", "neighbour.d")
NDM_FILES = {f"etc/ndm/{hook}/100-magitrickle" for hook in NDM_HOOKS} | {
    "lib/magitrickle/ndm-interface-hook",
}


def run(args: list[str], cwd: Path, env: dict[str, str] | None = None) -> str:
    result = subprocess.run(args, cwd=cwd, env=env, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"{args!r}\n{result.stdout}\n{result.stderr}")
    return result.stdout


def ipk_contract(work: Path, platform: str, target: str) -> None:
    key = f"{platform}_{target}"
    compile_dir = work / ".build" / key / "compile"
    compile_dir.mkdir(parents=True, exist_ok=True)
    frontend = work / ".build" / key / "frontend"
    frontend.mkdir(parents=True, exist_ok=True)
    marker = f"<title>synthetic {key}</title>\n"
    (frontend / "index.html").write_text(marker)
    for name in ("magitrickled",):
        executable = compile_dir / name
        executable.write_text("#!/bin/sh\n# Synthetic payload: never install this test package.\nexit 0\n")
        executable.chmod(0o755)
    variables = [f"PLATFORM={platform}", f"TARGET={target}", "PKG_VERSION=1.2.3", "PKG_REVISION=1"]
    run(["make", "-o", "build", "package_ipk", *variables], work)
    package = work / ".build" / f"mt-c_1.2.3-1_{key}.ipk"
    with tarfile.open(package) as outer:
        def inner(name: str) -> tarfile.TarFile:
            member = outer.extractfile(f"./{name}.tar.gz")
            assert member
            return tarfile.open(fileobj=io.BytesIO(member.read()), mode="r:gz")
        with inner("control") as control:
            member = control.extractfile("./control")
            assert member
            fields = dict(line.split(": ", 1) for line in member.read().decode().splitlines())
            assert fields["Package"] == "mt-c"
            assert fields["Conflicts"] == "magitrickle"
            assert fields["Version"] == "1.2.3-1"
            assert "Replaces" not in fields and "Provides" not in fields
        with inner("data") as payload:
            files = set(payload.getnames())
            binary_dir = "./opt/bin" if platform == "entware" else "./usr/bin"
            config = "./opt/var/lib/magitrickle/config.yaml" if platform == "entware" else "./etc/magitrickle/state/config.yaml"
            assert config in files
            assets = "./opt/usr/share" if platform == "entware" else "./usr/share"
            html = payload.extractfile(f"{assets}/magitrickle/skins/default/index.html")
            assert html and html.read().decode() == marker
            assert not any(name.endswith(".mt-c-build-properties") for name in files)
            assert f"{binary_dir}/mt-c-updater" not in files
            is_kn = platform == "entware" and target.endswith("_kn")
            for relative in NDM_FILES:
                installed = f"./opt/{relative}"
                assert (installed in files) == is_kn, (key, installed)
                if is_kn:
                    assert payload.getmember(installed).mode & 0o111
            if not is_kn:
                assert not any("/ndm/" in name or name.endswith("ndm-interface-hook") for name in files)
            for name in ("magitrickled",):
                binary = f"{binary_dir}/{name}"
                assert binary in files
                assert payload.getmember(binary).mode & 0o111
    print(f"PASS real IPK metadata and preserved payload paths: {key}")


def apk_contract(work: Path) -> None:
    bindir = work / "test-bin"
    bindir.mkdir()
    recorder = bindir / "apk"
    recorder.write_text("#!/usr/bin/env python3\nimport json, os, sys\nfrom pathlib import Path\nPath(os.environ['APK_ARGS']).write_text(json.dumps(sys.argv[1:]))\n")
    recorder.chmod(0o755)
    key = work / "test-only-key"
    key.write_text("not a key; mkpkg is an argument recorder in this test\n")
    args_path = work / "apk-args.json"
    env = dict(os.environ, PATH=str(bindir) + os.pathsep + os.environ["PATH"], APK_ARGS=str(args_path))
    run(["make", "-o", "build", "package_apk", "ROOT_WRAP=", "PLATFORM=openwrt", "TARGET=x86_64",
         "PKG_VERSION=1.2.3", "PKG_REVISION=1", f"BUILD_KEY_APK_SEC={key}"], work, env)
    args = json.loads(args_path.read_text())
    assert args[0] == "mkpkg"
    info = [args[i + 1] for i, value in enumerate(args[:-1]) if value == "-I"]
    assert "name:mt-c" in info
    depends = next(item for item in info if item.startswith("depends:"))
    assert "!magitrickle" in depends.split()
    assert not any(item.startswith(("provides:", "replaces:")) for item in info)
    root = work / ".build/openwrt_x86_64/root_apk"
    assert (root / "lib/apk/packages/mt-c.list").is_file()
    assert (root / "lib/apk/packages/mt-c.conffiles").is_file()
    inventory = (root / "lib/apk/packages/mt-c.list").read_text().splitlines()
    for name in ("magitrickled",):
        assert f"/usr/bin/{name}" in inventory
        assert (root / "usr/bin" / name).stat().st_mode & 0o111
    assert not any("/ndm/" in name or name.endswith("ndm-interface-hook") for name in inventory)
    assert "/usr/bin/mt-c-updater" not in inventory
    assert not (root / "usr/bin/mt-c-updater").exists()
    assert not list((root / "lib/apk/packages").glob("magitrickle.*"))
    for script in (work / ".build/openwrt_x86_64/apk").glob("*.sh"):
        assert 'pkgname="mt-c"' in script.read_text()
        run(["sh", "-n", str(script)], work)
    print("PASS APK mkpkg conflict argument, package inventories and lifecycle hooks (staging only)")


def sdk_contract(work: Path, platform: str) -> None:
    sdk = work / f"sdk-{platform}"
    sdk.mkdir()
    (sdk / "rules.mk").write_text("INCLUDE_DIR:=$(CURDIR)\nBUILD_DIR:=$(CURDIR)/build\n")
    (sdk / "version.mk").write_text("MAGITRICKLE_VERSION:=1.2.3\nMAGITRICKLE_RELEASE:=1\nMAGITRICKLE_TARGET:=aarch64-3.10_kn\n")
    # Evaluate the actual package definition, rather than a grep of its text.
    (sdk / "package.mk").write_text("define BuildPackage\n$$(eval $$(Package/$(1)))\n.PHONY: check\ncheck:\n\t@echo 'name=$(1)'\n\t@echo 'conflicts=$$(CONFLICTS)'\n\t@echo 'source=$$(PKG_SOURCE)'\nendef\n")
    shutil.copy(REPO / f"tools/ci/{platform}-package/Makefile", sdk / "Makefile")
    output = run(["make", "-s", "check", f"TOPDIR={sdk}"], sdk)
    assert "name=mt-c\n" in output and "conflicts=magitrickle\n" in output
    assert "source=mt-c-source.tar.gz\n" in output
    print(f"PASS evaluated {platform} SDK package identity/conflicts/source name")


def sdk_ndm_install_contract(work: Path) -> None:
    # Exercise the actual SDK install definition with ordinary install/cp, not
    # only the root Makefile. CI release builds use this separate recipe.
    for target in ("aarch64-3.10_kn", "aarch64-3.10"):
        sdk = work / f"sdk-payload-{target}"
        sdk.mkdir()
        (sdk / "rules.mk").write_text(
            "INCLUDE_DIR:=$(CURDIR)\nBUILD_DIR:=$(CURDIR)/build\n"
            "INSTALL_DIR:=mkdir -p\nINSTALL_BIN:=install -m 0755\n"
            "INSTALL_CONF:=install -m 0644\nCP:=cp -r\n"
        )
        (sdk / "package.mk").write_text("")
        (sdk / "version.mk").write_text(
            f"MAGITRICKLE_VERSION:=1.2.3\nMAGITRICKLE_RELEASE:=1\nMAGITRICKLE_TARGET:={target}\n"
        )
        source = sdk / "build/mt-c-1.2.3"
        shutil.copytree(REPO / "files", source / "files")
        binary = source / f"src/backend-c/build/entware-{target}/magitrickled-c"
        binary.parent.mkdir(parents=True)
        binary.write_text("synthetic noninstallable payload\n")
        frontend = source / "src/frontend/dist"
        frontend.mkdir(parents=True)
        (frontend / "index.html").write_text("synthetic fixture\n")
        recipe = (REPO / "tools/ci/entware-package/Makefile").read_text()
        # The SDK evaluates the install body, including its conditionals.
        recipe += "\ndefine newline\n\n\nendef\n"
        recipe += "$(eval fixture-install:$(newline)$(call Package/mt-c/install,$(CURDIR)/root))\n"
        (sdk / "Makefile").write_text(recipe)
        run(["make", "-s", "fixture-install", f"TOPDIR={sdk}"], sdk)
        root = sdk / "root/opt"
        for relative in NDM_FILES:
            installed = root / relative
            assert installed.exists() == target.endswith("_kn"), (target, relative)
            if installed.exists():
                assert installed.read_bytes() == (REPO / "files/entware_kn" / relative).read_bytes()
                assert installed.stat().st_mode & 0o111
        print(f"PASS evaluated SDK NDM install payload: {target}")


def ndm_hook_contract(work: Path) -> None:
    helper = REPO / "files/entware_kn/lib/magitrickle/ndm-interface-hook"
    text = helper.read_text()
    run(["sh", "-n", str(helper)], REPO)
    for hook, kind in zip(NDM_HOOKS, ("layer", "state", "created", "destroyed", "neighbour")):
        wrapper = REPO / f"files/entware_kn/etc/ndm/{hook}/100-magitrickle"
        run(["sh", "-n", str(wrapper)], REPO)
        assert f'ndm-interface-hook {kind} "$@"' in wrapper.read_text()
        assert wrapper.stat().st_mode & 0o111
    root = work / "hook-test"
    root.mkdir()
    socket_path, lost = root / "socket", root / "lost"
    recorder = root / "socat"
    recorder.write_text(
        "#!/usr/bin/env python3\nimport json, os, sys\nfrom pathlib import Path\n"
        "Path(os.environ['HOOK_REQUEST']).write_bytes(sys.stdin.buffer.read())\n"
        "Path(os.environ['HOOK_ARGS']).write_text(json.dumps(sys.argv[1:]))\n"
        "print(os.environ.get('HOOK_REPLY', 'HTTP/1.1 202 Accepted'))\n"
    )
    recorder.chmod(0o755)
    copied = root / "hook"
    # Change fixed paths only in the test copy. Production exposes no arbitrary
    # URL, command, socket or output-file setting through environment variables.
    text = text.replace("PATH=/opt/sbin:", f"PATH={root}:/opt/sbin:")
    text = text.replace("socket=/opt/var/run/magitrickle.sock", f"socket={socket_path}")
    text = text.replace("lost=/opt/var/run/magitrickle-ndm-lost", f"lost={lost}")
    copied.write_text(text)
    request, arguments = root / "request", root / "args"
    env = dict(os.environ, id="Wireguard0", system_name="nwg0", layer="ctrl", level="disabled",
               up="up", link="up", connected="no", HOOK_REQUEST=str(request), HOOK_ARGS=str(arguments))
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
        listener.bind(str(socket_path))
        for kind in ("layer", "state", "created", "destroyed"):
            run(["sh", str(copied), kind, "hook"], work, env)
            head, body = request.read_bytes().split(b"\r\n\r\n", 1)
            assert f"Content-Length: {len(body)}".encode() in head
            data = json.loads(body)
            assert data["kind"] == kind and data["id"] == "Wireguard0" and data["system_name"] == "nwg0"
            assert data["resync"] is False
            argv = json.loads(arguments.read_text())
            assert argv[:3] == ["-T", "1", "-"] and "connect-timeout=1" in argv[3]
            assert not lost.exists()
        request.unlink()
        # neighbour.d has different environment fields from interface hooks.
        # It sends a fixed invalidation, including with absent/hostile tokens.
        neighbour_env = dict(env, system_name="", id='unused"id', mac='unused;mac')
        run(["sh", str(copied), "neighbour", "hook"], work, neighbour_env)
        head, body = request.read_bytes().split(b"\r\n\r\n", 1)
        assert head.startswith(b"POST /api/v1/system/hooks/ndm-neighbour HTTP/1.1\r\n")
        assert b"Content-Length: 0" in head and body == b""
        assert not lost.exists()
        run(["sh", str(copied), "neighbour", "hook"], work,
            dict(neighbour_env, HOOK_REPLY="HTTP/1.1 503 Unavailable"))
        assert not lost.exists()  # metadata never creates a routing-loss marker
        lost.mkdir()
        run(["sh", str(copied), "neighbour", "hook"], work, neighbour_env)
        assert lost.exists()  # it must not consume an existing interface marker
        lost.rmdir()
        request.unlink()
        run(["sh", str(copied)], work, env)
        run(["sh", str(copied), "layer", "other"], work, env)
        run(["sh", str(copied), "neighbour", "other"], work, neighbour_env)
        assert not request.exists()
        for values in ({"id": 'bad"id'}, {"system_name": "x;touch BAD"}, {"level": "x\ncmd"}, {"id": "x" * 64}):
            run(["sh", str(copied), "layer", "hook"], work, dict(env, **values))
            assert not request.exists() and lost.is_dir()
            lost.rmdir()
        run(["sh", str(copied), "layer", "hook"], work, dict(env, HOOK_REPLY="HTTP/1.1 503 Unavailable"))
        assert lost.is_dir()
        run(["sh", str(copied), "layer", "hook"], work, env)
        assert json.loads(request.read_bytes().split(b"\r\n\r\n", 1)[1])["resync"] is True
        assert lost.exists()  # only the daemon may consume the loss marker
        lost.rmdir()
        # Optional real socat check: a silent receiver must not hold the hook
        # queue open. No root/network namespace changes or router required.
        real_socat = shutil.which("socat")
        if real_socat:
            recorder.unlink()
            copied.write_text(text.replace("socat -T 1", f"{real_socat} -T 1"))
            listener.listen(1)
            def silent_peer() -> None:
                connection, _ = listener.accept()
                with connection:
                    connection.settimeout(3)
                    try:
                        while connection.recv(4096):
                            pass
                    except TimeoutError:
                        pass
            worker = threading.Thread(target=silent_peer)
            worker.start()
            started = time.monotonic()
            run(["sh", str(copied), "layer", "hook"], work, env)
            elapsed = time.monotonic() - started
            worker.join(4)
            assert not worker.is_alive() and elapsed < 2.5 and lost.is_dir()
    print("PASS NDM hooks: protocol, argument validation, delivery loss, bounded socat options")


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="mt-package-test-") as directory:
        work = Path(directory)
        shutil.copy(REPO / "Makefile", work / "Makefile")
        shutil.copytree(REPO / "files", work / "files", symlinks=True)
        dist = work / "src/frontend/dist"
        dist.mkdir(parents=True)
        (dist / "index.html").write_text("<title>synthetic packaging fixture</title>\n")
        ipk_contract(work, "entware", "aarch64-3.10_kn")
        ipk_contract(work, "entware", "aarch64-3.10")
        ipk_contract(work, "openwrt", "x86_64")
        apk_contract(work)
        sdk_contract(work, "entware")
        sdk_contract(work, "openwrt")
        sdk_ndm_install_contract(work)
        ndm_hook_contract(work)


if __name__ == "__main__":
    main()
