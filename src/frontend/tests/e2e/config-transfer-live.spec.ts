import { spawn } from "node:child_process";
import { once } from "node:events";
import fs from "node:fs/promises";
import {
  test as base,
  expect,
  type APIRequestContext,
  type Locator,
  type Page,
} from "@playwright/test";

import type { ServerConfig, ServerSnapshot } from "../../src/modules/config-transfer/adapter";
import type { PortableConfig } from "../../src/modules/config-transfer/model";

// Each test runs the real Deno mock in its own process, including its actual
// snapshot/apply and collection routes. The user's dev server is never changed.
const test = base.extend<{
  backend: {
    api: APIRequestContext;
    snapshot: () => Promise<ServerSnapshot>;
    save: (config: ServerConfig) => Promise<void>;
    writes: { path: string; method: string; body: unknown }[];
  };
}>({
  backend: async ({ playwright, page }, use) => {
    // npm ci already installed the locked dependencies. Starting each mock must
    // not resolve or download a fresh dependency graph from the registry.
    const child = spawn(
      "deno",
      ["run", "--cached-only", "-A", "--unstable-sloppy-imports", "dev/backend-mock.ts"],
      {
        env: { ...process.env, MOCK_PORT: "0" },
        stdio: ["ignore", "pipe", "pipe"],
      },
    );
    try {
      const port = await new Promise<number>((resolve, reject) => {
        let output = "";
        let stderr = "";
        const timer = setTimeout(
          () => reject(new Error(`Mock backend startup timed out\n${stderr}`)),
          10000,
        );
        child.once("error", (error) => {
          clearTimeout(timer);
          reject(error);
        });
        child.once("exit", (code, signal) => {
          clearTimeout(timer);
          reject(new Error(`Mock backend exited during startup (${code ?? signal})\n${stderr}`));
        });
        child.stdout.on("data", (chunk) => {
          output += chunk.toString();
          const match = output.match(/running mock server on port (\d+)/);
          if (match) {
            clearTimeout(timer);
            resolve(Number(match[1]));
          }
        });
        child.stderr.on("data", (chunk) => {
          stderr = (stderr + chunk.toString()).slice(-8000);
        });
      });
      const baseURL = `http://localhost:${port}`;
      const api = await playwright.request.newContext({
        baseURL,
        extraHTTPHeaders: { Authorization: "Bearer disabled" },
      });
      const snapshot = async (): Promise<ServerSnapshot> => {
        const response = await api.get("/api/v1/system/config/snapshot");
        expect(response.ok()).toBe(true);
        return response.json();
      };
      const save = async (config: ServerConfig) => {
        const { revision } = await snapshot();
        const response = await api.post("/api/v1/system/config/apply", {
          data: { revision, config },
        });
        expect(await response.json()).toEqual({ ok: true });
      };
      const initial = (await snapshot()).config;
      const ids = new Set(initial.profiles?.map((profile) => profile.id));
      expect(
        [...initial.groups, ...initial.subscriptions].every(
          (route) => !route.profile || ids.has(route.profile),
        ),
      ).toBe(true);
      await save({ ...initial, groups: [], subscriptions: [], profiles: [] });
      await page.addInitScript(() =>
        localStorage.setItem("auth", JSON.stringify({ value: "disabled" })),
      );
      const writes: { path: string; method: string; body: unknown }[] = [];
      page.on("request", (request) => {
        const path = new URL(request.url()).pathname;
        if (path.startsWith("/api/v1/") && !["GET", "HEAD", "OPTIONS"].includes(request.method()))
          writes.push({ path, method: request.method(), body: request.postDataJSON() });
      });
      await page.route("**/api/v1/**", async (route) => {
        const url = new URL(route.request().url());
        const response = await route.fetch({
          url: `${baseURL}${url.pathname}${url.search}`,
          headers: { ...route.request().headers(), authorization: "Bearer disabled" },
        });
        await route.fulfill({ response });
      });
      await page.route("https://api.github.com/**", (route) => route.fulfill({ json: [] }));
      try {
        await use({ api, snapshot, save, writes });
      } finally {
        // The updater can still have background requests when a test finishes.
        // Teardown cancellation must not turn those requests into test failures.
        await page.unrouteAll({ behavior: "ignoreErrors" });
        await page.close();
        await api.dispose();
      }
    } finally {
      if (child.exitCode === null) {
        const stopped = once(child, "exit");
        child.kill("SIGTERM");
        await stopped;
      }
    }
  },
});

test.afterEach(async ({ page }) => {
  // Include earlier navigations: F5 must not hide a dialog teardown crash.
  expect((await page.pageErrors({ filter: "all" })).map((error) => error.message)).toEqual([]);
});

const profile = (name = "Portable VPN") => ({
  id: "portable_vpn",
  name,
  interfaces: ["wg0", "nwg0"],
  on_unavailable: "blackhole",
});
const group = {
  id: "11223344",
  name: "Portable group",
  profile: "portable_vpn",
  interface: "wg0",
  enable: true,
  priority: 300,
  color: "#123456",
  rules: [
    { id: "aabbccdd", name: "Example", rule: "example.org", type: "namespace", enable: true },
  ],
};
async function showPage(page: Page, name: string) {
  const menu = page.locator(".mobile-dropdown-btn");
  await expect(menu).toBeAttached();
  if ((await menu.isVisible()) && (await menu.getAttribute("aria-expanded")) !== "true")
    await menu.click();
  await page.getByRole("tab", { name, exact: true }).click();
}
async function upload(page: Page, input: unknown, filename = "config.mtrickle") {
  const chooser = page.waitForEvent("filechooser");
  await page.getByRole("button", { name: "Import Config", exact: true }).click();
  await (
    await chooser
  ).setFiles({
    name: filename,
    mimeType: "application/json",
    buffer: Buffer.from(JSON.stringify(input)),
  });
  const dialog = page.getByRole("dialog");
  await expect(dialog).toBeVisible();
  return dialog;
}
async function download(page: Page): Promise<PortableConfig> {
  const pending = page.waitForEvent("download");
  await page.getByRole("button", { name: "Export Config", exact: true }).click();
  const file = await pending;
  expect(file.suggestedFilename()).toBe("config.mtrickle");
  return JSON.parse(await fs.readFile((await file.path())!, "utf8"));
}

function activeSave(page: Page): Locator {
  return page.locator("#save-changes:visible, #save-subscriptions:visible, #save-settings:visible");
}

async function saveImported(page: Page) {
  const button = activeSave(page);
  await expect(button).toBeEnabled();
  await button.click();
  await expect(button).toHaveAttribute("aria-busy", "false");
  await expect(button).toBeDisabled();
}

