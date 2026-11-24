import { expect, test, type Page } from "@playwright/test";

import {
  readAdaptiveSettings,
  type AdaptiveSettings,
} from "../../src/modules/settings/adaptive/data";
import { adaptiveStatus } from "../mocks/adaptive-settings";
import { generalSettings } from "../mocks/general-settings";

/** These fixtures exercise the editor contract. Native integration tests own
 * TPROXY interception, real interfaces and the connection retry policy. */
async function backend(page: Page) {
  const state = {
    snapshot: adaptiveStatus(),
    reads: 0,
    clears: 0,
    writes: [] as AdaptiveSettings[],
    failRead: false,
    putFailure: null as "activation" | "persistence" | null,
    putGate: undefined as Promise<void> | undefined,
  };
  await page.route("**/api/v1/**", async (route) => {
    const request = route.request();
    const path = new URL(request.url()).pathname.replace("/api/v1", "");
    if (request.method() === "OPTIONS") return route.fulfill({ status: 204 });
    if (path === "/adaptive") {
      if (request.method() === "GET") {
        state.reads++;
        if (state.failRead)
          return route.fulfill({ status: 404, json: { error: "Adaptive API unavailable" } });
        return route.fulfill({ json: state.snapshot });
      }
      expect(request.method()).toBe("PUT");
      const body: unknown = request.postDataJSON();
      if (!body || typeof body !== "object" || !("settings" in body))
        throw new Error("Missing adaptive settings");
      const settings = readAdaptiveSettings(body.settings);
      state.writes.push(settings);
      if (state.putGate) await state.putGate;
      if (state.putFailure === "activation")
        return route.fulfill({
          status: 503,
          json: {
            ...state.snapshot,
            code: "TPROXY_UNAVAILABLE",
            error: "TPROXY support is unavailable",
          },
        });
      state.snapshot = {
        ...state.snapshot,
        settings,
        active: settings.enabled,
        error: null,
      };
      if (state.putFailure === "persistence")
        return route.fulfill({
          status: 500,
          json: {
            ...state.snapshot,
            error: "Disk is full",
            code: "PERSISTENCE_FAILED",
            applied: true,
          },
        });
      return route.fulfill({ json: state.snapshot });
    }
    if (path === "/adaptive/cache") {
      expect(request.method()).toBe("DELETE");
      state.clears++;
      state.snapshot.entries = [];
      return route.fulfill({ json: state.snapshot });
    }
    const fixtures: Record<string, unknown> = {
      "/auth": { enabled: false },
      "/groups": { groups: [] },
      "/profiles": { profiles: [] },
      "/subscriptions": { subscriptions: [] },
      "/system/settings": {
        settings: generalSettings(),
        revision: "1".repeat(64),
        operation: null,
      },
      "/system/interfaces": { interfaces: state.snapshot.interfaces },
      "/system/update/status": { stage: "idle" },
      "/keenetic/policy": {
        settings: { enabled: false, policy_id: "", mode: "include" },
        runtime: { state: "disabled" },
      },
      "/keenetic/policies": { policies: [] },
    };
    return route.fulfill({ json: fixtures[path] ?? { version: "test" } });
  });
  return state;
}

async function showPage(page: Page, name: string) {
  const menu = page.locator(".mobile-dropdown-btn");
  if (await menu.isVisible()) await menu.click();
  await page.getByRole("tab", { name, exact: true }).click();
}

async function openAdaptive(page: Page) {
  await showPage(page, "Settings");
  await page.getByRole("tab", { name: "Adaptive routing", exact: true }).click();
}

async function choose(page: Page, label: string, id: string) {
  // The shared Select uses a native button to open its listbox.
  const trigger = page.getByRole("button", { name: label, exact: true });
  await expect(trigger).toHaveAttribute("aria-haspopup", "listbox");
  await expect(trigger).toHaveAttribute("aria-expanded", "false");
  await trigger.click();
  await expect(trigger).toHaveAttribute("aria-expanded", "true");
  await page
    .getByRole("listbox", { name: label, exact: true })
    .getByRole("option", { name: new RegExp(`^${id}\\b`) })
    .click();
  await expect(trigger).toHaveAttribute("aria-expanded", "false");
}

