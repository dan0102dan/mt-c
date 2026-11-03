# OpenWrt guest integration tests

The package build workflow boots OpenWrt in Router Emulator v3.7 and tests the
exact cross-built package from that matrix job. These checks run on eligible
`push` builds only; release and `workflow_dispatch` builds keep their existing
behavior. The tests exercise the backend, not a browser or API mocks.

| OpenWrt | Architecture | Package manager | Emulator preset |
| --- | --- | --- | --- |
| 24.10.4 | aarch64_cortex-a53 | `opkg`, IPK | `cudy-tr3000-v1` |
| 25.12.5 | aarch64_cortex-a53 | `apk`, APK | `cudy-wr3000p-v1` |

The older release uses TR3000 because the WR3000P OpenWrt U-Boot images needed
by `prepare-nand.sh` are not available in 24.10.4. This selection does not change
the supported build targets, which remain defined by `config/openwrt/*.config`.

## Checks inside each guest

`tools/ci/router-emulator-smoke.py` verifies the running release, downloads the
packages into the guest, checks their SHA-256 and installs them with the guest's
package manager. Feed failures abort the test; dependency checks are not bypassed.
The package's own hooks must start the service, and it must remain alive while
the HTTP API responds. The test does not manually start or restart the service.

Before installing the exact CI artifact, the workflow installs a lower-version
fixture generated from it by `tools/ci/build-upgrade-fixture.py`. The fixture
preserves its payload, dependencies and lifecycle hooks, apart from a test package
version and a harmless trailer appended to the daemon's ELF file. This creates
different executable bytes without compiling a second backend. The fixture is
used only in the disposable guest and is not published as a release artifact.

The original CI artifact is then installed over the fixture without removing the
package first. The test requires the old process to exit, a new PID to appear,
and the running `/proc/<pid>/exe` checksum to match the newly installed binary
and differ from the fixture. It also checks preservation of a modified UCI
conffile and waits for the API to return before testing DHCP and routing.
Checking only the version on disk or an HTTP response could miss a daemon still
running the old executable.

On APK, `post-upgrade` invokes OpenWrt's `default_postinst`, which calls `start`.
APK does not run the removal hook before an upgrade. The mt-c init script
registers its executable with procd's `file` parameter so changed binary contents
cause this `start` to replace an existing instance, even though the command path
has not changed. This applies to both the SDK and root Makefile packaging paths.

`router-emulator-dhcp.sh` creates a separate LAN client, acquires a real dnsmasq
lease and checks device names through ubus, DNS journal JSON and SSE. Renewing
with another hostname on the same IP must update existing journal rows and the
connected viewer without restarting mt-c. Reconnecting must rediscover the name.

`router-emulator-routing.sh` creates separate LAN-client and destination-server
network namespaces. All six configured rule types run independently:

- `domain`: exact domain, with a child domain as the nonmatching control;
- `namespace`: child domain, with a similar but unrelated suffix as the control;
- `wildcard`: matching and nonmatching domain names;
- `regex`: matching and nonmatching domain names;
- `subnet`: IPv4 CIDR populated without a DNS request;
- `subnet6`: IPv6 CIDR populated without a DNS request.

For each type, a main-table blackhole first blocks the destination. The test
creates a group through the real API, checks its exact ipset, packet-marking
chain, policy rule and selected routing table, then makes a real client HTTP
request through the selected interface. For DNS rules, the nonmatching domain
resolves to the same destination address and must leave the set empty and the
destination blocked. Deleting the group must remove its set and block traffic
again, so another rule cannot conceal a failure.

The fixture temporarily permits LAN forwarding into its test egress zone and
IPv6 neighbor discovery from that zone. It
does not inject mt-c ipset members, packet marks or policy routes. The IPv6
server subnet has an ordinary main-table route for neighbor discovery and
source validation; a more specific blackhole for the LAN source prefix still
blocks client traffic unless mt-c's marked routing table takes precedence.

## Running locally

Prepare fresh flash using the matching release and profile in the table above,
then run from the project root, for example:

```sh
python3 tools/ci/router-emulator-smoke.py \
  --emulator-dir /path/to/Router-Emulator \
  --package /path/to/mt-c_openwrt-24.10.4_aarch64_cortex-a53.ipk \
  --preset cudy-tr3000-v1 \
  --openwrt-version 24.10.4
```

For 25.12.5, supply its APK, `cudy-wr3000p-v1` and `--openwrt-version 25.12.5`.
To exercise upgrades, also pass `--baseline-package /path/to/baseline.ipk` (or
`.apk`), created with `build-upgrade-fixture.py --package <candidate> --output
<baseline>`. For APK, supply `--apk-tool <sdk>/staging_dir/host/bin/apk` and run
the generator through the SDK's `fakeroot` to preserve file ownership, with
`STAGING_DIR_HOST=<sdk>/staging_dir/host` exported so the relocated SDK can find
its fakeroot library and helper. Omitting the baseline tests a fresh installation
only.
Run guests sequentially: the runner reserves host port 18881 for the API.
The guest needs access to its release's package feeds. Host preparation needs
the emulator tools and `mtd-utils`; the smoke runner uses Python's standard
library. Only run the guest shell fixtures in disposable test routers.

Serial and emulator logs are saved under the emulator's `work/` directory and
uploaded by CI even on failure. Guest fixture output is copied into the serial
log before its exit status is checked.

Host orchestration regressions can be checked separately:

```sh
python3 tools/ci/tests/test_router_emulator.py
```

These host tests cover package-manager selection, package delivery, upgrade
ordering, stale-process failures and cleanup; they do not establish that installation or routing works
on OpenWrt. The emulator does not establish behavior on other architectures,
physical switches, Wi-Fi hardware or hardware flow offload. Reboot persistence
and profile failover are separate scenarios. The upgrade fixture exercises a
version change and executable replacement, not a historical configuration
migration or a backend feature change.
