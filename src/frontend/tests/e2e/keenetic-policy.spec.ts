import { expect, test, type Page } from "@playwright/test";

const keenetic = process.env.VITE_ENTWARE_KN === "1";
type Settings = { enabled: boolean; policy_id: string; mode: "include" | "exclude" };
async function backend(page: Page, enabled = false) {
  const state = {
    settings: { enabled, policy_id: enabled ? "Policy0" : "", mode: "include" } as Settings,
    policies: [
      { id: "Policy0", name: "VPN", available: true },
      { id: "Policy1", name: "VPN", available: true },
    ],
    writes: 0,
    listReads: 0,
    settingsReads: 0,
    listFailure: false,
    diskFailure: false,
  };
  await page.route("**/api/v1/**", async (route) => {
    const req = route.request();
    const path = new URL(req.url()).pathname.replace("/api/v1", "");
    if (req.method() === "OPTIONS") {
      await route.fulfill({ status: 204 });
      return;
    }
    if (path === "/auth") {
      await route.fulfill({ json: { enabled: false } });
      return;
    }
    if (path === "/keenetic/policy") {
      state.settingsReads++;
      if (req.method() === "PUT") {
        state.writes++;
        state.settings = req.postDataJSON();
      }
      const value = {
        settings: state.settings,
        runtime: { state: state.settings.enabled ? "ready" : "disabled" },
      };
      await route.fulfill({
        status: req.method() === "PUT" && state.diskFailure ? 500 : 200,
        json:
          req.method() === "PUT" && state.diskFailure
            ? { ...value, code: "PERSISTENCE_FAILED", applied: true, error: "Disk is full" }
            : value,
      });
      return;
    }
    if (path === "/keenetic/policies") {
      state.listReads++;
      await route.fulfill({
        status: state.listFailure ? 502 : 200,
        json: state.listFailure ? { error: "RCI is unavailable" } : { policies: state.policies },
      });
      return;
    }
    if (path === "/profiles") {
      await route.fulfill({ json: { profiles: [] } });
      return;
    }
    if (path === "/groups") {
      await route.fulfill({ json: { groups: [] } });
      return;
    }
    if (path === "/subscriptions") {
      await route.fulfill({ json: { subscriptions: [] } });
      return;
    }
    if (path === "/system/interfaces") {
      await route.fulfill({ json: { interfaces: [] } });
      return;
    }
    await route.fulfill({ json: { version: "test" } });
  });
  return state;
}
async function openSettings(page: Page) {
  await page.goto("/");
  if ((page.viewportSize()?.width ?? 1280) <= 700)
    await page.getByRole("button", { name: "Open menu", exact: true }).click();
  await page.getByRole("tab", { name: "Settings", exact: true }).click();
  await expect(page.getByRole("button", { name: "Add profile", exact: true })).toBeVisible();
}
const panel = (page: Page) => page.locator(".policy-settings");
const save = (page: Page) => page.locator("#save-keenetic-policy");

test("generic UI has no policy feature or policy requests", async ({ page }) => {
  test.skip(keenetic, "generic build only");
  const state = await backend(page);
  await openSettings(page);
  await expect(panel(page)).toHaveCount(0);
  expect(state.settingsReads).toBe(0);
  expect(state.listReads).toBe(0);
});