async function enable(page: Page) {
  await page.getByRole("switch", { name: "Enable adaptive routing", exact: true }).click();
  await choose(page, "Alternate interface", "wg0");
  await choose(page, "Add client LAN interface", "br-lan");
}

test("adaptive routing loads lazily, validates LAN scope and saves the retained draft once", async ({
  page,
}) => {
  const state = await backend(page);
  await page.goto("/");
  await showPage(page, "Settings");
  await expect(page.getByRole("tab", { name: "Profiles", exact: true })).toHaveAttribute(
    "aria-selected",
    "true",
  );
  expect(state.reads).toBe(0);
  await page.getByRole("tab", { name: "Adaptive routing", exact: true }).click();
  const scope = page.locator("#adaptive");
  await expect(scope.getByRole("switch")).toBeVisible();
  expect(state.reads).toBe(1);
  const save = page.locator("#save-settings");
  await expect(save).toBeDisabled();
  await page.getByRole("switch", { name: "Enable adaptive routing", exact: true }).click();
  await expect(scope.getByText("Choose an alternate interface.", { exact: true })).toBeVisible();
  await expect(
    scope.getByText("Choose between 1 and 16 client LAN interfaces.", { exact: true }),
  ).toBeVisible();
  await expect(save).toBeDisabled();
  await choose(page, "Alternate interface", "wg0");
  await choose(page, "Add client LAN interface", "br-lan");
  const timeout = page.getByRole("textbox", { name: "Connection timeout, ms", exact: true });
  await timeout.fill("99");
  await expect(timeout).toHaveAttribute("aria-invalid", "true");
  await expect(save).toBeDisabled();
  await timeout.fill("2500");
  await expect(save).toBeEnabled();
  await page.getByRole("tab", { name: "Profiles", exact: true }).click();
  await showPage(page, "Groups");
  await openAdaptive(page);
  await expect(timeout).toHaveValue("2500");
  await expect(scope.getByRole("switch")).toBeChecked();
  await expect(
    scope.getByRole("button", { name: "Remove client interface br-lan", exact: true }),
  ).toBeVisible();
  expect(state.reads).toBe(1);
  expect(state.writes).toEqual([]);

  let release!: () => void;
  state.putGate = new Promise<void>((resolve) => {
    release = resolve;
  });
  try {
    await save.click();
    await expect.poll(() => state.writes.length).toBe(1);
    await expect(scope.getByRole("switch")).toBeDisabled();
    await expect(scope.locator(".runtime-status")).toHaveText("Disabled");
    await expect(
      page.getByRole("dialog", { name: "Apply general settings?", exact: true }),
    ).toHaveCount(0);
    expect(state.writes[0]).toEqual({
      ...adaptiveStatus().settings,
      enabled: true,
      interface: "wg0",
      ingressInterfaces: ["br-lan"],
      connectTimeoutMs: 2500,
    });
  } finally {
    release();
  }
  await expect(scope.locator(".runtime-status")).toHaveText("Active");
  await expect(save).toBeDisabled();
  expect(state.reads).toBe(1);
});

test("an older backend can be retried without automatic adaptive reads", async ({ page }) => {
  const state = await backend(page);
  state.failRead = true;
  await page.goto("/");
  await openAdaptive(page);
  await expect(
    page.getByText("Failed to load adaptive routing settings", { exact: true }),
  ).toBeVisible();
  await page.getByRole("tab", { name: "Profiles", exact: true }).click();
  await page.getByRole("tab", { name: "Adaptive routing", exact: true }).click();
  expect(state.reads).toBe(1);
  state.failRead = false;
  await page.locator("#adaptive").getByRole("button", { name: "Retry", exact: true }).click();
  await expect(
    page.getByRole("switch", { name: "Enable adaptive routing", exact: true }),
  ).toBeVisible();
  expect(state.reads).toBe(2);
  expect(state.writes).toEqual([]);
});