for (const width of [1280, 390]) {
  for (const source of ["Groups", "Subscriptions", "Settings"]) {
    for (const format of ["legacy", "full"] as const) {
      test(`${format} profile file imports from ${source} at ${width}px`, async ({
        page,
        backend,
      }) => {
        await page.setViewportSize({ width, height: width === 390 ? 844 : 900 });
        await page.goto("/");
        await showPage(page, source);
        const dialog = await upload(
          page,
          format === "legacy"
            ? { profiles: [profile()] }
            : {
                format: "mt-c-config",
                schemaVersion: 1,
                groups: [],
                subscriptions: [],
                profiles: [profile()],
              },
        );
        await expect(dialog.getByRole("tab", { name: "Settings", exact: true })).toHaveAttribute(
          "aria-selected",
          "true",
        );
        await expect(
          dialog.getByRole("checkbox", { name: "Portable VPN", exact: true }),
        ).toBeChecked();
        await dialog.getByRole("button", { name: "Import", exact: true }).click();
        await expect(dialog).toHaveCount(0);
        expect((await backend.snapshot()).config.profiles).toEqual([]);
        expect(backend.writes).toEqual([]);
        await saveImported(page);
        expect((await backend.snapshot()).config.profiles).toEqual([profile()]);
        await showPage(page, "Settings");
        await expect(page.getByLabel("Profile name", { exact: true })).toHaveValue("Portable VPN");
        await page.reload();
        await expect(page.getByLabel("Profile name", { exact: true })).toHaveValue("Portable VPN");
      });
    }
  }
  test(`real export/import roundtrip restores profile draft, order and rules at ${width}px`, async ({
    page,
    backend,
  }) => {
    await page.setViewportSize({ width, height: width === 390 ? 844 : 900 });
    const current = (await backend.snapshot()).config;
    await backend.save({ ...current, profiles: [profile("Saved VPN")], groups: [group] });
    await page.goto("/");
    await showPage(page, "Settings");
    await page.getByLabel("Profile name", { exact: true }).fill("Draft VPN");
    const exported = await download(page);
    expect(exported.profiles).toEqual([profile("Draft VPN")]);
    expect((await backend.snapshot()).config.profiles?.[0].name).toBe("Saved VPN");
    await backend.save({ ...current, groups: [], subscriptions: [], profiles: [] });
    await page.reload();
    const dialog = await upload(page, exported);
    await dialog.getByRole("tab", { name: "Groups", exact: true }).click();
    await dialog.getByRole("button", { name: "All", exact: true }).click();
    await dialog.getByRole("button", { name: "Import", exact: true }).click();
    await expect(dialog).toHaveCount(0);
    expect((await backend.snapshot()).config.groups).toEqual([]);
    expect(backend.writes).toEqual([]);
    await saveImported(page);
    const restored = (await backend.snapshot()).config;
    expect(restored.profiles).toEqual([profile("Draft VPN")]);
    expect(restored.groups[0].rules).toEqual(group.rules);
    expect(restored.groups[0].profile).toBe("portable_vpn");
    await expect(page.getByLabel("Profile name", { exact: true })).toHaveValue("Draft VPN");
    await expect(page.locator(".profile [data-interface]")).toHaveCount(2);
    await page.reload();
    expect((await download(page)).profiles).toEqual(exported.profiles);
  });
}

for (const choice of ["Keep current", "Use imported", "Create a copy"]) {
  test(`profile conflict: ${choice} has the expected persisted result`, async ({
    page,
    backend,
  }) => {
    const current = (await backend.snapshot()).config;
    await backend.save({ ...current, profiles: [profile("Existing VPN")] });
    await page.goto("/");
    await showPage(page, "Settings");
    const dialog = await upload(page, { profiles: [profile("Imported VPN")] });
    await dialog
      .locator('[data-select-trigger][aria-label="Resolve conflict for Imported VPN"]')
      .click();
    await page.getByRole("option", { name: choice, exact: true }).click();
    await dialog.getByRole("button", { name: "Import", exact: true }).click();
    if (choice === "Keep current") {
      await expect(page.getByText("No changes to import", { exact: true })).toBeVisible();
      await expect(dialog).toHaveCount(0);
      expect((await backend.snapshot()).config.profiles).toEqual([profile("Existing VPN")]);
      expect(backend.writes).toEqual([]);
    } else {
      await expect(dialog).toHaveCount(0);
      expect((await backend.snapshot()).config.profiles).toEqual([profile("Existing VPN")]);
      expect(backend.writes).toEqual([]);
      await saveImported(page);
      const names = (await backend.snapshot()).config.profiles?.map((item) => item.name);
      expect(names).toEqual(
        choice === "Create a copy" ? ["Existing VPN", "Imported VPN"] : ["Imported VPN"],
      );
      await expect(page.getByLabel("Profile name", { exact: true })).toHaveCount(names!.length);
      await page.reload();
      await expect(page.getByLabel("Profile name", { exact: true })).toHaveCount(names!.length);
    }
  });
}

for (const failure of ["ignored", "malformed", "rejected", "network", "stale"]) {
  test(`saving an imported profile retains its draft after ${failure} response`, async ({
    page,
    backend,
  }) => {
    await page.goto("/");
    await showPage(page, "Settings");
    const dialog = await upload(page, { profiles: [profile()] });
    await dialog.getByRole("button", { name: "Import", exact: true }).click();
    await expect(dialog).toHaveCount(0);
    await expect(page.getByLabel("Profile name", { exact: true })).toHaveValue("Portable VPN");
    expect(backend.writes).toEqual([]);
    if (failure === "stale") {
      const current = (await backend.snapshot()).config;
      await backend.save({ ...current, profiles: [profile("Concurrent edit")] });
    } else {
      await page.route("**/api/v1/system/config/apply", (route) =>
        failure === "network"
          ? route.abort("failed")
          : route.fulfill({
              status: failure === "rejected" ? 400 : 200,
              json:
                failure === "ignored"
                  ? { ok: true }
                  : failure === "malformed"
                    ? {}
                    : { error: "Rejected" },
            }),
      );
    }
    await activeSave(page).click();
    await expect(activeSave(page)).toBeEnabled();
    await expect(page.getByLabel("Profile name", { exact: true })).toHaveValue("Portable VPN");
    expect(backend.writes).toHaveLength(1);
    const names = (await backend.snapshot()).config.profiles?.map((item) => item.name);
    expect(names).toEqual(failure === "stale" ? ["Concurrent edit"] : []);
    if (failure !== "stale") {
      await page.unroute("**/api/v1/system/config/apply");
      await saveImported(page);
      expect(backend.writes).toHaveLength(2);
      expect((await backend.snapshot()).config.profiles).toEqual([profile()]);
      await page.reload();
      await expect(page.getByLabel("Profile name", { exact: true })).toHaveValue("Portable VPN");
    }
  });
}

test("staging a used profile keeps both consumer counts and disables Delete", async ({
  page,
  backend,
}) => {
  const initial = (await backend.snapshot()).config;
  const usedSubscription = {
    id: "abcd1234",
    name: "Used list",
    url: "https://example.com/list.txt",
    interface: "wg0",
    profile: "portable_vpn",
    enable: true,
    priority: 100,
    interval: 86400,
    last_update: 0,
    rules: [],
  };
  await backend.save({
    ...initial,
    profiles: [profile()],
    groups: [group],
    subscriptions: [usedSubscription],
  });
  await page.goto("/");
  await showPage(page, "Settings");
  await expect(page.getByRole("button", { name: "Delete profile: Portable VPN" })).toBeDisabled();
  const dialog = await upload(page, { profiles: [profile("Imported VPN")] });
  await dialog.getByRole("switch", { name: "Replace", exact: true }).click();
  await dialog.getByRole("button", { name: "Import", exact: true }).click();
  await expect(dialog).toHaveCount(0);
  await expect(page.getByLabel("Profile name", { exact: true })).toHaveValue("Imported VPN");
  await expect(page.getByRole("button", { name: "Delete profile: Imported VPN" })).toBeDisabled();
  await expect(page.locator('[data-profile-id="portable_vpn"] .usage')).toContainText(
    "1 groups, 1 subscriptions",
  );
  expect(backend.writes).toEqual([]);
  await page.reload();
  await expect(page.getByRole("button", { name: "Delete profile: Portable VPN" })).toBeDisabled();
});