test.describe("Keenetic policy settings", () => {
  test.skip(!keenetic, "Keenetic-only bundle");
  test("off by default, explicit save, include/exclude and reload", async ({ page }, testInfo) => {
    const state = await backend(page);
    await openSettings(page);
    const toggle = panel(page).getByRole("switch");
    await expect(toggle).not.toBeChecked();
    expect(state.listReads).toBe(0);
    await toggle.click();
    await expect(panel(page).getByRole("radio")).toHaveCount(2);
    await expect(save(page)).toBeDisabled();
    await panel(page).locator('[data-select-trigger][aria-label="Router policy"]').click();
    await page.getByRole("option", { name: /VPN.*Policy0/ }).click();
    expect(state.writes).toBe(0);
    await save(page).click();
    await expect(save(page)).toBeDisabled();
    expect(state.settings).toEqual({ enabled: true, policy_id: "Policy0", mode: "include" });
    await panel(page)
      .getByRole("radio", { name: "Do not process devices in the selected policy", exact: true })
      .check();
    await save(page).click();
    await expect.poll(() => state.settings.mode).toBe("exclude");
    await page.reload();
    await expect(
      panel(page).getByRole("radio", {
        name: "Do not process devices in the selected policy",
        exact: true,
      }),
    ).toBeChecked();
    await expect(toggle).toBeChecked();
    if (process.env.POLICY_SCREENSHOTS)
      await panel(page).screenshot({ path: testInfo.outputPath("policy-desktop.png") });
  });
  test("missing policy is retained; RCI failure never blocks disabling", async ({ page }) => {
    const state = await backend(page, true);
    state.policies = [];
    await openSettings(page);
    await expect(
      panel(page).getByText(
        "The selected policy is missing or unavailable. Select another policy or disable the filter.",
        { exact: true },
      ),
    ).toBeVisible();
    expect(state.settings.policy_id).toBe("Policy0");
    state.listFailure = true;
    await panel(page).getByRole("button", { name: "Refresh policies", exact: true }).click();
    await expect(panel(page).getByRole("alert")).toContainText("Failed to load Keenetic policies");
    await panel(page).getByRole("switch").click();
    await expect(save(page)).toBeEnabled();
    await save(page).click();
    await expect(save(page)).toBeDisabled();
    expect(state.settings.enabled).toBe(false);
    expect(state.settings.policy_id).toBe("Policy0");
  });
  test("disk failure acknowledges memory settings and keeps Save available for retry", async ({
    page,
  }) => {
    const state = await backend(page, true);
    await openSettings(page);
    await expect(panel(page).getByRole("switch")).toBeChecked();
    await panel(page).getByRole("switch").click();
    state.diskFailure = true;
    await save(page).click();
    await expect(
      panel(page).getByText("Changes are applied but not saved to disk. Retry saving.", {
        exact: true,
      }),
    ).toBeVisible();
    await expect(save(page)).toBeEnabled();
    expect(state.settings.enabled).toBe(false);
    state.diskFailure = false;
    await save(page).click();
    await expect(save(page)).toBeDisabled();
    await expect(panel(page).getByRole("alert")).toHaveCount(0);
    expect(state.writes).toBe(2);
  });
  test("list refresh and status polling preserve unsaved mode and ID", async ({ page }) => {
    const state = await backend(page, true);
    await openSettings(page);
    await panel(page)
      .getByRole("radio", { name: "Do not process devices in the selected policy", exact: true })
      .check();
    await panel(page).locator('[data-select-trigger][aria-label="Router policy"]').click();
    await page.getByRole("option", { name: /VPN.*Policy1/ }).click();
    await panel(page).getByRole("button", { name: "Refresh policies", exact: true }).click();
    await expect.poll(() => state.settingsReads, { timeout: 7000 }).toBeGreaterThan(1);
    await expect(
      panel(page).getByRole("radio", {
        name: "Do not process devices in the selected policy",
        exact: true,
      }),
    ).toBeChecked();
    await expect(
      panel(page).locator('[data-select-trigger][aria-label="Router policy"]'),
    ).toContainText("VPN");
    await save(page).click();
    await expect.poll(() => state.settings.policy_id).toBe("Policy1");
    expect(state.settings.mode).toBe("exclude");
  });
  test("narrow viewport remains usable without horizontal overflow", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await backend(page, true);
    await openSettings(page);
    await expect(panel(page).getByRole("switch")).toBeChecked();
    await panel(page)
      .getByRole("radio", { name: "Do not process devices in the selected policy", exact: true })
      .check();
    await expect(save(page)).toBeEnabled();
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(
      true,
    );
    await save(page).click();
    await expect(save(page)).toBeDisabled();
  });
});
