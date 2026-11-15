import { expect, test } from "@playwright/test";

import { openInterface, selectLanguage } from "./helpers/interface";

test.describe("Interface preferences and header", () => {
  test.beforeEach(async ({ page }) => {
    // Mock auth disabled to access layout directly
    await page.route("**/auth", async (route) => route.fulfill({ json: { enabled: false } }));
    await page.route("**/groups?with_rules=true", async (route) =>
      route.fulfill({ json: { groups: [] } }),
    );
    await page.route("**/interfaces", async (route) => route.fulfill({ json: { interfaces: [] } }));
    await page.route("**/profiles", (route) => route.fulfill({ json: { profiles: [] } }));
    await page.route("**/subscriptions", (route) => route.fulfill({ json: { subscriptions: [] } }));
    // Settings stay mounted with the updater; keep these UI checks offline.
    await page.route("**/system/update/status", (route) =>
      route.fulfill({ json: { stage: "idle" } }),
    );
    await page.route("**/system/update", (route) =>
      route.fulfill({
        json: {
          installed_version: "1.0.0",
          installed_revision: 1,
          asset_suffix: "test.ipk",
          can_install: true,
          reason: "",
        },
      }),
    );
    await page.route("https://api.github.com/**", (route) => route.fulfill({ json: [] }));
    await page.goto("/");
    await openInterface(page);
  });

  test("build and app information live in Interface instead of the header", async ({ page }) => {
    const version = page.locator("#interface .version .version-text");
    await expect(version).toBeVisible();
    await expect(version).not.toHaveText("");
    const navigation = page.locator("nav");
    await expect(navigation.getByRole("tab")).toHaveCount(3);
    await expect(navigation.locator(".version, .locale, .info")).toHaveCount(0);
  });

  test("version popup opens from keyboard and follows language", async ({ page }) => {
    const trigger = page.getByRole("button", { name: /Software update:/ });
    await expect(trigger).toBeVisible();
    await trigger.focus();
    await page.keyboard.press("Enter");
    const popup = page.getByRole("dialog", { name: "Software update", exact: true });
    await expect(popup).toBeVisible();
    await expect(popup.getByText("No releases found for this channel")).toBeVisible();
    await expect(popup.getByRole("button", { name: "Check for updates" })).toBeEnabled();
    await page.keyboard.press("Escape");

    await selectLanguage(page, "Русский");
    await page.getByRole("button", { name: /Обновление программы:/ }).click();
    await expect(page.getByRole("dialog", { name: "Обновление программы" })).toBeVisible();
  });

  test("language selection updates the interface and survives reload", async ({ page }) => {
    await selectLanguage(page, "Русский");
    await expect(page.getByRole("tab", { name: "Интерфейс", exact: true })).toHaveAttribute(
      "aria-selected",
      "true",
    );
    await page.reload();
    await expect(page.locator("#interface")).toBeVisible();
    await expect(page.getByLabel("Язык", { exact: true })).toHaveText("Русский");
    await selectLanguage(page, "English");
    await expect(page.getByRole("tab", { name: "Interface", exact: true })).toHaveAttribute(
      "aria-selected",
      "true",
    );
  });

  test("should open info dialog", async ({ page }) => {
    const infoBtn = page.locator("#interface").getByRole("button", { name: "About", exact: true });
    await expect(infoBtn).toBeVisible();

    await infoBtn.click();

    // Check dialog title
    const dialog = page.locator("[data-dialog-content]");
    await expect(dialog).toBeVisible();
    await expect(dialog.getByText("About", { exact: true })).toBeVisible();

    // Check for some content
    await expect(dialog.locator("text=Official website")).toBeVisible();
    await expect(dialog.getByText("Issue Tracker")).toBeVisible();
    await expect(dialog.getByText("GitHub Repository")).toBeVisible();
    await expect(
      dialog.getByRole("link", { name: "https://github.com/dan0102dan/mt-c/issues", exact: true }),
    ).toHaveAttribute("href", "https://github.com/dan0102dan/mt-c/issues");
    await expect(
      dialog.getByRole("link", { name: "https://github.com/dan0102dan/mt-c", exact: true }),
    ).toHaveAttribute("href", "https://github.com/dan0102dan/mt-c");

    // Check translations after switching language.
    await page.keyboard.press("Escape");
    await expect(dialog).not.toBeVisible();
    await selectLanguage(page, "Русский");
    await page.locator("#interface").getByRole("button", { name: "Инфо", exact: true }).click();
    await expect(dialog.locator("[data-dialog-title]")).toHaveText("Инфо");
    await expect(dialog.locator(".build")).toContainText("Билд:");
    await expect(dialog.getByText("Трекер багов")).toBeVisible();
    await expect(dialog.getByText("Репозиторий GitHub")).toBeVisible();

    // Close dialog
    await page.keyboard.press("Escape");
    await expect(dialog).not.toBeVisible();
  });

  test("mobile navigation keeps all sections directly accessible in both languages", async ({
    page,
  }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    for (const language of ["English", "Русский"] as const) {
      await selectLanguage(page, language);
      const navigation = page.locator("nav");
      await expect(navigation.getByRole("tab")).toHaveCount(3);
      for (const tab of await navigation.getByRole("tab").all()) {
        await expect(tab).toBeVisible();
        await expect(tab).toBeInViewport();
      }
      const documentWidth = await page.evaluate(() => document.documentElement.scrollWidth);
      expect(documentWidth).toBeLessThanOrEqual(390);
    }
  });
});

test("logout removes the hovered tooltip with its trigger", async ({ page }) => {
  await page.addInitScript(() => {
    localStorage.setItem("auth", JSON.stringify({ value: "test-token" }));
  });
  await page.route("**/api/v1/**", (route) => {
    const path = new URL(route.request().url()).pathname;
    if (path.endsWith("/auth")) return route.fulfill({ json: { enabled: true } });
    if (path.endsWith("/interfaces")) return route.fulfill({ json: { interfaces: [] } });
    if (path.endsWith("/profiles")) return route.fulfill({ json: { profiles: [] } });
    return route.fulfill({ json: { groups: [] } });
  });
  await page.goto("/");
  const logout = page.locator(".logout button");
  await logout.hover();
  await expect(page.locator("#global-tooltip")).toHaveText("Logout");
  await logout.click();
  await expect(page.locator("#login")).toBeVisible();
  await expect(page.locator("#global-tooltip")).toHaveCount(0);
});