test("no-op import never acknowledges a previous applied-but-unpersisted group save", async ({
  page,
  backend,
}) => {
  const initial = (await backend.snapshot()).config;
  await backend.save({ ...initial, groups: [group], profiles: [profile()] });
  await page.goto("/");
  await expect(page.locator("input.group-name")).toHaveValue("Portable group");

  // Apply to the live mock, then report a failed disk write as the real API does.
  await page.route("**/api/v1/groups?save=true", async (route) => {
    const applied = await backend.api.put("/api/v1/groups?save=true", {
      data: route.request().postDataJSON(),
    });
    expect(applied.ok()).toBe(true);
    const response = await applied.json();
    await route.fulfill({
      status: 500,
      json: {
        ...response,
        code: "PERSISTENCE_FAILED",
        applied: true,
        error: "Changes active in memory only",
      },
    });
  });
  await page.locator("input.group-name").fill("Applied in memory");
  await page.locator("#save-changes").click();
  await expect(page.locator("#save-changes")).toBeEnabled();
  await expect
    .poll(async () => (await backend.snapshot()).config.groups[0].name)
    .toBe("Applied in memory");
  await page.unroute("**/api/v1/groups?save=true");

  // Establish an import session, undo its staged profile edit, then import
  // identical definitions. This enters the read-only acceptImportSave path.
  await showPage(page, "Settings");
  const staged = await upload(page, { profiles: [profile("Temporary name")] });
  await staged.getByRole("switch", { name: "Replace", exact: true }).click();
  await staged.getByRole("button", { name: "Import", exact: true }).click();
  await expect(staged).toHaveCount(0);
  await expect(page.getByLabel("Profile name", { exact: true })).toHaveValue("Temporary name");
  await page.getByLabel("Profile name", { exact: true }).fill("Portable VPN");
  const noop = await upload(page, { profiles: [profile()] });
  await noop.getByRole("button", { name: "Import", exact: true }).click();
  await expect(noop).toHaveCount(0);
  await expect(page.getByText("No changes to import", { exact: true })).toBeVisible();

  await showPage(page, "Groups");
  await expect(page.locator("#save-changes")).toBeEnabled();
  await expect
    .poll(() =>
      page.evaluate(() => {
        const event = new Event("beforeunload", { cancelable: true });
        window.dispatchEvent(event);
        return event.defaultPrevented;
      }),
    )
    .toBe(true);
  expect(backend.writes.filter((write) => write.path.endsWith("/system/config/apply"))).toEqual([]);
});

test("profile dependencies import with groups and Replace cannot remove a used profile", async ({
  page,
  backend,
}) => {
  await page.goto("/");
  const dialog = await upload(page, { groups: [group], profiles: [profile()] });
  await dialog.getByRole("button", { name: "Import", exact: true }).click();
  await expect(dialog).toHaveCount(0);
  expect(backend.writes).toEqual([]);
  expect((await backend.snapshot()).config.profiles).toEqual([]);
  await saveImported(page);
  expect((await backend.snapshot()).config.profiles).toEqual([profile()]);
  await expect(page.locator("input.group-name")).toHaveValue("Portable group");
  await showPage(page, "Settings");
  await expect(page.getByLabel("Profile name", { exact: true })).toHaveValue("Portable VPN");
  const clear = await upload(page, { profiles: [] });
  await clear.getByRole("switch", { name: "Replace", exact: true }).click();
  await expect(clear.getByRole("button", { name: "Import", exact: true })).toBeDisabled();
  expect((await backend.snapshot()).config.profiles).toEqual([profile()]);
});

test("unfinished profile can be exported and reopened without blocking unrelated valid groups", async ({
  page,
  backend,
}) => {
  await page.goto("/");
  await showPage(page, "Settings");
  await page.getByRole("button", { name: "Add profile", exact: true }).click();
  const exported = await download(page);
  expect(exported.profiles?.[0]).toMatchObject({ name: "", interfaces: [] });
  await page.reload();
  await showPage(page, "Groups");
  const dialog = await upload(page, { ...exported, groups: [{ ...group, profile: undefined }] });
  await dialog.getByRole("tab", { name: "Settings", exact: true }).click();
  const unfinished = dialog.getByRole("checkbox", { name: "Profiles 1", exact: true });
  await unfinished.check();
  await expect(dialog.getByRole("button", { name: "Import", exact: true })).toBeDisabled();
  await unfinished.uncheck();
  await dialog.getByRole("button", { name: "Import", exact: true }).click();
  await expect(dialog).toHaveCount(0);
  expect(backend.writes).toEqual([]);
  await saveImported(page);
  expect((await backend.snapshot()).config.profiles).toEqual([]);
  expect((await backend.snapshot()).config.groups[0].name).toBe("Portable group");
});

test(
  "policy imports and editor saves share the real mock configuration",
  { tag: "@entware_kn" },
  async ({ page, backend }) => {
    test.skip(process.env.VITE_ENTWARE_KN !== "1", "Keenetic-only bundle");
    await page.goto("/");
    await showPage(page, "Settings");
    const dialog = await upload(page, {
      settings: { keeneticPolicy: { enabled: true, policyId: "Policy1", mode: "exclude" } },
    });
    await dialog.getByRole("button", { name: "Import", exact: true }).click();
    await expect(dialog).toHaveCount(0);
    expect(backend.writes).toEqual([]);
    await saveImported(page);
    const read = await backend.api.get("/api/v1/keenetic/policy");
    expect((await read.json()).settings).toEqual({
      enabled: true,
      policy_id: "Policy1",
      mode: "exclude",
    });
    const write = await backend.api.put("/api/v1/keenetic/policy", {
      data: { enabled: true, policy_id: "Policy2", mode: "include" },
    });
    expect(write.ok()).toBe(true);
    expect((await backend.snapshot()).config.app.keeneticPolicy).toEqual({
      enabled: true,
      policyId: "Policy2",
      mode: "include",
    });
    expect((await download(page)).settings?.keeneticPolicy).toEqual({
      enabled: true,
      policyId: "Policy2",
      mode: "include",
    });
  },
);

test("Replace, explicit empty profiles and repeat imports preserve the confirmed collection", async ({
  page,
  backend,
}) => {
  const current = (await backend.snapshot()).config;
  await backend.save({
    ...current,
    profiles: [profile("Old VPN"), { ...profile("Spare"), id: "spare" }],
  });
  await page.goto("/");
  await showPage(page, "Settings");
  const replace = await upload(page, { profiles: [profile("Replacement VPN")] });
  await replace.getByRole("switch", { name: "Replace", exact: true }).click();
  await replace.getByRole("button", { name: "Import", exact: true }).click();
  await expect(replace).toHaveCount(0);
  expect(backend.writes).toEqual([]);
  await saveImported(page);
  expect((await backend.snapshot()).config.profiles).toEqual([profile("Replacement VPN")]);
  await expect(page.getByLabel("Profile name", { exact: true })).toHaveValue("Replacement VPN");
  const repeat = await upload(page, { profiles: [profile("Replacement VPN")] });
  await repeat.getByRole("button", { name: "Import", exact: true }).click();
  await expect(page.getByText("No changes to import", { exact: true })).toBeVisible();
  await expect(repeat).toHaveCount(0);
  const clear = await upload(page, { profiles: [] });
  await clear.getByRole("switch", { name: "Replace", exact: true }).click();
  await clear.getByRole("button", { name: "Import", exact: true }).click();
  await expect(clear).toHaveCount(0);
  await expect(page.getByLabel("Profile name", { exact: true })).toHaveCount(0);
  expect((await backend.snapshot()).config.profiles).toEqual([profile("Replacement VPN")]);
  await saveImported(page);
  expect((await backend.snapshot()).config.profiles).toEqual([]);
  await page.reload();
  await expect(page.getByText("No profiles yet", { exact: true })).toBeVisible();
});

