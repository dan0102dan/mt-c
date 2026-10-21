import { spawn } from "node:child_process";
import { once } from "node:events";
import fs from "node:fs/promises";
import { test as base, expect, type APIRequestContext, type Page } from "@playwright/test";

import type { ServerConfig, ServerSnapshot } from "../../src/modules/config-transfer/adapter";
import type { PortableConfig } from "../../src/modules/config-transfer/model";

// Each test runs the real Deno mock in its own process, including its actual
// snapshot/apply and collection routes. The user's dev server is never changed.
const test = base.extend<{
  backend: {
    api: APIRequestContext;
    snapshot: () => Promise<ServerSnapshot>;
    save: (config: ServerConfig) => Promise<void>;
  };
}>({
  backend: async ({ playwright, page }, use) => {
    const child = spawn("deno", ["run", "-A", "--unstable-sloppy-imports", "dev/backend-mock.ts"], {
      env: { ...process.env, MOCK_PORT: "0" },
      stdio: ["ignore", "pipe", "pipe"],
    });
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
        await use({ api, snapshot, save });
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
  if ((await menu.isVisible()) && (await menu.getAttribute("aria-expanded")) !== "true")
    await menu.click();
  await page.getByRole("tab", { name, exact: true }).click();
}
async function upload(page: Page, input: PortableConfig) {
  const chooser = page.waitForEvent("filechooser");
  await page.getByRole("button", { name: "Import Config", exact: true }).click();
  await (
    await chooser
  ).setFiles({
    name: "config.mtrickle",
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
      .getByRole("button", { name: "Resolve conflict for Imported VPN", exact: true })
      .click();
    await page.getByRole("option", { name: choice, exact: true }).click();
    await dialog.getByRole("button", { name: "Import", exact: true }).click();
    if (choice === "Keep current") {
      await expect(page.getByText("No changes to import", { exact: true })).toBeVisible();
      await expect(dialog).toBeVisible();
      expect((await backend.snapshot()).config.profiles).toEqual([profile("Existing VPN")]);
    } else {
      await expect(dialog).toHaveCount(0);
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

for (const failure of ["ignored", "malformed", "rejected", "stale", "refresh", "empty-view"]) {
  test(`profile import does not claim success after ${failure} response`, async ({
    page,
    backend,
  }) => {
    await page.goto("/");
    await showPage(page, "Settings");
    const dialog = await upload(page, { profiles: [profile()] });
    if (failure === "stale") {
      const current = (await backend.snapshot()).config;
      await backend.save({ ...current, profiles: [profile("Concurrent edit")] });
    } else if (failure === "refresh") {
      await page.route("**/api/v1/profiles", (route) =>
        route.fulfill({ status: 500, json: { error: "Unavailable" } }),
      );
    } else if (failure === "empty-view") {
      await page.route("**/api/v1/profiles", (route) => route.fulfill({ json: { profiles: [] } }));
    } else {
      await page.route("**/api/v1/system/config/apply", (route) =>
        route.fulfill({
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
    await dialog.getByRole("button", { name: "Import", exact: true }).click();
    await expect(page.getByText("Failed to import configuration", { exact: true })).toBeVisible();
    await expect(page.getByText("Configuration imported", { exact: true })).toHaveCount(0);
    await expect(dialog).toBeVisible();
    const names = (await backend.snapshot()).config.profiles?.map((item) => item.name);
    expect(names).toEqual(
      failure === "stale"
        ? ["Concurrent edit"]
        : failure === "refresh" || failure === "empty-view"
          ? ["Portable VPN"]
          : [],
    );
  });
}

test("profile dependencies import with groups and Replace cannot remove a used profile", async ({
  page,
  backend,
}) => {
  await page.goto("/");
  const dialog = await upload(page, { groups: [group], profiles: [profile()] });
  await dialog.getByRole("button", { name: "Import", exact: true }).click();
  await expect(dialog).toHaveCount(0);
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
  expect((await backend.snapshot()).config.profiles).toEqual([]);
  expect((await backend.snapshot()).config.groups[0].name).toBe("Portable group");
});

test("policy imports and editor saves share the real mock configuration", async ({
  page,
  backend,
}) => {
  await page.goto("/");
  await showPage(page, "Settings");
  const dialog = await upload(page, {
    settings: { keeneticPolicy: { enabled: true, policyId: "Policy1", mode: "exclude" } },
  });
  await dialog.getByRole("button", { name: "Import", exact: true }).click();
  await expect(dialog).toHaveCount(0);
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
});

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
  expect((await backend.snapshot()).config.profiles).toEqual([profile("Replacement VPN")]);
  await expect(page.getByLabel("Profile name", { exact: true })).toHaveValue("Replacement VPN");
  const repeat = await upload(page, { profiles: [profile("Replacement VPN")] });
  await repeat.getByRole("button", { name: "Import", exact: true }).click();
  await expect(page.getByText("No changes to import", { exact: true })).toBeVisible();
  await expect(repeat).toBeVisible();
  await repeat.getByRole("button", { name: "Cancel", exact: true }).click();
  const clear = await upload(page, { profiles: [] });
  await clear.getByRole("switch", { name: "Replace", exact: true }).click();
  await clear.getByRole("button", { name: "Import", exact: true }).click();
  await expect(clear).toHaveCount(0);
  await expect(page.getByLabel("Profile name", { exact: true })).toHaveCount(0);
  expect((await backend.snapshot()).config.profiles).toEqual([]);
  await page.reload();
  await expect(page.getByText("No profiles yet", { exact: true })).toBeVisible();
});

test("a double click applies a profile import only once", async ({ page, backend }) => {
  await page.goto("/");
  await showPage(page, "Settings");
  const dialog = await upload(page, { profiles: [profile()] });
  let writes = 0;
  page.on("request", (request) => {
    if (request.url().endsWith("/system/config/apply") && request.method() === "POST") writes++;
  });
  await dialog
    .getByRole("button", { name: "Import", exact: true })
    .evaluate((element: HTMLButtonElement) => {
      element.click();
      element.click();
    });
  await expect(dialog).toHaveCount(0);
  expect(writes).toBe(1);
  expect((await backend.snapshot()).config.profiles).toEqual([profile()]);
});

for (const payload of [
  "{broken",
  '{"schemaVersion":999,"profiles":[]}',
  '{"profiles":[{"id":1}]}',
]) {
  test(`invalid profile file is rejected without changing backend: ${payload}`, async ({
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
  await dialog.getByRole("button", { name: "Cancel", exact: true }).click();
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
        await dialog.getByRole("button", { name: "Keenetic policy mapping", exact: true }).click();
        await page.getByRole("option", { name: local.name, exact: true }).click();
        await expect(button).toBeEnabled();
        await button.click();
        await expect(dialog).toHaveCount(0);
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
        dialog.getByRole("button", { name: "Keenetic policy mapping", exact: true }),
      ).toHaveText("ForeignPolicy73");
      await expect(dialog.getByRole("button", { name: "Import", exact: true })).toBeEnabled();
      await dialog.getByRole("button", { name: "Import", exact: true }).click();
      await expect(dialog).toHaveCount(0);
      expect((await backend.snapshot()).config.app.keeneticPolicy).toEqual({
        enabled: true,
        policyId: "ForeignPolicy73",
        mode: "exclude",
      });
      const panel = page.locator(".policy-settings");
      await panel.getByRole("switch", { name: "Processing mode", exact: true }).click();
      await expect(page.locator("#save-settings")).toBeEnabled();
      await page.locator("#save-settings").click();
      await expect(page.locator("#save-settings")).toBeDisabled();
      await page.reload();
      await expect(panel.getByRole("button", { name: "Router policy", exact: true })).toContainText(
        "ForeignPolicy73",
      );
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