test("activation failure preserves the draft and never claims success", async ({ page }) => {
  const state = await backend(page);
  state.putFailure = "activation";
  await page.goto("/");
  await openAdaptive(page);
  await enable(page);
  await page.locator("#save-settings").click();
  await expect(
    page.getByText("Request failed: TPROXY support is unavailable", { exact: true }),
  ).toBeVisible();
  await expect(page.locator("#adaptive .runtime-status")).toHaveText("Disabled");
  await expect(
    page.getByRole("switch", { name: "Enable adaptive routing", exact: true }),
  ).toBeChecked();
  await expect(page.locator("#save-settings")).toBeEnabled();
  await page.getByRole("button", { name: "Settings need attention", exact: true }).click();
  const feedback = page.getByRole("dialog", { name: "Settings need attention", exact: true });
  await expect(feedback).toContainText(
    "The settings could not be applied. Your draft is preserved.",
  );
  await expect(feedback.getByText("TPROXY support is unavailable", { exact: true })).toBeVisible();
  await page.keyboard.press("Escape");
  state.putFailure = null;
  await page.locator("#save-settings").click();
  await expect(page.locator("#adaptive .runtime-status")).toHaveText("Active");
  await expect(page.locator("#save-settings")).toBeDisabled();
  expect(state.writes.length).toBe(2);
});

test("an applied persistence failure exposes runtime state and keeps a disk-save retry", async ({
  page,
}) => {
  const state = await backend(page);
  state.putFailure = "persistence";
  await page.goto("/");
  await openAdaptive(page);
  await enable(page);
  await page.locator("#save-settings").click();
  await expect(page.locator("#adaptive .runtime-status")).toHaveText("Active");
  await expect(page.locator("#save-settings")).toBeEnabled();
  await page.getByRole("button", { name: "Settings need attention", exact: true }).click();
  await expect(
    page.getByRole("dialog", { name: "Settings need attention", exact: true }),
  ).toContainText("Changes are applied but not saved to disk. Retry saving.");
  await page.keyboard.press("Escape");
  await page.getByRole("tab", { name: "Profiles", exact: true }).click();
  await page.getByRole("tab", { name: "Adaptive routing", exact: true }).click();
  await expect(page.locator("#save-settings")).toBeEnabled();
  state.putFailure = null;
  await page.locator("#save-settings").click();
  await expect(page.locator("#save-settings")).toBeDisabled();
  await expect(
    page.getByRole("button", { name: "Settings need attention", exact: true }),
  ).toBeHidden();
  expect(state.writes.length).toBe(2);
  expect(state.writes[0]).toEqual(state.writes[1]);
});

test("saved settings can retry activation explicitly without unsaved changes", async ({ page }) => {
  const state = await backend(page);
  state.snapshot.settings = {
    ...state.snapshot.settings,
    enabled: true,
    interface: "wg0",
    ingressInterfaces: ["br-lan"],
  };
  state.snapshot.error = "TPROXY support is unavailable";
  await page.goto("/");
  await openAdaptive(page);
  await expect(page.locator("#adaptive .runtime-status")).toHaveText("Activation unavailable");
  await expect(page.locator("#save-settings")).toBeDisabled();
  await page.getByRole("button", { name: "Retry activation", exact: true }).click();
  await expect(page.locator("#adaptive .runtime-status")).toHaveText("Active");
  await expect(page.getByRole("button", { name: "Retry activation", exact: true })).toHaveCount(0);
  await expect(page.locator("#save-settings")).toBeDisabled();
  expect(state.writes).toEqual([state.snapshot.settings]);
});