test("double clicking Import only stages once and double clicking Save only applies once", async ({
  page,
  backend,
}) => {
  await page.goto("/");
  await showPage(page, "Settings");
  const dialog = await upload(page, { profiles: [profile()] });
  let writes = 0;
  page.on("request", (request) => {
    if (request.url().endsWith("/system/config/apply") && request.method() === "POST") writes++;
  });
  const importButton = dialog.getByRole("button", { name: "Import", exact: true });
  // Worker preparation deliberately disables Apply until the latest plan is ready.
  await expect(importButton).toBeEnabled();
  await importButton.evaluate((element: HTMLButtonElement) => {
    element.click();
    element.click();
  });
  await expect(dialog).toHaveCount(0);
  expect(writes).toBe(0);
  expect((await backend.snapshot()).config.profiles).toEqual([]);
  // Svelte updates the Save state after dialog teardown. Click only after
  // it becomes enabled, so this actually tests duplicate Save requests.
  await expect(activeSave(page)).toBeEnabled();
  await activeSave(page).evaluate((element: HTMLButtonElement) => {
    element.click();
    element.click();
  });
  await expect(activeSave(page)).toBeDisabled();
  expect(writes).toBe(1);
  expect((await backend.snapshot()).config.profiles).toEqual([profile()]);
});

for (const payload of [
  "{broken",
  '{"schemaVersion":999,"profiles":[]}',
  '{"profiles":[{"id":1}]}',
  '{"groups":{}}',
  '{"groups":[{"interface":"wg0","rules":[{"type":"namespace"}]}]}',
  '{"settings":{"__proto__":{"enabled":true}}}',
]) {
  test(`invalid configuration file is rejected without changing backend: ${payload}`, async ({
    page,
    backend,
  }) => {
    await page.goto("/");
    await showPage(page, "Settings");
    const before = await backend.snapshot();
    const chooser = page.waitForEvent("filechooser");
    await page.getByRole("button", { name: "Import Config", exact: true }).click();
    await (
      await chooser
    ).setFiles({
      name: "invalid.mtrickle",
      mimeType: "application/json",
      buffer: Buffer.from(payload),
    });
    await expect(page.getByText("Invalid config file", { exact: true })).toBeVisible();
    await expect(page.getByRole("dialog")).toHaveCount(0);
    expect((await backend.snapshot()).config).toEqual(before.config);
  });
}

test("a clean stale editor exports fresh server profiles and a no-op import refreshes their view", async ({
  page,
  backend,
}) => {
  await page.goto("/");
  await showPage(page, "Settings");
  await expect(page.getByText("No profiles yet", { exact: true })).toBeVisible();
  const current = (await backend.snapshot()).config;
  await backend.save({ ...current, profiles: [profile("Created on server")] });
  const exported = await download(page);
  expect(exported.profiles).toEqual([profile("Created on server")]);
  const dialog = await upload(page, exported);
  await dialog.getByRole("button", { name: "Import", exact: true }).click();
  await expect(page.getByText("No changes to import", { exact: true })).toBeVisible();
  await expect(page.getByText("Configuration imported", { exact: true })).toHaveCount(0);
  await expect(dialog).toHaveCount(0);
  await expect(page.getByLabel("Profile name", { exact: true })).toHaveValue("Created on server");
  expect((await backend.snapshot()).config.profiles).toEqual([profile("Created on server")]);
});

test("subscription rule overrides survive save, export, import and reload", async ({
  page,
  backend,
}) => {
  const current = (await backend.snapshot()).config;
  const subscription = {
    id: "55667788",
    name: "Rule export",
    interface: "wg0",
    url: "https://example.org/list",
    enable: true,
    priority: 100,
    interval: 86400,
    last_update: 1,
    rules: [{ id: "aabbccdd", rule: "example.org", type: "namespace", enable: true }],
    overrides: [],
  };
  await backend.save({ ...current, subscriptions: [subscription] });
  await page.goto("/");
  await showPage(page, "Subscriptions");
  await page.locator(".subscription-panel [data-collapsible-trigger]").click();
  await page.locator(".subscription-rule").getByRole("switch").click();
  await page.locator("#save-subscriptions").click();
  await expect(page.locator("#save-subscriptions")).toBeDisabled();
  const exported = await download(page);
  expect(exported.subscriptions?.[0].overrides).toEqual([{ rule: "example.org", enable: false }]);
  expect(exported.subscriptions?.[0]).not.toHaveProperty("rules");
  expect((await backend.snapshot()).config.subscriptions[0].overrides).toEqual(
    exported.subscriptions?.[0].overrides,
  );
  await backend.save({ ...current, subscriptions: [] });
  await page.reload();
  const dialog = await upload(page, exported);
  await dialog.getByRole("button", { name: "Import", exact: true }).click();
  await expect(dialog).toHaveCount(0);
  await expect(page.locator(".subscription-name")).toHaveValue("Rule export");
  expect((await backend.snapshot()).config.subscriptions).toEqual([]);
  await saveImported(page);
  const restored = (await backend.snapshot()).config.subscriptions[0];
  expect(restored.overrides).toEqual([{ rule: "example.org", enable: false }]);
  expect(restored.rules).toEqual([]); // Download cache is not a portable rule list.
  await page.reload();
  expect((await download(page)).subscriptions?.[0].overrides).toEqual(
    exported.subscriptions?.[0].overrides,
  );
});

