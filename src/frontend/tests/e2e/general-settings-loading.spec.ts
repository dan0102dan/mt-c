import { expect, test, type Page } from "@playwright/test";

import { generalSettings } from "../mocks/general-settings";

type Operation = {
  id: string;
  state: "applying" | "succeeded";
  target_revision: string;
};

async function backend(page: Page) {
  const state = {
    settings: generalSettings(),
    revision: "1".repeat(64),
    operation: null as Operation | null,
    reads: 0,
    writes: 0,
    failRead: false,
    readGate: undefined as Promise<void> | undefined,
    completeOnRead: false,
  };
  await page.route("**/api/v1/**", async (route) => {
    const request = route.request();
    const path = new URL(request.url()).pathname.replace("/api/v1", "");
    if (path === "/auth") return route.fulfill({ json: { enabled: false } });
    if (path === "/system/settings") {
      if (request.method() === "GET") {
        state.reads++;
        if (state.readGate) await state.readGate;
        if (state.failRead)
          return route.fulfill({ status: 503, json: { error: "Settings unavailable" } });
        if (state.completeOnRead && state.operation) {
          state.operation = { ...state.operation, state: "succeeded" };
          state.completeOnRead = false;
        }
        return route.fulfill({
          json: {
            settings: state.settings,
            revision: state.revision,
            operation: state.operation,
          },
        });
      }
      expect(request.method()).toBe("PUT");
      const body = request.postDataJSON();
      expect(body.revision).toBe(state.revision);
      state.writes++;
      state.settings = body.settings;
      state.revision = "2".repeat(64);
      state.operation = {
        id: "settings-save",
        state: "applying",
        target_revision: state.revision,
      };
      state.completeOnRead = true;
      return route.fulfill({
        status: 202,
        json: {
          operation: state.operation,
          target_revision: state.revision,
          next_web: {
            enabled: state.settings.httpWeb.enabled,
            ...state.settings.httpWeb.host,
            auth_enabled: state.settings.httpWeb.auth.enabled,
          },
        },
      });
    }
    if (path === "/system/interfaces") return route.fulfill({ json: { interfaces: [] } });
    if (path === "/profiles") return route.fulfill({ json: { profiles: [] } });
    if (path === "/groups") return route.fulfill({ json: { groups: [] } });
    if (path === "/subscriptions") return route.fulfill({ json: { subscriptions: [] } });
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
  return state;
}

async function showPage(page: Page, name: "Groups" | "Settings") {
  const menu = page.getByRole("button", { name: "Open menu", exact: true });
  if (await menu.isVisible()) await menu.click();
  await page.getByRole("tab", { name, exact: true }).click();
}

async function openGeneral(page: Page) {
  await showPage(page, "Settings");
  await page.getByRole("tab", { name: "General", exact: true }).click();
}

async function expectLoaded(page: Page) {
  await expect(page.getByRole("textbox", { name: "Upstream DNS server", exact: true })).toBeVisible();
  await expect(page.locator("#general")).toHaveAttribute("aria-busy", "false");
}

async function revisit(page: Page) {
  await page.getByRole("tab", { name: "Profiles", exact: true }).click();
  await page.getByRole("tab", { name: "General", exact: true }).click();
  await expectLoaded(page);
}

test("General loads once across settings tabs and main-page navigation", async ({ page }) => {
  const state = await backend(page);
  await page.goto("/");
  await openGeneral(page);
  await expectLoaded(page);
  expect(state.reads).toBe(1);
  for (let i = 0; i < 3; i++) await revisit(page);
  await showPage(page, "Groups");
  await openGeneral(page);
  await expectLoaded(page);
  expect(state.reads).toBe(1);
  expect(state.writes).toBe(0);
  // A real page reload starts a new state and must fetch the current settings.
  await page.reload();
  await openGeneral(page);
  await expectLoaded(page);
  expect(state.reads).toBe(2);
});

test("General does not duplicate its initial in-flight read on tab switches", async ({ page }) => {
  const state = await backend(page);
  let release!: () => void;
  state.readGate = new Promise<void>((resolve) => {
    release = resolve;
  });
  try {
    await page.goto("/");
    await openGeneral(page);
    await expect.poll(() => state.reads).toBe(1);
    await page.getByRole("tab", { name: "Profiles", exact: true }).click();
    await page.getByRole("tab", { name: "General", exact: true }).click();
    await expect(page.locator("#general")).toHaveAttribute("aria-busy", "true");
  } finally {
    release();
  }
  await expectLoaded(page);
  expect(state.reads).toBe(1);
});

test("General can retry a failed initial read and then reuse the snapshot", async ({ page }) => {
  const state = await backend(page);
  state.failRead = true;
  await page.goto("/");
  await openGeneral(page);
  await expect(page.getByText("Failed to load general settings", { exact: true })).toBeVisible();
  expect(state.reads).toBe(1);
  state.failRead = false;
  await page.locator("#general").getByRole("button", { name: "Retry", exact: true }).click();
  await expectLoaded(page);
  expect(state.reads).toBe(2);
  await revisit(page);
  expect(state.reads).toBe(2);
});

test("Check application explicitly refreshes a cached external operation", async ({ page }) => {
  const state = await backend(page);
  state.operation = { id: "external", state: "applying", target_revision: state.revision };
  await page.goto("/");
  await openGeneral(page);
  await expectLoaded(page);
  await expect(page.getByRole("textbox", { name: "Upstream DNS server", exact: true })).toBeDisabled();
  expect(state.reads).toBe(1);
  state.operation = { ...state.operation, state: "succeeded" };
  await page.getByRole("button", { name: "Settings need attention", exact: true }).click();
  await page.getByRole("button", { name: "Check application", exact: true }).click();
  await expect(page.getByRole("textbox", { name: "Upstream DNS server", exact: true })).toBeEnabled();
  expect(state.reads).toBe(2);
  await page.keyboard.press("Escape");
  await revisit(page);
  expect(state.reads).toBe(2);
});

test("General still checks saves and reuses the confirmed settings on return", async ({ page }) => {
  const state = await backend(page);
  await page.goto("/");
  await openGeneral(page);
  await expectLoaded(page);
  const toggle = page.getByRole("switch", { name: "Show all interfaces in selectors", exact: true });
  await toggle.click();
  await page.locator("#save-settings").click();
  await expect(page.locator("#general")).toHaveAttribute("data-application-state", "succeeded");
  await expectLoaded(page);
  expect(state.writes).toBe(1);
  expect(state.reads).toBe(2);
  await revisit(page);
  await expect(toggle).toBeChecked();
  await expect(page.locator("#save-settings")).toBeDisabled();
  expect(state.reads).toBe(2);
});
