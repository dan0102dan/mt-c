# Router Emulator host runtime compatibility

The latest stable release is still resolved by `router_emulator_release.py`.
Both release assets must pass their published SHA256 and size checks. There
is no version downgrade and no substitution of a distribution's unpatched QEMU.

## Why a version check alone was insufficient

On 2026-11-01 the v4.1 x86-64 package required GLIBC_2.42/GLIBC_2.43, while
our Ubuntu 24.04 runners provided an older glibc. The archive and preset
checks passed and NAND creation succeeded, but QEMU exited before OpenWrt
booted. Both #31 and its stacked #32 inherited this CI problem. The failed
run's `work/ci-emulator-launch.log` contains the dynamic-loader errors.

The upstream Linux build guide documents this constraint: the x86-64
package bundles its non-libc dependencies, but not glibc. Updating the NAND
capacity argument did not fix this separate runtime compatibility problem.

## Preparation path

The existing `router_emulator_preset.py` CLI now checks the **actual QEMU
binary** with `--version` and `-machine help` before preparing flash. Merely
running `emulator.sh -V` does not load QEMU and cannot validate compatibility.

When the packaged binary runs and exposes `mt7981-router`, it is used as-is.
Otherwise `router_emulator_runtime.py` checks out the **same selected release
tag**, verifies that the checkout's commit and VERSION agree, and executes
upstream `build.sh` against the host libraries, with headless configure options.
The rebuilt binary must pass the same executable/machine checks before it is
published at the source-build path already supported by upstream `emulator.sh`.
It does not inherit the release's incompatible bundled libraries.

Build-tool installation is enabled by default only inside GitHub Actions;
local invocation needs `--install-build-deps` to authorize apt installation.
`--no-install-build-deps` explicitly disables that behavior. No host glibc
replacement, global LD_LIBRARY_PATH override, runtime dependency in mt-c,
new daemon configuration field, skipped routing test or continue-on-error is
introduced. A failed build or runtime probe fails the job.

`work/ci-emulator-runtime-build.log` and `work/ci-emulator-runtime.log` are
included by the existing smoke-log artifact glob. They record the failing
prebuilt probe, build output, source commit, selected release, actual binary
SHA256 and readiness status. Reuse requires matching provenance and a new
executable probe. Guest boot/package/DNS/LAN checks still run afterwards and
remain mandatory: passing a host preflight is not a guest-test pass.

## Scope of validation

`test_router_emulator_runtime.py` uses real executable subprocess fixtures
and controlled source checkout fixtures without network access. It covers
loader failure, same-release rebuild, failure cleanup, provenance, reuse,
unsupported machine, and the actual preset CLI integration. It is **not**
a QEMU boot test; the OpenWrt 24.10.4/IPK and 25.12.5/APK smoke jobs provide
that integration coverage.

A host rebuild is not the release's PGO binary. Historical timing results
remain tied to their recorded QEMU hashes and must not be attributed to a
new build; benchmark both application variants on the same recorded runtime.