for (const width of [1280, 390]) {
  for (const [kind, policyId] of [
    ["control character", "Foreign\x1f0"],
    ["128 UTF-8 bytes", "я".repeat(64)],
  ]) {
    test(
      `invalid foreign policy (${kind}) blocks atomic import until mapped (${width}px)`,
      { tag: "@entware_kn" },
      async ({ page, backend }) => {
        test.skip(process.env.VITE_ENTWARE_KN !== "1", "Keenetic-only bundle");
        await page.setViewportSize({ width, height: width === 390 ? 844 : 900 });
        await page.goto("/");
        const before = (await backend.snapshot()).config;
        let applyRequests = 0;
        page.on("request", (request) => {
          if (new URL(request.url()).pathname === "/api/v1/system/config/apply") applyRequests++;
        });
        const dialog = await upload(page, {
          groups: [group],
          profiles: [profile()],
          settings: { keeneticPolicy: { enabled: true, policyId, mode: "exclude" } },
        });
        await dialog.getByRole("tab", { name: "Settings", exact: true }).click();
        await dialog.getByRole("button", { name: "All", exact: true }).click();
        const button = dialog.getByRole("button", { name: "Import", exact: true });
        await expect(button).toBeDisabled();
        await expect(button).toHaveAccessibleDescription(
          "Policy ID must be shorter than 128 UTF-8 bytes and contain no control characters",
        );
        expect(applyRequests).toBe(0);
        expect((await backend.snapshot()).config).toEqual(before);

        const policySelection = dialog.getByRole("checkbox", {
          name: "Routing policy",
          exact: true,
        });
        await policySelection.uncheck();
        await expect(button).toBeEnabled();
        await policySelection.check();
        await expect(button).toBeDisabled();

        const { policies } = await (await backend.api.get("/api/v1/keenetic/policies")).json();
        const local = policies.find((policy: { id: string }) => policy.id === "Policy0");
        await dialog.locator('[data-select-trigger][aria-label="Keenetic policy mapping"]').click();
        await page.getByRole("option", { name: local.name, exact: true }).click();
        await expect(button).toBeEnabled();
        await button.click();
        await expect(dialog).toHaveCount(0);
        expect(applyRequests).toBe(0);
        expect((await backend.snapshot()).config).toEqual(before);
        await saveImported(page);
        expect(applyRequests).toBe(1);
        const applied = (await backend.snapshot()).config;
        expect(applied.groups).toEqual([group]);
        expect(applied.app.keeneticPolicy).toEqual({
          enabled: true,
          policyId: local.id,
          mode: "exclude",
        });
      },
    );
  }
  test(
    `a foreign Keenetic policy can be imported and saved before it exists (${width}px)`,
    { tag: "@entware_kn" },
    async ({ page, backend }) => {
      test.skip(process.env.VITE_ENTWARE_KN !== "1", "Keenetic-only bundle");
      await page.setViewportSize({ width, height: width === 390 ? 844 : 900 });
      await page.goto("/");
      await showPage(page, "Settings");
      const dialog = await upload(page, {
        settings: {
          keeneticPolicy: { enabled: true, policyId: "ForeignPolicy73", mode: "exclude" },
        },
      });
      await expect(
        dialog.locator('[data-select-trigger][aria-label="Keenetic policy mapping"]'),
      ).toHaveText("ForeignPolicy73");
      await expect(dialog.getByRole("button", { name: "Import", exact: true })).toBeEnabled();
      await dialog.getByRole("button", { name: "Import", exact: true }).click();
      await expect(dialog).toHaveCount(0);
      expect(backend.writes).toEqual([]);
      await saveImported(page);
      expect((await backend.snapshot()).config.app.keeneticPolicy).toEqual({
        enabled: true,
        policyId: "ForeignPolicy73",
        mode: "exclude",
      });
      await page.getByRole("tab", { name: "Keenetic", exact: true }).click();
      const panel = page.locator(".policy-settings");
      await panel.getByRole("switch", { name: "Processing mode", exact: true }).click();
      await expect(page.locator("#save-settings")).toBeEnabled();
      await page.locator("#save-settings").click();
      await expect(page.locator("#save-settings")).toBeDisabled();
      await page.reload();
      await page.getByRole("tab", { name: "Keenetic", exact: true }).click();
      await expect(
        panel.locator('[data-select-trigger][aria-label="Router policy"]'),
      ).toContainText("ForeignPolicy73");
      expect((await backend.snapshot()).config.app.keeneticPolicy).toEqual({
        enabled: true,
        policyId: "ForeignPolicy73",
        mode: "include",
      });
      const state = await (await backend.api.get("/api/v1/keenetic/policy")).json();
      expect(state.runtime).toEqual({ state: "unavailable", reason: "POLICY_NOT_FOUND" });
    },
  );
}

for (const width of [1280, 390]) {
  for (const source of ["Groups", "Subscriptions", "Settings"] as const) {
    for (const mode of ["Append", "Replace"] as const) {
      test(`${mode} from ${source} remains a draft and F5 discards it at ${width}px`, async ({
        page,
        backend,
      }) => {
        await page.setViewportSize({ width, height: width === 390 ? 844 : 900 });
        const current = (await backend.snapshot()).config;
        const savedGroup = { ...group, profile: undefined, name: "Saved group" };
        const savedSubscription = {
          id: "33445566",
          name: "Saved subscription",
          url: "https://example.org/saved",
          interface: "wg0",
          enable: true,
          priority: 100,
          interval: 86400,
          last_update: 0,
          rules: [],
          overrides: [],
        };
        await backend.save({
          ...current,
          groups: [savedGroup],
          subscriptions: [savedSubscription],
          profiles: [profile("Saved profile")],
        });
        const before = await backend.snapshot();
        await page.goto("/");
        await showPage(page, source);
        const dialog = await upload(page, {
          groups: [{ ...savedGroup, id: "22334455", name: "Draft group", rules: [] }],
          subscriptions: [
            {
              ...savedSubscription,
              id: "44556677",
              name: "Draft subscription",
              url: "https://example.org/draft",
            },
          ],
          profiles: [{ ...profile("Draft profile"), id: "draft_profile" }],
        });
        if (mode === "Replace")
          await dialog.getByRole("switch", { name: "Replace", exact: true }).click();
        await dialog.getByRole("button", { name: "Import", exact: true }).click();
        await expect(dialog).toHaveCount(0);
        expect(backend.writes).toEqual([]);
        expect(await backend.snapshot()).toEqual(before);
        await expect(activeSave(page)).toBeEnabled();
        const names =
          source === "Groups"
            ? page.locator("input.group-name")
            : source === "Subscriptions"
              ? page.locator(".subscription-name")
              : page.getByLabel("Profile name", { exact: true });
        const kind =
          source === "Groups" ? "group" : source === "Subscriptions" ? "subscription" : "profile";
        await expect(names).toHaveCount(mode === "Replace" ? 1 : 2);
        await expect(names.last()).toHaveValue(`Draft ${kind}`);
        await names.last().fill(`Edited ${kind}`);
        const exported = await download(page);
        const section =
          source === "Groups"
            ? "groups"
            : source === "Subscriptions"
              ? "subscriptions"
              : "profiles";
        expect(exported[section]?.at(-1)?.name).toBe(`Edited ${kind}`);
        expect(backend.writes).toEqual([]);
        expect(await backend.snapshot()).toEqual(before);
        await showPage(page, source === "Groups" ? "Settings" : "Groups");
        await showPage(page, source);
        await expect(names.last()).toHaveValue(`Edited ${kind}`);
        await page.reload();
        await expect(names).toHaveCount(1);
        await expect(names).toHaveValue(`Saved ${kind}`);
        await expect(activeSave(page)).toBeDisabled();
        expect(backend.writes).toEqual([]);
        expect(await backend.snapshot()).toEqual(before);
      });
    }
  }

  test(`the same Replace file can be imported again after edits, Save and a no-op at ${width}px`, async ({
    page,
    backend,
  }) => {
    await page.setViewportSize({ width, height: width === 390 ? 844 : 900 });
    const current = (await backend.snapshot()).config;
    await backend.save({
      ...current,
      groups: [{ ...group, profile: undefined, name: "Previous group" }],
    });
    await page.goto("/");
    const groups = Array.from({ length: 22 }, (_, index) => ({
      ...group,
      id: (0x20000000 + index).toString(16),
      name: index === 0 ? "TikTok" : `Imported group ${index}`,
      profile: undefined,
      rules: Array.from({ length: 8 }, (_, ruleIndex) => ({
        id: (0x40000000 + index * 16 + ruleIndex).toString(16),
        rule: `rule-${index}-${ruleIndex}.example.org`,
        type: "namespace",
        enable: true,
      })),
    }));
    const file = { format: "mt-c-config", schemaVersion: 1, groups };
    const filename = width === 390 ? "config.mtrickle.json" : "config.mtrickle";
    let dialog = await upload(page, file, filename);
    await dialog.getByRole("switch", { name: "Replace", exact: true }).click();
    await dialog.getByRole("button", { name: "Import", exact: true }).click();
    await expect(dialog).toHaveCount(0);
    expect(backend.writes).toEqual([]);
    await expect(page.locator("input.group-name").first()).toHaveValue("TikTok");
    await page.locator("input.group-name").first().fill("TikTok edited");
    await page.locator(".group-header [data-collapsible-trigger]").first().click();
    const ruleName = page.locator(".rule .name input").first();
    await ruleName.fill("Edited rule");
    await saveImported(page);
    const first = await backend.snapshot();
    expect(first.config.groups).toHaveLength(22);
    expect(first.config.groups[0].name).toBe("TikTok edited");
    expect(first.config.groups[0].rules[0].name).toBe("Edited rule");
    expect(backend.writes).toHaveLength(1);
    dialog = await upload(page, file, filename);
    await dialog.getByRole("switch", { name: "Replace", exact: true }).click();
    await expect(dialog.getByRole("button", { name: "Import", exact: true })).toBeEnabled();
    await dialog.getByRole("button", { name: "Import", exact: true }).click();
    await expect(dialog).toHaveCount(0);
    await expect(page.locator("input.group-name").first()).toHaveValue("TikTok");
    expect(backend.writes).toHaveLength(1);
    expect(await backend.snapshot()).toEqual(first);
    await saveImported(page);
    expect(backend.writes).toHaveLength(2);
    const restored = (await backend.snapshot()).config.groups;
    expect(restored.map((item) => item.id)).toEqual(groups.map((item) => item.id));
    expect(restored[0].name).toBe("TikTok");
    expect(restored[0].rules[0].name ?? "").toBe("");
    dialog = await upload(page, file, filename);
    await dialog.getByRole("switch", { name: "Replace", exact: true }).click();
    await dialog.getByRole("button", { name: "Import", exact: true }).click();
    await expect(dialog).toHaveCount(0);
    await expect(page.getByText("No changes to import", { exact: true })).toBeVisible();
    await expect(activeSave(page)).toBeDisabled();
    expect(backend.writes).toHaveLength(2);
    await page.reload();
    await expect(page.locator("input.group-name").first()).toHaveValue("TikTok");
    await expect(activeSave(page)).toBeDisabled();
  });
}

