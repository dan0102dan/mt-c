import { expect, test, type Page } from "@playwright/test";

import type { MockGeneralSettings } from "../../dev/settings-mock.ts";
import type { Profile } from "../../src/types";
import { generalServerConfig, generalSettings } from "../mocks/general-settings";

type Operation = {
  id: string;
  state: "applying" | "succeeded" | "failed" | "rollback_failed";
  target_revision: string;
  error?: string;
};
type SettingsWrite = { settings: MockGeneralSettings; revision: string };
type PutFailure = "validation" | "server" | "lost-ack" | "malformed";

function object(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

/** Model confirmed settings and asynchronous application independently of the
 * browser. Native lifecycle tests own actual listeners, routing and disk writes. */
async function backend(page: Page) {
  const config = generalServerConfig();
  const state = {
    settings: generalSettings(),
    version: 1,
    operation: null as Operation | null,
    pending: null as { settings: MockGeneralSettings; version: number } | null,
    holdApplying: false,
    applyFailure: null as "failed" | "rollback_failed" | null,
    putFailure: null as PutFailure | null,
    readFailure: null as "network" | "malformed" | null,
    checkOperationId: null as string | null,
    checkReads: 0,
    writes: [] as SettingsWrite[],
    actions: [] as string[],
    requests: [] as { path: string; url: string; method: string; authorization?: string }[],
    structuredWrites: [] as string[],
    profileFailure: null as "server" | "persistence" | null,
    groups: config.groups,
    profiles: config.profiles as Profile[],
    subscriptions: config.subscriptions.map(({ last_update, ...sub }) => ({
      ...sub,
      lastUpdate: last_update,
    })),
    acceptedToken: "initial-token",
    logins: 0,
  };
  const revision = (version = state.version) => version.toString(16).padStart(64, "0");
  const complete = () => {
    if (!state.pending || !state.operation) return;
    if (state.applyFailure) {
      state.operation = {
        ...state.operation,
        state: state.applyFailure,
        error: "Could not start the requested DNS listener",
      };
    } else {
      state.settings = structuredClone(state.pending.settings);
      state.version = state.pending.version;
      state.operation = { ...state.operation, state: "succeeded" };
    }
    state.pending = null;
  };
  const snapshot = () => ({
    settings: structuredClone(state.settings),
    revision: revision(),
    operation: state.operation && {
      ...state.operation,
      ...(state.checkOperationId ? { id: state.checkOperationId } : {}),
    },
  });

  await page.route("**/api/v1/**", async (route) => {
    const request = route.request();
    const path = new URL(request.url()).pathname.replace("/api/v1", "");
    const authorization = request.headers().authorization;
    state.requests.push({ path, url: request.url(), method: request.method(), authorization });
    if (request.method() === "OPTIONS") return route.fulfill({ status: 204 });
    if (path === "/auth") {
      if (request.method() === "POST") {
        state.logins++;
        return route.fulfill({ json: { token: state.acceptedToken } });
      }
      return route.fulfill({ json: { enabled: state.settings.httpWeb.auth.enabled } });
    }
    if (state.settings.httpWeb.auth.enabled && authorization !== `Bearer ${state.acceptedToken}`)
      return route.fulfill({ status: 401, json: { error: "Unauthorized" } });
    if (path === "/system/interfaces")
      return route.fulfill({
        json: { interfaces: ["blackhole", "br0", "tun0", "tun1"].map((id) => ({ id, name: "" })) },
      });
    if (path === "/system/settings") {
      if (request.method() === "GET") {
        if (state.operation) state.checkReads++;
        if (state.readFailure === "network") return route.abort("failed");
        if (state.readFailure === "malformed")
          return route.fulfill({ json: { settings: null, revision: 5 } });
        if (!state.holdApplying) complete();
        return route.fulfill({ json: snapshot() });
      }
      const body: unknown = request.postDataJSON();
      if (!object(body) || !object(body.settings) || typeof body.revision !== "string")
        throw new Error("Invalid General settings request");
      state.actions.push("settings");
      state.writes.push(structuredClone(body) as SettingsWrite);
      if (body.revision !== revision())
        return route.fulfill({
          status: 409,
          json: {
            code: "SETTINGS_CONFLICT",
            error: "General settings changed; reload before saving",
          },
        });
      if (state.putFailure === "validation")
        return route.fulfill({
          status: 400,
          json: {
            code: "SETTINGS_INVALID",
            field: "dnsProxy.timeoutMs",
            error: "Timeout rejected by server",
          },
        });
      if (state.putFailure === "server")
        return route.fulfill({
          status: 503,
          json: { code: "SETTINGS_UNAVAILABLE", error: "Settings application is unavailable" },
        });
      const settings = structuredClone(body.settings) as MockGeneralSettings;
      const unchanged = JSON.stringify(settings) === JSON.stringify(state.settings);
      const targetVersion = unchanged ? state.version : state.version + 1;
      state.operation = {
        id: `operation-${state.writes.length}`,
        state: unchanged ? "succeeded" : "applying",
        target_revision: revision(targetVersion),
      };
      state.pending = unchanged ? null : { settings, version: targetVersion };
      if (state.putFailure === "lost-ack") return route.abort("failed");
      if (state.putFailure === "malformed")
        return route.fulfill({ status: 202, json: { operation: { id: 7, state: "applying" } } });
      return route.fulfill({
        status: unchanged ? 200 : 202,
        json: {
          operation: state.operation,
          target_revision: revision(targetVersion),
          next_web: {
            enabled: settings.httpWeb.enabled,
            ...settings.httpWeb.host,
            auth_enabled: settings.httpWeb.auth.enabled,
          },
        },
      });
    }
    if (request.method() === "PUT") state.structuredWrites.push(path);
    if (path === "/profiles") {
      if (request.method() === "PUT") {
        state.actions.push("profiles");
        const body: unknown = request.postDataJSON();
        if (!object(body) || !Array.isArray(body.profiles))
          throw new Error("Invalid profiles request");
        if (state.profileFailure === "server")
          return route.fulfill({ status: 500, json: { error: "Cannot save profiles" } });
        state.profiles = structuredClone(body.profiles) as Profile[];
        if (state.profileFailure === "persistence")
          return route.fulfill({
            status: 500,
            json: {
              code: "PERSISTENCE_FAILED",
              applied: true,
              error: "Disk is full",
              profiles: state.profiles,
            },
          });
      }
      return route.fulfill({
        json: {
          profiles: state.profiles.map((profile) => ({
            ...profile,
            usage: { groups: 0, subscriptions: 0 },
          })),
        },
      });
    }
    if (path === "/groups") return route.fulfill({ json: { groups: state.groups } });
    if (path === "/subscriptions")
      return route.fulfill({ json: { subscriptions: state.subscriptions } });
    if (path === "/system/update/status") return route.fulfill({ json: { stage: "idle" } });
    if (path === "/keenetic/policy")
      return route.fulfill({
        json: {
          settings: { enabled: false, policy_id: "", mode: "include" },
          runtime: { state: "disabled" },
        },
      });
    if (path === "/keenetic/policies") return route.fulfill({ json: { policies: [] } });
    return route.fulfill({ json: { version: "test" } });
  });
  return { state, revision, complete };
}

async function showPage(page: Page, name: "Groups" | "Subscriptions" | "Settings") {
  const menu = page.getByRole("button", { name: "Open menu", exact: true });
  if (await menu.isVisible()) await menu.click();
  await page.getByRole("tab", { name, exact: true }).click();
}

async function openGeneral(page: Page) {
  await showPage(page, "Settings");
  await page.getByRole("tab", { name: "General", exact: true }).click();
  await expect(
    page.getByRole("textbox", { name: "Upstream DNS address", exact: true }),
  ).toBeVisible();
  const timeout = page.getByRole("spinbutton", { name: "DNS response timeout (ms)", exact: true });
  if (!(await timeout.isVisible()))
    await page.getByText("DNS limits and timeout", { exact: true }).click();
  await expect(timeout).toBeVisible();
  return timeout;
}

async function confirmSave(page: Page) {
  await page.locator("#save-settings").click();
  const dialog = page.getByRole("dialog", { name: "Apply general settings?", exact: true });
  await expect(dialog).toBeVisible();
  await dialog.getByRole("button", { name: "Apply", exact: true }).click();
}

async function applied(page: Page) {
  await expect(page.getByText("Settings applied", { exact: true }).first()).toBeVisible();
}

async function hasUnloadWarning(page: Page) {
  return page.evaluate(() => {
    const event = new Event("beforeunload", { cancelable: true });
    window.dispatchEvent(event);
    return event.defaultPrevented;
  });
}

test("General edits typed settings, saves and reloads without exposing YAML or internal fields", async ({
  page,
}) => {
  const { state, revision } = await backend(page);
  await page.goto("/");
  const timeout = await openGeneral(page);
  await expect(timeout).toHaveValue("5000");
  await expect(page.getByRole("tab", { name: "YAML", exact: true })).toHaveCount(0);
  await expect(page.getByRole("textbox", { name: "YAML configuration", exact: true })).toHaveCount(
    0,
  );
  await expect(page.getByLabel(/chain prefix|table prefix|skin|mark table/i)).toHaveCount(0);
  const baseline = revision();
  await timeout.fill("7500");
  await page.getByRole("textbox", { name: "Upstream DNS address", exact: true }).fill("8.8.8.8");
  await page
    .getByRole("spinbutton", { name: "Additional IP retention (seconds)", exact: true })
    .fill("0");
  await page
    .getByRole("combobox", { name: "Interfaces for DNS interception", exact: true })
    .fill("br0, tun1");
  await page.locator('[data-select-trigger][aria-label="Log level"]').click();
  await page.getByRole("option", { name: "Debug", exact: true }).click();
  const expected = generalSettings();
  expected.dnsProxy.timeoutMs = 7500;
  expected.dnsProxy.upstream.address = "8.8.8.8";
  expected.netfilter.ipset.additionalTTLSeconds = 0;
  expected.link = ["br0", "tun1"];
  expected.logLevel = "debug";
  await confirmSave(page);
  await applied(page);
  expect(state.settings).toEqual(expected);
  expect(state.writes).toEqual([{ settings: expected, revision: baseline }]);
  expect(state.structuredWrites).toEqual([]);
  expect(state.requests.some(({ path }) => path === "/system/config/yaml")).toBe(false);
  expect(await hasUnloadWarning(page)).toBe(false);
  await page.reload();
  await openGeneral(page);
  await expect(timeout).toHaveValue("7500");
  await expect(
    page.getByRole("textbox", { name: "Upstream DNS address", exact: true }),
  ).toHaveValue("8.8.8.8");
});

test("cancelling the DNS interruption warning starts no General or profile writes", async ({
  page,
}) => {
  const { state } = await backend(page);
  await page.goto("/");
  const timeout = await openGeneral(page);
  await timeout.fill("7500");
  await page.getByRole("tab", { name: "Profiles", exact: true }).click();
  await page
    .getByRole("textbox", { name: "Profile name", exact: true })
    .first()
    .fill("Pending profile");
  await page.getByRole("tab", { name: "General", exact: true }).click();
  await page.locator("#save-settings").click();
  const dialog = page.getByRole("dialog", { name: "Apply general settings?", exact: true });
  await expect(dialog).toContainText(/DNS/i);
  await expect(dialog).toContainText(/temporarily unavailable/i);
  await dialog.getByRole("button", { name: "Cancel", exact: true }).click();
  await expect(timeout).toHaveValue("7500");
  expect(state.actions).toEqual([]);
  expect(await hasUnloadWarning(page)).toBe(true);
  await page.getByRole("tab", { name: "Profiles", exact: true }).click();
  await expect(
    page.getByRole("textbox", { name: "Profile name", exact: true }).first(),
  ).toHaveValue("Pending profile");
});

test("Save applies General only after other changed Settings sections have succeeded", async ({
  page,
}) => {
  const { state } = await backend(page);
  await page.goto("/");
  const timeout = await openGeneral(page);
  await timeout.fill("7500");
  await page.getByRole("tab", { name: "Profiles", exact: true }).click();
  await page
    .getByRole("textbox", { name: "Profile name", exact: true })
    .first()
    .fill("Saved before restart");
  await confirmSave(page);
  await page.getByRole("tab", { name: "General", exact: true }).click();
  await applied(page);
  expect(state.actions).toEqual(["profiles", "settings"]);
  expect(state.profiles[0].name).toBe("Saved before restart");
  expect(state.settings.dnsProxy.timeoutMs).toBe(7500);
  expect(await hasUnloadWarning(page)).toBe(false);
});

for (const failure of ["server", "persistence"] as const) {
  test(`${failure} failure in Profiles prevents General apply and keeps both drafts retryable`, async ({
    page,
  }) => {
    const { state } = await backend(page);
    state.profileFailure = failure;
    await page.goto("/");
    const timeout = await openGeneral(page);
    await timeout.fill("7500");
    await page.getByRole("tab", { name: "Profiles", exact: true }).click();
    const profileName = page.getByRole("textbox", { name: "Profile name", exact: true }).first();
    await profileName.fill("Profile draft");
    await confirmSave(page);
    await expect.poll(() => state.actions).toEqual(["profiles"]);
    await expect(page.locator("#save-settings")).toBeEnabled();
    await expect(profileName).toHaveValue("Profile draft");
    await page.getByRole("tab", { name: "General", exact: true }).click();
    await expect(timeout).toHaveValue("7500");
    expect(state.writes).toEqual([]);
    expect(state.settings.dnsProxy.timeoutMs).toBe(5000);
    expect(await hasUnloadWarning(page)).toBe(true);
    state.profileFailure = null;
    await confirmSave(page);
    await applied(page);
    expect(state.actions).toEqual(["profiles", "profiles", "settings"]);
    expect(state.settings.dnsProxy.timeoutMs).toBe(7500);
  });
}

test("a 202 response and unrelated operation never produce a premature success", async ({
  page,
}) => {
  const { state, complete } = await backend(page);
  state.holdApplying = true;
  await page.goto("/");
  const timeout = await openGeneral(page);
  await timeout.fill("7500");
  await confirmSave(page);
  await expect.poll(() => state.checkReads).toBeGreaterThan(0);
  await expect(page.getByText("Settings applied", { exact: true })).toHaveCount(0);
  await expect(page.getByText("Settings saved", { exact: true })).toHaveCount(0);
  expect(state.settings.dnsProxy.timeoutMs).toBe(5000);
  expect(state.writes).toHaveLength(1);
  state.checkOperationId = "another-operation";
  complete();
  const checks = state.checkReads;
  await expect.poll(() => state.checkReads).toBeGreaterThan(checks);
  await expect(page.getByText("Settings applied", { exact: true })).toHaveCount(0);
  state.checkOperationId = null;
  const check = page.getByRole("button", { name: "Check application", exact: true });
  if (await check.isVisible()) await check.click();
  await applied(page);
  expect(state.writes).toHaveLength(1);
});

test("a stale settings revision preserves the General draft and never starts an operation", async ({
  page,
}) => {
  const { state, revision } = await backend(page);
  await page.goto("/");
  const timeout = await openGeneral(page);
  const baseline = revision();
  await timeout.fill("7500");
  state.settings.dnsProxy.timeoutMs = 9000;
  state.version++;
  await confirmSave(page);
  await expect(
    page.getByText("General settings changed; reload before saving", { exact: false }).first(),
  ).toBeVisible();
  await expect(timeout).toHaveValue("7500");
  expect(state.writes[0].revision).toBe(baseline);
  expect(state.operation).toBeNull();
  expect(state.settings.dnsProxy.timeoutMs).toBe(9000);
  expect(await hasUnloadWarning(page)).toBe(true);
});

for (const failure of ["validation", "server"] as const) {
  test(`${failure} rejection retains the form and permits an explicit corrected save`, async ({
    page,
  }) => {
    const { state } = await backend(page);
    state.putFailure = failure;
    await page.goto("/");
    const timeout = await openGeneral(page);
    await timeout.fill("7500");
    await confirmSave(page);
    await expect(
      page
        .getByText(
          failure === "validation"
            ? "Timeout rejected by server"
            : "Settings application is unavailable",
          { exact: false },
        )
        .first(),
    ).toBeVisible();
    await expect(timeout).toHaveValue("7500");
    expect(state.settings.dnsProxy.timeoutMs).toBe(5000);
    expect(state.operation).toBeNull();
    expect(await hasUnloadWarning(page)).toBe(true);
    state.putFailure = null;
    await timeout.fill("8000");
    await confirmSave(page);
    await applied(page);
    expect(state.settings.dnsProxy.timeoutMs).toBe(8000);
    expect(state.writes).toHaveLength(2);
  });
}

for (const failure of ["lost-ack", "malformed"] as const) {
  test(`${failure} acknowledgement keeps the draft and checking does not repeat PUT`, async ({
    page,
  }) => {
    await page.clock.install();
    const { state, complete } = await backend(page);
    state.putFailure = failure;
    state.holdApplying = true;
    await page.goto("/");
    const timeout = await openGeneral(page);
    await timeout.fill("7500");
    await confirmSave(page);
    await expect.poll(() => state.checkReads).toBeGreaterThan(0);
    // Exhaust the bounded reconciliation window without waiting for real time.
    await page.clock.fastForward(30000);
    await expect(
      page.getByRole("button", { name: "Check application", exact: true }),
    ).toBeVisible();
    await expect(
      page.getByRole("button", { name: "Check application", exact: true }),
    ).toBeEnabled();
    await expect(timeout).toHaveValue("7500");
    expect(state.writes).toHaveLength(1);
    expect(await hasUnloadWarning(page)).toBe(true);
    complete();
    await page.getByRole("button", { name: "Check application", exact: true }).click();
    await applied(page);
    expect(state.settings.dnsProxy.timeoutMs).toBe(7500);
    expect(state.writes).toHaveLength(1);
  });
}

for (const failure of ["failed", "rollback_failed"] as const) {
  test(`${failure} application outcome is visible and preserves the requested values`, async ({
    page,
  }) => {
    const { state } = await backend(page);
    state.applyFailure = failure;
    await page.goto("/");
    const timeout = await openGeneral(page);
    await timeout.fill("7500");
    await confirmSave(page);
    await expect(
      page.getByText("Could not start the requested DNS listener", { exact: false }).first(),
    ).toBeVisible();
    await expect(timeout).toHaveValue("7500");
    await expect(page.getByText("Settings applied", { exact: true })).toHaveCount(0);
    expect(state.settings.dnsProxy.timeoutMs).toBe(5000);
    expect(await hasUnloadWarning(page)).toBe(true);
    if (failure === "failed") {
      state.applyFailure = null;
      await confirmSave(page);
      await applied(page);
      expect(state.settings.dnsProxy.timeoutMs).toBe(7500);
    }
  });
}

test("General drafts survive tab changes and saving retains unrelated group and subscription drafts", async ({
  page,
}) => {
  const { state } = await backend(page);
  await page.goto("/");
  const timeout = await openGeneral(page);
  await timeout.fill("7500");
  await showPage(page, "Groups");
  await page.locator(".group-name").first().fill("Unsaved group");
  await showPage(page, "Subscriptions");
  await page.locator(".subscription-name").first().fill("Unsaved subscription");
  await openGeneral(page);
  await expect(timeout).toHaveValue("7500");
  await confirmSave(page);
  await applied(page);
  await showPage(page, "Groups");
  await expect(page.locator(".group-name").first()).toHaveValue("Unsaved group");
  await expect(page.locator("#save-changes")).toBeEnabled();
  await showPage(page, "Subscriptions");
  await expect(page.locator(".subscription-name").first()).toHaveValue("Unsaved subscription");
  await expect(page.locator("#save-subscriptions")).toBeEnabled();
  expect(state.structuredWrites).toEqual([]);
  expect(state.groups[0].name).toBe("Routing group");
  expect(state.subscriptions[0].name).toBe("Domain subscription");
  expect(await hasUnloadWarning(page)).toBe(true);
});

test("edits made in another section during General application survive completion", async ({
  page,
}) => {
  const { state, complete } = await backend(page);
  state.holdApplying = true;
  await page.goto("/");
  const timeout = await openGeneral(page);
  await timeout.fill("7500");
  await confirmSave(page);
  await expect.poll(() => state.checkReads).toBeGreaterThan(0);
  await showPage(page, "Groups");
  const name = page.locator(".group-name").first();
  await name.fill("Typed during application");
  complete();
  await openGeneral(page);
  await applied(page);
  await showPage(page, "Groups");
  await expect(name).toHaveValue("Typed during application");
  await expect(page.locator("#save-changes")).toBeEnabled();
  expect(state.structuredWrites).toEqual([]);
  expect(state.writes).toHaveLength(1);
});

for (const shortcut of ["Control+s", "Meta+s"]) {
  test(`${shortcut} in General cannot save hidden groups or subscriptions`, async ({ page }) => {
    const { state } = await backend(page);
    await page.goto("/");
    await showPage(page, "Groups");
    await page.locator(".group-name").first().fill("Group draft");
    await showPage(page, "Subscriptions");
    await page.locator(".subscription-name").first().fill("Subscription draft");
    const timeout = await openGeneral(page);
    await timeout.fill("7500");
    await timeout.press(shortcut);
    const confirmation = page.getByRole("dialog", { name: "Apply general settings?", exact: true });
    await expect(confirmation).toBeVisible();
    await confirmation.getByRole("button", { name: "Cancel", exact: true }).click();
    await showPage(page, "Groups");
    await expect(page.locator("#save-changes")).toBeEnabled();
    await expect(page.locator(".group-name").first()).toHaveValue("Group draft");
    await showPage(page, "Subscriptions");
    await expect(page.locator("#save-subscriptions")).toBeEnabled();
    await expect(page.locator(".subscription-name").first()).toHaveValue("Subscription draft");
    expect(state.structuredWrites).toEqual([]);
    expect(state.writes).toEqual([]);
  });
}

test("a changed web port offers a clean destination without sending credentials to it", async ({
  page,
}) => {
  await page.addInitScript(() =>
    localStorage.setItem("auth", JSON.stringify({ value: "initial-token" })),
  );
  const { state } = await backend(page);
  state.settings.httpWeb.auth.enabled = true;
  await page.goto("/");
  await openGeneral(page);
  const originalUrl = page.url();
  await page.getByRole("spinbutton", { name: "Web port", exact: true }).fill("8081");
  await confirmSave(page);
  const destination = page.getByRole("link", { name: /^Open new web address/ });
  await expect(destination).toHaveAttribute("href", /:8081\/$/);
  const href = await destination.getAttribute("href");
  expect(href).not.toContain("initial-token");
  expect(href).not.toContain("?");
  expect(page.url()).toBe(originalUrl);
  expect(state.requests.filter(({ url }) => new URL(url).port === "8081")).toEqual([]);
  expect(state.writes[0].settings.httpWeb.host.port).toBe(8081);
});

test("disabling the web interface explains the expected loss of access", async ({ page }) => {
  const { state } = await backend(page);
  await page.goto("/");
  await openGeneral(page);
  await page.getByRole("switch", { name: "Enable web interface", exact: true }).click();
  await confirmSave(page);
  await expect(
    page.getByText(
      "If web access is disabled, this page cannot confirm the result. Your draft is preserved.",
      { exact: true },
    ),
  ).toBeVisible();
  expect(state.writes).toHaveLength(1);
  expect(state.writes[0].settings.httpWeb.enabled).toBe(false);
  await expect(page.getByRole("link", { name: /^Open new web address/ })).toHaveCount(0);
});

test("enabling sign-in preserves the accepted General save through authentication and verification", async ({
  page,
}) => {
  const { state, complete } = await backend(page);
  state.holdApplying = true;
  await page.goto("/");
  const timeout = await openGeneral(page);
  await timeout.fill("7500");
  await page.getByRole("switch", { name: "Require sign-in", exact: true }).click();
  await confirmSave(page);
  await expect.poll(() => state.checkReads).toBeGreaterThan(0);
  expect(state.writes).toHaveLength(1);
  expect(state.writes[0].settings.httpWeb.auth.enabled).toBe(true);
  const operationId = state.operation?.id;
  const unauthorized = page.waitForResponse(
    (response) =>
      new URL(response.url()).pathname.endsWith("/system/settings") && response.status() === 401,
  );
  complete();
  await unauthorized;
  const login = page.getByRole("dialog", { name: "Sign In", exact: true });
  await expect(login).toBeVisible();
  expect(await hasUnloadWarning(page)).toBe(true);
  await login.locator("#login").fill("root");
  await login.locator("#login").press("Control+s");
  expect(state.writes).toHaveLength(1);
  expect(state.structuredWrites).toEqual([]);
  await login.locator("#password").fill("correct-password");
  await login.getByRole("button", { name: "Sign In", exact: true }).click();
  await expect(login).toHaveCount(0);
  await expect(timeout).toHaveValue("7500");
  await expect(page.getByRole("switch", { name: "Require sign-in", exact: true })).toBeChecked();
  await expect(page.getByText("Settings applied", { exact: true })).toHaveCount(0);
  const check = page.getByRole("button", { name: "Check application", exact: true });
  await expect(check).toBeEnabled();
  await check.click();
  await applied(page);
  expect(state.logins).toBe(1);
  expect(state.operation?.id).toBe(operationId);
  expect(state.writes).toHaveLength(1);
  expect(
    state.requests.filter(({ path, method }) => path === "/system/settings" && method === "PUT")[0]
      .authorization,
  ).toBeUndefined();
  expect(
    state.requests.some(
      ({ path, method, authorization }) =>
        path === "/system/settings" && method === "GET" && authorization === "Bearer initial-token",
    ),
  ).toBe(true);
  expect(await hasUnloadWarning(page)).toBe(false);
});

test("expired authentication preserves General, group and subscription drafts through login", async ({
  page,
}) => {
  await page.addInitScript(() =>
    localStorage.setItem("auth", JSON.stringify({ value: "initial-token" })),
  );
  const { state } = await backend(page);
  state.settings.httpWeb.auth.enabled = true;
  await page.goto("/");
  const timeout = await openGeneral(page);
  await timeout.fill("7500");
  await showPage(page, "Groups");
  await page.locator(".group-name").first().fill("Group survives login");
  await showPage(page, "Subscriptions");
  await page.locator(".subscription-name").first().fill("Subscription survives login");
  state.acceptedToken = "renewed-token";
  await showPage(page, "Settings");
  const login = page.getByRole("dialog", { name: "Sign In", exact: true });
  await expect(login).toBeVisible();
  await login.locator("#login").fill("root");
  await login.locator("#login").press("Control+s");
  expect(state.structuredWrites).toEqual([]);
  expect(state.writes).toEqual([]);
  await login.locator("#password").fill("correct-password");
  await login.getByRole("button", { name: "Sign In", exact: true }).click();
  await expect(login).toHaveCount(0);
  expect(state.logins).toBe(1);
  await openGeneral(page);
  await expect(timeout).toHaveValue("7500");
  await showPage(page, "Groups");
  await expect(page.locator(".group-name").first()).toHaveValue("Group survives login");
  await expect(page.locator("#save-changes")).toBeEnabled();
  await showPage(page, "Subscriptions");
  await expect(page.locator(".subscription-name").first()).toHaveValue(
    "Subscription survives login",
  );
  await expect(page.locator("#save-subscriptions")).toBeEnabled();
  expect(state.structuredWrites).toEqual([]);
  expect(await hasUnloadWarning(page)).toBe(true);
});

test("invalid numeric and interface fields block Save until corrected", async ({ page }) => {
  const { state } = await backend(page);
  await page.goto("/");
  const timeout = await openGeneral(page);
  for (const invalid of ["", "0", "1.5", "4294967296"]) {
    await timeout.fill(invalid);
    await expect(page.locator("#save-settings")).toBeDisabled();
    expect(state.writes).toEqual([]);
  }
  await timeout.fill("7500");
  const port = page.getByRole("spinbutton", { name: "Web port", exact: true });
  await port.fill("65536");
  await expect(page.locator("#save-settings")).toBeDisabled();
  await port.fill("8080");
  const links = page.getByRole("combobox", {
    name: "Interfaces for DNS interception",
    exact: true,
  });
  await links.fill("br0, br0");
  await expect(page.locator("#save-settings")).toBeDisabled();
  await links.fill("br0");
  await expect(page.locator("#save-settings")).toBeEnabled();
  expect(state.writes).toEqual([]);
});

test("a failed application check preserves the pending save and retry only reads its result", async ({
  page,
}) => {
  await page.clock.install();
  const { state, complete } = await backend(page);
  state.holdApplying = true;
  await page.goto("/");
  const timeout = await openGeneral(page);
  await timeout.fill("7500");
  state.readFailure = "network";
  await confirmSave(page);
  await expect.poll(() => state.checkReads).toBeGreaterThan(0);
  await page.clock.fastForward(30000);
  const check = page.getByRole("button", { name: "Check application", exact: true });
  await expect(check).toBeEnabled();
  await expect(
    page.getByText(
      "Could not confirm whether the settings were applied. Your draft is preserved.",
      { exact: true },
    ),
  ).toBeVisible();
  await expect(timeout).toHaveValue("7500");
  expect(state.writes).toHaveLength(1);
  expect(await hasUnloadWarning(page)).toBe(true);
  state.readFailure = null;
  complete();
  await check.click();
  await applied(page);
  expect(state.writes).toHaveLength(1);
});

test("web access changes cannot discard unrelated drafts through navigation", async ({ page }) => {
  const { state } = await backend(page);
  await page.goto("/");
  await openGeneral(page);
  await page.getByRole("spinbutton", { name: "Web port", exact: true }).fill("8081");
  await showPage(page, "Groups");
  await page.locator(".group-name").first().fill("Keep this draft");
  await openGeneral(page);
  await expect(page.locator("#save-settings")).toBeDisabled();
  await expect(
    page.getByText("Save changes in Groups and Subscriptions before changing web access.", {
      exact: true,
    }),
  ).toBeVisible();
  await expect(page.getByRole("spinbutton", { name: "Web port", exact: true })).toHaveValue("8081");
  expect(state.writes).toEqual([]);
  expect(state.structuredWrites).toEqual([]);
  expect(await hasUnloadWarning(page)).toBe(true);
});

for (const russian of [false, true]) {
  test(`General controls and confirmation remain accessible on a narrow ${russian ? "Russian" : "English"} page`, async ({
    page,
  }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    if (russian)
      await page.addInitScript(() =>
        localStorage.setItem("locale", JSON.stringify({ value: "ru" })),
      );
    const { state } = await backend(page);
    await page.goto("/");
    await page.getByRole("button", { name: "Open menu", exact: true }).click();
    await page.getByRole("tab", { name: russian ? "Настройки" : "Settings", exact: true }).click();
    const tab = page.getByRole("tab", { name: russian ? "Общие" : "General", exact: true });
    await tab.click();
    await page
      .getByText(russian ? "Ограничения DNS и время ожидания" : "DNS limits and timeout", {
        exact: true,
      })
      .click();
    const timeout = page.getByRole("spinbutton", {
      name: russian ? "Время ожидания DNS-ответа (мс)" : "DNS response timeout (ms)",
      exact: true,
    });
    await timeout.fill("7500");
    const save = page.getByRole("button", {
      name: russian ? "Сохранить изменения" : "Save Changes",
      exact: true,
    });
    for (const control of [tab, save]) {
      await expect(control).toBeInViewport();
      const box = await control.boundingBox();
      expect(box).not.toBeNull();
      expect(box!.x).toBeGreaterThanOrEqual(0);
      expect(box!.x + box!.width).toBeLessThanOrEqual(390);
    }
    expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBeLessThanOrEqual(
      390,
    );
    await save.click();
    const dialog = page.getByRole("dialog", {
      name: russian ? "Применить общие настройки?" : "Apply general settings?",
      exact: true,
    });
    await expect(dialog).toBeVisible();
    const cancel = dialog.getByRole("button", { name: russian ? "Отмена" : "Cancel", exact: true });
    await expect(cancel).toBeInViewport();
    await expect(
      dialog.getByRole("button", { name: russian ? "Применить" : "Apply", exact: true }),
    ).toBeInViewport();
    await cancel.click();
    await expect(timeout).toHaveValue("7500");
    expect(state.writes).toEqual([]);
  });
}