test("manual status refresh and clearing learned routes preserve unsaved settings", async ({
  page,
}) => {
  const state = await backend(page);
  state.snapshot.settings = {
    ...state.snapshot.settings,
    enabled: true,
    interface: "wg0",
    ingressInterfaces: ["br-lan"],
  };
  state.snapshot.active = true;
  state.snapshot.entries = [
    { address: "203.0.113.8", family: 4, port: 443, alternate: true, expiresInSeconds: 540 },
    { address: "2001:db8::8", family: 6, port: 80, alternate: false, expiresInSeconds: 220 },
  ];
  await page.goto("/");
  await openAdaptive(page);
  const timeout = page.getByRole("textbox", { name: "Connection timeout, ms", exact: true });
  await timeout.fill("4000");
  await expect(page.getByRole("cell", { name: "[2001:db8::8]:80", exact: true })).toBeVisible();
  state.snapshot.entries[0].expiresInSeconds = 410;
  await page.getByRole("button", { name: "Refresh adaptive routing", exact: true }).click();
  await expect(page.getByRole("cell", { name: "410 sec", exact: true })).toBeVisible();
  await expect(timeout).toHaveValue("4000");
  await page.getByRole("button", { name: "Clear learned routes", exact: true }).click();
  await expect(page.getByText("No learned routes yet", { exact: true })).toBeVisible();
  await expect(timeout).toHaveValue("4000");
  await expect(page.locator("#save-settings")).toBeEnabled();
  expect(state.reads).toBe(2);
  expect(state.clears).toBe(1);
  expect(state.writes).toEqual([]);

  state.failRead = true;
  await page.getByRole("button", { name: "Refresh adaptive routing", exact: true }).click();
  await expect(page.locator("#adaptive .runtime-status")).toHaveText("Status unavailable");
  await expect(page.locator("#save-settings")).toBeDisabled();
  state.failRead = false;
  await page.getByRole("button", { name: "Refresh adaptive routing", exact: true }).click();
  await expect(page.locator("#adaptive .runtime-status")).toHaveText("Active");
  await expect(page.locator("#save-settings")).toBeEnabled();
  await expect(timeout).toHaveValue("4000");
});

for (const width of [1280, 390]) {
  test(`adaptive routing stays usable with unavailable interfaces (${width}px, Russian)`, async ({
    page,
  }, testInfo) => {
    await page.setViewportSize({ width, height: width === 1280 ? 900 : 844 });
    await page.addInitScript(() => localStorage.setItem("locale", JSON.stringify({ value: "ru" })));
    const state = await backend(page);
    state.snapshot.settings = {
      ...state.snapshot.settings,
      enabled: true,
      interface: "tun-missing",
      ingressInterfaces: ["br-lan", "br-old"],
    };
    state.snapshot.error = "Interface tun-missing is unavailable";
    state.snapshot.entries = Array.from({ length: 54 }, (_, i) => ({
      address: `2001:db8:abcd:abcd:abcd:abcd:abcd:${(i + 1).toString(16)}`,
      family: 6,
      port: 443,
      alternate: true,
      expiresInSeconds: 300 + i,
    }));
    await page.goto("/");
    await showPage(page, "Настройки");
    await page.getByRole("tab", { name: "Автомаршрутизация", exact: true }).click();
    await expect(
      page.getByRole("button", { name: "Резервный интерфейс", exact: true }),
    ).toContainText("tun-missing");
    await expect(
      page.getByRole("button", { name: "Удалить локальный интерфейс br-old", exact: true }),
    ).toBeVisible();
    await expect(page.locator("#adaptive .runtime-status")).toHaveText("Не удалось активировать");
    await expect(page.locator("#adaptive tbody tr")).toHaveCount(50);
    await expect
      .poll(() => page.evaluate(() => document.documentElement.scrollWidth))
      .toBeLessThanOrEqual(width);
    await page.screenshot({
      path: testInfo.outputPath(`adaptive-${width}-ru.png`),
      fullPage: true,
    });
    await choose(page, "Резервный интерфейс", "wg0");
    await page
      .getByRole("button", { name: "Удалить локальный интерфейс br-old", exact: true })
      .click();
    await expect(page.locator("#save-settings")).toBeEnabled();
    expect(state.writes).toEqual([]);
  });
}