for (const source of ["Groups", "Subscriptions"] as const) {
  for (const choice of ["Keep current", "Use imported", "Create a copy"] as const) {
    test(`${source} Append conflict ${choice} stages the expected collection before saving`, async ({
      page,
      backend,
    }) => {
      const section = source === "Groups" ? "groups" : "subscriptions";
      const current = (await backend.snapshot()).config;
      const item =
        source === "Groups"
          ? { ...group, profile: undefined, name: "Existing" }
          : {
              id: "11223344",
              name: "Existing",
              url: "https://example.org/current",
              interface: "wg0",
              enable: true,
              priority: 100,
              interval: 86400,
              last_update: 0,
              rules: [],
              overrides: [],
            };
      await backend.save({ ...current, [section]: [item] });
      const before = await backend.snapshot();
      await page.goto("/");
      await showPage(page, source);
      const dialog = await upload(page, { [section]: [{ ...item, name: "Imported" }] });
      await dialog
        .locator('[data-select-trigger][aria-label="Resolve conflict for Imported"]')
        .click();
      await page.getByRole("option", { name: choice, exact: true }).click();
      await dialog.getByRole("button", { name: "Import", exact: true }).click();
      await expect(dialog).toHaveCount(0);
      expect(backend.writes).toEqual([]);
      expect(await backend.snapshot()).toEqual(before);
      const names =
        source === "Groups" ? page.locator("input.group-name") : page.locator(".subscription-name");
      const expected =
        choice === "Create a copy"
          ? ["Existing", "Imported"]
          : [choice === "Keep current" ? "Existing" : "Imported"];
      await expect(names).toHaveCount(expected.length);
      for (let i = 0; i < expected.length; i++) await expect(names.nth(i)).toHaveValue(expected[i]);
      if (choice === "Keep current") {
        await expect(activeSave(page)).toBeDisabled();
        return;
      }
      if (source === "Subscriptions" && choice === "Create a copy") {
        await expect(activeSave(page)).toBeDisabled();
        await page.locator(".subscription-url-input").nth(1).fill("https://example.org/copied");
      }
      await saveImported(page);
      const saved = (await backend.snapshot()).config;
      expect(saved[section].map((entry) => entry.name)).toEqual(expected);
      if (source === "Groups" && choice === "Create a copy") {
        const ids = saved.groups.flatMap((entry) => [
          entry.id,
          ...entry.rules.map((rule) => rule.id),
        ]);
        expect(new Set(ids).size).toBe(ids.length);
      }
      expect(backend.writes).toHaveLength(1);
      await page.reload();
      await expect(names).toHaveCount(expected.length);
      await expect(activeSave(page)).toBeDisabled();
    });
  }
  test(`deleting a saved ${source.toLowerCase()} item after Replace only changes the draft`, async ({
    page,
    backend,
  }) => {
    const section = source === "Groups" ? "groups" : "subscriptions";
    const current = (await backend.snapshot()).config;
    const original =
      source === "Groups"
        ? { ...group, profile: undefined, name: "Saved item" }
        : {
            id: "11223344",
            name: "Saved item",
            url: "https://example.org/current",
            interface: "wg0",
            enable: true,
            priority: 100,
            interval: 86400,
            last_update: 0,
            rules: [],
            overrides: [],
          };
    await backend.save({ ...current, [section]: [original] });
    const before = await backend.snapshot();
    await page.goto("/");
    await showPage(page, source);
    const dialog = await upload(page, {
      [section]: [
        { ...original, name: "Imported replacement" },
        { ...original, id: "55667788", name: "Keep this item", rules: [] },
      ],
    });
    await dialog.getByRole("switch", { name: "Replace", exact: true }).click();
    await dialog.getByRole("button", { name: "Import", exact: true }).click();
    await expect(dialog).toHaveCount(0);
    page.once("dialog", (confirmation) => confirmation.accept());
    const panel = page
      .locator(source === "Groups" ? ".group-wrapper" : ".subscription-panel")
      .first();
    await panel
      .locator(
        `[data-value="${source === "Groups" ? "Delete Group" : "Delete Subscription"}"] button`,
      )
      .click();
    const names = page.locator(source === "Groups" ? "input.group-name" : ".subscription-name");
    await expect(names).toHaveCount(1);
    await expect(names).toHaveValue("Keep this item");
    expect(backend.writes).toEqual([]);
    expect(await backend.snapshot()).toEqual(before);
    await saveImported(page);
    expect((await backend.snapshot()).config[section].map((entry) => entry.name)).toEqual([
      "Keep this item",
    ]);
    expect(backend.writes.map((entry) => entry.path)).toEqual(["/api/v1/system/config/apply"]);
  });
}

