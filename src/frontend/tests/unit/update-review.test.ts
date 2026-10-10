import assert from "node:assert/strict";

import {
  candidate,
  compareVersions,
  parseVersion,
  pickRelease,
  ReleaseClient,
} from "../../src/modules/updates/releases.ts";

function release(tag: string, date: string, prerelease = false) {
  return {
    id: 100,
    tag_name: tag,
    draft: false,
    prerelease,
    published_at: date,
    assets: [],
  };
}

Deno.test("installed APK SDK snapshot identities sort before the matching release", () => {
  const snapshot = "0.8.4_pre20261010140000";
  assert.equal(parseVersion(snapshot)?.development, true);
  assert.equal(compareVersions("0.8.4", snapshot), 1);
  assert.equal(compareVersions("0.8.3", snapshot), -1);
  assert.equal(compareVersions(snapshot, "0.8.4~git20261010140000.abcdef0"), 0);
  for (const invalid of [
    "0.8.4_pre2026101014000",
    "0.8.4_pre202610101400000",
    "0.8.4_pre20261010140000.abcdef0",
    "0.8.4_pre20261010140000-rev1",
  ]) {
    assert.equal(parseVersion(invalid), null);
  }
  const info = {
    installed_version: snapshot,
    installed_revision: 1,
    asset_suffix: "openwrt-25.12.5_aarch64_cortex-a53.apk",
    can_install: true,
    reason: "",
  };
  const selected = pickRelease([release("0.8.4", "2026-10-10T00:00:00Z")], false);
  assert.equal(candidate(selected, info).newer, true);
  const dev = pickRelease([release(snapshot, "2026-10-10T00:00:00Z", true)], true);
  assert.equal(candidate(dev, { ...info, installed_version: "0.8.3" }).asset, null);
  assert.equal(candidate(dev, info).reason, "Unknown installed or release version");
});

Deno.test("both channels select publication time, not version maximum", () => {
  const releases = [
    release("2.0.0", "2026-10-01T00:00:00Z"),
    release("1.1.0", "2026-10-03T00:00:00Z"),
    release("3.0.0", "2026-10-04T00:00:00Z", true),
  ];
  assert.equal(pickRelease(releases, false)?.tag_name, "1.1.0");
  assert.equal(pickRelease(releases, true)?.tag_name, "3.0.0");
});

Deno.test("one shared 100-item window includes candidates beyond the old worker page", async () => {
  const releases = Array.from({ length: 30 }, () =>
    release("0.8.2.2", "2026-10-01T00:00:00Z", true),
  );
  releases.push(release("0.8.3", "2026-10-09T00:00:00Z"));
  let requests = 0;
  const client = new ReleaseClient(null, async (url) => {
    assert.equal(String(url), "https://api.github.com/repos/dan0102dan/mt-c/releases?per_page=100");
    requests++;
    return new Response(JSON.stringify(releases));
  });
  assert.equal((await client.latest(false, false)).release?.tag_name, "0.8.3");
  assert.equal((await client.latest(true, false)).release?.tag_name, "0.8.3");
  for (let i = 0; i < 40; i++) assert.ok(client.cached(i % 2 === 0));
  assert.equal(requests, 1);
});
