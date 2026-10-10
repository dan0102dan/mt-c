# GitHub updates from WebUI

## Flow

The browser loads `dan0102dan/mt-c/releases?per_page=100` once per cache period.
One shared one-hour cache (with ETag/backoff) serves both channels. Stable picks
the latest published non-draft, non-prerelease entry; dev also includes
prereleases. Toggling the channel uses the list locally. Opening the version
popup checks an expired cache; failed checks have a manual Retry action. Router
credentials are never sent to GitHub. Actions-only artifacts are not a channel.

The router ships **one `magitrickled` executable**. On an explicit Update click:

1. The API validates root authorization and the requested newer release, takes
   the runtime lock and saves the current config through the normal save path.
2. A thread in the existing daemon fetches GitHub metadata independently of the
   browser, checks the same release selection and exact build target, and streams
   the package into a private temporary directory. Metadata, package size, HTTPS
   redirects and SHA-256 remain validated. DNS/API network waits are not blocked.
3. The verified local package is handed to the system `opkg install` or
   `apk add --allow-untrusted` command. Package hooks stop/start the service as
   usual. The command is detached from the daemon so stopping the daemon does
   not cancel installation. There is no second daemon or `--update-worker` mode.
4. WebUI reconnects and checks the actual running version and package revision.
   An idle status alone never means success; the install POST is never retried
   automatically. Refreshing the page after success loads the new JS/CSS.

A fixed `/bin/sh -c` invocation only connects the package manager to the existing
system `logger` (`mt-c-install` tag) and removes the temporary package after it
finishes. All paths/options are separate fixed or validated arguments, not
browser-provided commands. This uses standard OS utilities, not a new installed
script or program. Logger retention is the device's system-log policy. There is
no mt-c-owned `install.log`, `status.json`, request journal, backup copy, automatic
recovery loop, or binary rollback. Diagnose failed installations using normal
system/package-manager tools or CLI. Old updater job files are ignored.

## API contract (D-74, amended)

All routes remain below `/api/v1/system`:

- `GET /update`: local `installed_version`, `installed_revision`, `asset_suffix`,
  `can_install`, and `reason`; HTTP 200, no GitHub access.
- `GET /update/status`: current in-memory preparation (`queued`, `checking`,
  `downloading`, `verifying`, `failed`) or `installing` while a system command
  holds the runtime lock. Preparation can include a `job_id`, `tag`,
  `target_version`, `target_revision`, and a transient `error`. After restart,
  installing may have no job identity; after completion the new daemon reports
  idle. No terminal installation record is retained.
- `POST /update/install`: exactly `{ "tag": string, "release_id": positive safe
  integer, "preview": boolean }`. HTTP 202 accepts preparation and returns the
  queued job ID and target identity. It does not confirm package installation.
  Invalid/non-newer/cross-site requests return 400; unavailable tools/root JWT
  return 403; concurrent update returns 409; config/setup/thread failure 500.
  Ordinary authentication middleware may return 401 first.

Both reads use `Cache-Control: no-store`. Installation still requires a verified
root JWT on HTTP and Unix transports, including when ordinary WebUI auth is off.
A root-owned flock at `<MT_SOCK_PATH>.update-lock` prevents overlapping commands
across daemon restarts. It is runtime coordination, not saved update history.
Only the detached handoff shell inherits it; package/logger children close the
lock so package hooks cannot pass it into the newly started daemon.

Preparation is canceled/joined when the daemon shuts down. Once handed off,
shutdown does not wait for or kill the system package manager. Temporary files
are removed on normal preparation failure or after the system command; abrupt
power loss/kill during preparation may leave a file in `/tmp` until reboot.

## Compatibility and tests

Build target, package revision and exact firmware/ABI matching are unchanged.
Both numeric release tags and installed `~git...`/APK `_pre...` snapshot versions
are understood; unknown formats and downgrades fail closed. The APK trust flag
only applies to the already verified local asset. Normal config conffiles and
package hooks remain unchanged; package upgrades remove the obsolete updater
binary from package ownership, without an ad-hoc deletion of user files.

The Deno mock simulates preparation and a restart to idle, updating its in-memory
installed version without downloading/installing anything. Tests cover release
selection/digests, detached command survival, lock inheritance, standard logging,
temporary cleanup, single-binary IPK/APK inventories, browser retries and
confirmation against a restarted daemon with no job history. Normal backend
unit/sanitizer and frontend unit/E2E suites apply. Real Entware/Keenetic/OpenWrt
upgrade and failure testing is still required before production rollout.