test("sequential imports and edits save atomically from another page without losing sections", async ({
  page,
  backend,
}) => {
  const current = (await backend.snapshot()).config;
  await backend.save({
    ...current,
    groups: [{ ...group, profile: undefined, name: "Saved group" }],
  });
  const before = await backend.snapshot();
  await page.goto("/");
  await page.locator("input.group-name").fill("Edited group");
  await showPage(page, "Subscriptions");
  let dialog = await upload(page, {
    subscriptions: [
      {
        id: "22334455",
        name: "Imported subscription",
        url: "https://example.org/list",
        interface: "wg0",
        enable: true,
        priority: 100,
        interval: 86400,
      },
    ],
  });
  await dialog.getByRole("button", { name: "Import", exact: true }).click();
  await expect(dialog).toHaveCount(0);
  await page.locator(".subscription-name").fill("Edited subscription");
  await showPage(page, "Settings");
  dialog = await upload(page, { profiles: [profile("Imported profile")] });
  await dialog.getByRole("button", { name: "Import", exact: true }).click();
  await expect(dialog).toHaveCount(0);
  await page.getByLabel("Profile name", { exact: true }).fill("Edited profile");
  expect(backend.writes).toEqual([]);
  expect(await backend.snapshot()).toEqual(before);
  await showPage(page, "Groups");
  await saveImported(page);
  const saved = (await backend.snapshot()).config;
  expect(saved.groups[0].name).toBe("Edited group");
  expect(saved.subscriptions[0].name).toBe("Edited subscription");
  expect(saved.profiles?.[0].name).toBe("Edited profile");
  expect(backend.writes.map((entry) => entry.path)).toEqual(["/api/v1/system/config/apply"]);
  await page.reload();
  expect((await download(page)).profiles?.[0].name).toBe("Edited profile");
});

test("canceling the import dialog preserves the existing draft and performs no write", async ({
  page,
  backend,
}) => {
  const current = (await backend.snapshot()).config;
  await backend.save({
    ...current,
    groups: [{ ...group, profile: undefined, name: "Saved group" }],
  });
  const before = await backend.snapshot();
  await page.goto("/");
  await page.locator("input.group-name").fill("Existing draft");
  const dialog = await upload(page, {
    groups: [{ ...group, profile: undefined, name: "Replacement" }],
  });
  await dialog.getByRole("switch", { name: "Replace", exact: true }).click();
  await dialog.getByRole("button", { name: "Cancel", exact: true }).click();
  await expect(dialog).toHaveCount(0);
  await expect(page.locator("input.group-name")).toHaveValue("Existing draft");
  await expect(activeSave(page)).toBeEnabled();
  expect(backend.writes).toEqual([]);
  expect(await backend.snapshot()).toEqual(before);
});

for (const mode of ["Append", "Replace"] as const) {
  test(`repeating ${mode} before Save keeps the pending draft and Ctrl+S saves it once`, async ({
    page,
    backend,
  }) => {
    await page.goto("/");
    const file = { profiles: [profile()] };
    for (let round = 0; round < 2; round++) {
      const dialog = await upload(page, file);
      if (mode === "Replace")
        await dialog.getByRole("switch", { name: "Replace", exact: true }).click();
      await dialog.getByRole("button", { name: "Import", exact: true }).click();
      await expect(dialog).toHaveCount(0);
      await expect(activeSave(page)).toBeEnabled();
      expect(backend.writes).toEqual([]);
      expect((await backend.snapshot()).config.profiles).toEqual([]);
    }
    await page.keyboard.press("Control+s");
    await expect(activeSave(page)).toBeDisabled();
    expect(backend.writes.map((entry) => entry.path)).toEqual(["/api/v1/system/config/apply"]);
    expect((await backend.snapshot()).config.profiles).toEqual([profile()]);
    await page.reload();
    await showPage(page, "Settings");
    await expect(page.getByLabel("Profile name", { exact: true })).toHaveValue("Portable VPN");
  });
}

test("adding and syncing subscriptions during a pending import cannot write before Save", async ({
  page,
  backend,
}) => {
  await page.goto("/");
  await showPage(page, "Subscriptions");
  const before = await backend.snapshot();
  const dialog = await upload(page, {
    subscriptions: [
      {
        id: "22334455",
        name: "Imported subscription",
        url: "https://example.org/imported",
        interface: "wg0",
        enable: true,
        priority: 100,
        interval: 86400,
      },
    ],
  });
  await dialog.getByRole("button", { name: "Import", exact: true }).click();
  await expect(dialog).toHaveCount(0);
  await page.route("**/api/v1/subscriptions/rules?**", (route) =>
    route.fulfill({ json: { count: 1, types: { namespace: 1 } } }),
  );
  await page.getByRole("button", { name: "Add Subscription", exact: true }).click();
  const add = page.getByRole("dialog");
  await add.getByLabel("URL", { exact: true }).fill("https://example.org/added");
  await add.getByRole("button", { name: "Next", exact: true }).click();
  await add.getByLabel("Name", { exact: true }).fill("Added draft subscription");
  await add.getByRole("button", { name: "Add", exact: true }).click();
  await expect(add).toHaveCount(0);
  await expect(page.locator(".subscription-name")).toHaveCount(2);
  await page.locator(".action.sync button").first().click();
  await expect(
    page.getByText("Save the imported configuration before syncing subscriptions.", {
      exact: true,
    }),
  ).toBeVisible();
  expect(backend.writes).toEqual([]);
  expect(await backend.snapshot()).toEqual(before);
  await saveImported(page);
  expect((await backend.snapshot()).config.subscriptions.map((entry) => entry.name)).toEqual([
    "Added draft subscription",
    "Imported subscription",
  ]);
  expect(backend.writes.map((entry) => entry.path)).toEqual(["/api/v1/system/config/apply"]);
});

for (const failure of ["unavailable", "malformed", "mismatched"] as const) {
  test(`a saved import recovers after ${failure} confirmation without applying twice`, async ({
    page,
    backend,
  }) => {
    await page.goto("/");
    await showPage(page, "Settings");
    const before = await backend.snapshot();
    const dialog = await upload(page, { profiles: [profile()] });
    await dialog.getByRole("button", { name: "Import", exact: true }).click();
    await expect(dialog).toHaveCount(0);
    let committed = false;
    page.on("response", (response) => {
      if (response.url().endsWith("/system/config/apply") && response.ok()) committed = true;
    });
    await page.route("**/api/v1/system/config/snapshot", (route) =>
      committed
        ? route.fulfill({
            status: failure === "unavailable" ? 503 : 200,
            json:
              failure === "mismatched"
                ? before
                : failure === "malformed"
                  ? {}
                  : { error: "Snapshot unavailable" },
          })
        : route.fallback(),
    );
    await activeSave(page).click();
    await expect(
      page.getByText("Failed to save imported configuration", { exact: true }),
    ).toBeVisible();
    await expect(activeSave(page)).toBeEnabled();
    await expect(page.getByLabel("Profile name", { exact: true })).toHaveValue("Portable VPN");
    expect((await backend.snapshot()).config.profiles).toEqual([profile()]);
    expect(backend.writes).toHaveLength(1);
    if (failure === "unavailable") {
      await page
        .getByLabel("Profile name", { exact: true })
        .fill("Edited after failed confirmation");
    }
    await page.unroute("**/api/v1/system/config/snapshot");
    await activeSave(page).click();
    await expect(page.getByText("Saved", { exact: true })).toBeVisible();
    expect(backend.writes).toHaveLength(1);
    if (failure === "unavailable") {
      await expect(page.getByLabel("Profile name", { exact: true })).toHaveValue(
        "Edited after failed confirmation",
      );
      await expect(activeSave(page)).toBeEnabled();
      await saveImported(page);
      expect((await backend.snapshot()).config.profiles?.[0].name).toBe(
        "Edited after failed confirmation",
      );
      expect(backend.writes.map((entry) => entry.path)).toEqual([
        "/api/v1/system/config/apply",
        "/api/v1/profiles",
      ]);
    } else {
      await expect(activeSave(page)).toBeDisabled();
    }
  });
}

for (const width of [1280, 390]) {
  test(
    `Keenetic policy import survives tab changes, F5 cancels it and Save can repeat it at ${width}px`,
    { tag: "@entware_kn" },
    async ({ page, backend }) => {
      test.skip(process.env.VITE_ENTWARE_KN !== "1", "Keenetic-only bundle");
      await page.setViewportSize({ width, height: width === 390 ? 844 : 900 });
      const current = (await backend.snapshot()).config;
      await backend.save({
        ...current,
        app: {
          ...current.app,
          keeneticPolicy: { enabled: true, policyId: "Policy0", mode: "include" },
        },
      });
      const before = await backend.snapshot();
      await page.goto("/");
      await showPage(page, "Settings");
      const file = {
        settings: { keeneticPolicy: { enabled: true, policyId: "Policy1", mode: "exclude" } },
      };
      let dialog = await upload(page, file);
      await dialog.getByRole("switch", { name: "Replace", exact: true }).click();
      await dialog.getByRole("button", { name: "Import", exact: true }).click();
      await expect(dialog).toHaveCount(0);
      await page.getByRole("tab", { name: "Keenetic", exact: true }).click();
      const panel = page.locator(".policy-settings");
      const policy = panel.locator('[data-select-trigger][aria-label="Router policy"]');
      const mode = panel.getByRole("switch", { name: "Processing mode", exact: true });
      const policies = (await (await backend.api.get("/api/v1/keenetic/policies")).json()).policies;
      const label = (id: string) =>
        policies.find((entry: { id: string; name: string }) => entry.id === id).name || id;
      await expect(policy).toContainText(label("Policy1"));
      await expect(mode).toBeChecked();
      await mode.click();
      await showPage(page, "Groups");
      await showPage(page, "Settings");
      await expect(policy).toContainText(label("Policy1"));
      await expect(mode).not.toBeChecked();
      expect(backend.writes).toEqual([]);
      expect(await backend.snapshot()).toEqual(before);
      await page.reload();
      await expect(policy).toContainText(label("Policy0"));
      await expect(mode).not.toBeChecked();
      await expect(activeSave(page)).toBeDisabled();
      dialog = await upload(page, file);
      await dialog.getByRole("button", { name: "Import", exact: true }).click();
      await expect(dialog).toHaveCount(0);
      expect(backend.writes).toEqual([]);
      await showPage(page, "Groups");
      await saveImported(page);
      expect((await backend.snapshot()).config.app.keeneticPolicy).toEqual(
        file.settings.keeneticPolicy,
      );
      expect(backend.writes.map((entry) => entry.path)).toEqual(["/api/v1/system/config/apply"]);
      await showPage(page, "Settings");
      dialog = await upload(page, file);
      await dialog.getByRole("button", { name: "Import", exact: true }).click();
      await expect(dialog).toHaveCount(0);
      await expect(activeSave(page)).toBeDisabled();
      expect(backend.writes).toHaveLength(1);
    },
  );
}

test("another import after lost save confirmation retains the new dependency and later edits", async ({
  page,
  backend,
}) => {
  await page.goto("/");
  let dialog = await upload(page, { groups: [group], profiles: [profile()] });
  await dialog.getByRole("button", { name: "Import", exact: true }).click();
  await expect(dialog).toHaveCount(0);
  let applied = false;
  page.on("response", (response) => {
    if (response.url().endsWith("/system/config/apply") && response.ok()) applied = true;
  });
  await page.route("**/api/v1/system/config/snapshot", (route) =>
    applied
      ? route.fulfill({ status: 503, json: { error: "Snapshot unavailable" } })
      : route.fallback(),
  );
  await activeSave(page).click();
  await expect(
    page.getByText("Failed to save imported configuration", { exact: true }),
  ).toBeVisible();
  await page.locator("input.group-name").fill("Edited after save");
  await page.unroute("**/api/v1/system/config/snapshot");
  dialog = await upload(page, {
    groups: [{ ...group, id: "22334455", name: "Second group", profile: "second_vpn", rules: [] }],
    profiles: [{ ...profile("Second VPN"), id: "second_vpn" }],
  });
  await dialog.getByRole("button", { name: "Import", exact: true }).click();
  await expect(dialog).toHaveCount(0);
  await expect(page.locator("input.group-name")).toHaveCount(2);
  await expect(page.locator("input.group-name").first()).toHaveValue("Edited after save");
  expect(backend.writes).toHaveLength(1);
  expect((await backend.snapshot()).config.groups.map((entry) => entry.name)).toEqual([
    "Portable group",
  ]);
  await saveImported(page);
  const saved = (await backend.snapshot()).config;
  expect(saved.groups.map((entry) => entry.name)).toEqual(["Edited after save", "Second group"]);
  expect(saved.profiles?.map((entry) => entry.id)).toEqual(["portable_vpn", "second_vpn"]);
  expect(saved.groups[1].profile).toBe("second_vpn");
  expect(backend.writes.map((entry) => entry.path)).toEqual([
    "/api/v1/system/config/apply",
    "/api/v1/system/config/apply",
  ]);
});

test("importing a profile primary updates its consumers while retaining unrelated group edits", async ({
  page,
  backend,
}) => {
  const current = (await backend.snapshot()).config;
  await backend.save({ ...current, groups: [group], profiles: [profile("Saved VPN")] });
  await page.goto("/");
  await page.locator("input.group-name").fill("Unsaved group name");
  await showPage(page, "Settings");
  const importedProfile = { ...profile("Imported VPN"), interfaces: ["nwg0", "wg0"] };
  const dialog = await upload(page, { profiles: [importedProfile] });
  await dialog.getByRole("switch", { name: "Replace", exact: true }).click();
  await dialog.getByRole("button", { name: "Import", exact: true }).click();
  await expect(dialog).toHaveCount(0);
  expect(backend.writes).toEqual([]);
  expect((await backend.snapshot()).config.groups[0].interface).toBe("wg0");
  await saveImported(page);
  const saved = (await backend.snapshot()).config;
  expect(saved.profiles).toEqual([importedProfile]);
  expect(saved.groups[0]).toMatchObject({
    name: "Portable group",
    profile: "portable_vpn",
    interface: "nwg0",
  });
  await showPage(page, "Groups");
  await expect(page.locator("input.group-name")).toHaveValue("Unsaved group name");
  await expect(page.locator('.group-wrapper [data-select-trigger][aria-label="Route"]')).toHaveText(
    "Imported VPN",
  );
  await expect(activeSave(page)).toBeEnabled();
  expect(backend.writes).toHaveLength(1);
  await saveImported(page);
  expect((await backend.snapshot()).config.groups[0]).toMatchObject({
    name: "Unsaved group name",
    interface: "nwg0",
  });
  await page.reload();
  await expect(page.locator("input.group-name")).toHaveValue("Unsaved group name");
  await expect(activeSave(page)).toBeDisabled();
});
