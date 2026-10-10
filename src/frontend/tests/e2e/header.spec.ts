import { expect, test } from "@playwright/test";

test.describe("Header Settings", () => {
  test.beforeEach(async ({ page }) => {
    // Mock auth disabled to access layout directly
    await page.route("**/auth", async (route) => route.fulfill({ json: { enabled: false } }));
    await page.route("**/groups?with_rules=true", async (route) =>
      route.fulfill({ json: { groups: [] } }),
    );
    await page.route("**/interfaces", async (route) => route.fulfill({ json: { interfaces: [] } }));
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
    await page.route("**/system/update/status", (route) =>
      route.fulfill({ json: { stage: "idle" } }),
    );
    await page.route("https://api.github.com/**", (route) =>
      route.fulfill({ status: 404, json: {} }),
    );
    await page.goto("/");
  });

  test("should display version", async ({ page }) => {
    // Version is in .version span.version-text
    const version = page.locator(".version .version-text");
    await expect(version).toBeVisible();
    await expect(version).not.toHaveText("");
  });

  test("development version opens localized popup from keyboard", async ({ page }) => {
    const trigger = page.getByRole("button", { name: /Software update:.*dev-build/ });
    await expect(trigger).toBeVisible();
    await trigger.hover();
    await expect(page.locator("#global-tooltip")).toHaveText("dev-build");
    await trigger.focus();
    await page.keyboard.press("Enter");
    const popup = page.getByRole("dialog", { name: "Software update", exact: true });
    await expect(popup).toBeVisible();
    await expect(popup.getByRole("button", { name: "Up to date", exact: true })).toBeDisabled();
    await page.keyboard.press("Escape");
    await page.locator(".locale button").click();
    await page.getByRole("button", { name: /Обновление программы:/ }).click();
    await expect(
      page
        .getByRole("dialog")
        .getByRole("button", { name: "Установлена актуальная версия", exact: true }),
    ).toBeVisible();
  });

  test("dev toggle selects cached releases without requests and persists the channel", async ({
    page,
  }) => {
    await page.getByRole("button", { name: /Software update:/ }).click();
    await expect(page.getByRole("button", { name: "Up to date", exact: true })).toBeVisible();
    await page.keyboard.press("Escape");
    let releaseChecks = 0;
    let routerChecks = 0;
    await page.route("https://api.github.com/**", (route) => {
      releaseChecks++;
      expect(route.request().url()).toContain("releases?per_page=100");
      return route.fulfill({
        json: [
          {
            id: 1,
            tag_name: "1.0.0",
            draft: false,
            prerelease: false,
            published_at: "2026-10-01T00:00:00Z",
            assets: [],
          },
          {
            id: 2,
            tag_name: "2.0.0",
            draft: false,
            prerelease: true,
            published_at: "2026-10-02T00:00:00Z",
            assets: [],
          },
        ],
      });
    });
    await page.route("**/system/update", (route) => {
      routerChecks++;
      return route.fulfill({
        json: {
          installed_version: "1.0.0",
          installed_revision: 1,
          asset_suffix: "test.ipk",
          can_install: true,
          reason: "",
        },
      });
    });
    await page.evaluate(() => localStorage.removeItem("mt-c.releases.v2"));
    await page.reload();
    await page.getByRole("button", { name: /Software update:/ }).click();
    const popup = page.getByRole("dialog", { name: "Software update", exact: true });
    const toggle = popup.getByRole("switch", { name: "dev" });
    await expect(toggle).toBeEnabled();
    await expect(popup.getByRole("link", { name: "1.0.0 · Release notes" })).toBeVisible();
    await toggle.focus();
    await page.keyboard.press("Space");
    await expect(toggle).toBeChecked();
    await expect(popup.getByRole("link", { name: "2.0.0 · Release notes" })).toBeVisible();
    await toggle.click();
    await expect(toggle).not.toBeChecked();
    await expect(popup.getByRole("link", { name: "1.0.0 · Release notes" })).toBeVisible();
    await toggle.click();
    await expect(popup.getByRole("link", { name: "2.0.0 · Release notes" })).toBeVisible();
    expect(releaseChecks).toBe(1);
    expect(routerChecks).toBe(1);
    await page.reload();
    await page.getByRole("button", { name: /Software update:/ }).click();
    await expect(toggle).toBeChecked();
    await expect(toggle).toBeEnabled();
    await expect(popup.getByRole("link", { name: "2.0.0 · Release notes" })).toBeVisible();
    expect(releaseChecks).toBe(1);
    await toggle.click();
    await expect(toggle).not.toBeChecked();
    await expect(popup.getByRole("link", { name: "1.0.0 · Release notes" })).toBeVisible();
    expect(routerChecks).toBe(2);
  });

  test("should rotate locale", async ({ page }) => {
    const localeBtn = page.locator(".locale button");

    // Get initial text (flag)
    const initialText = await localeBtn.textContent();

    // Click to rotate
    await localeBtn.click();

    // Verify text changed
    await expect(localeBtn).not.toHaveText(initialText || "");

    // Click again to rotate back (assuming 2 locales, or just rotate more)
    await localeBtn.click();
  });

  test("should open info dialog", async ({ page }) => {
    const infoBtn = page.locator(".info button");
    await expect(infoBtn).toBeVisible();

    await infoBtn.click();

    // Check dialog title
    const dialog = page.locator("[data-dialog-content]");
    await expect(dialog).toBeVisible();
    await expect(dialog.getByText("About", { exact: true })).toBeVisible();

    // Check for some content
    await expect(dialog.locator("text=Official website")).toBeVisible();
    await expect(dialog.locator("text=Bug Tracker")).toBeVisible();

    // Close dialog
    await page.keyboard.press("Escape");
    await expect(dialog).not.toBeVisible();
  });
});

for (const device of [
  {
    name: "Windows",
    width: 1280,
    platform: "Win32",
    userAgent: "Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
    touchPoints: 0,
    instruction: "Press F5 to reload the page.",
  },
  {
    name: "Mac",
    width: 1280,
    platform: "MacIntel",
    userAgent: "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)",
    touchPoints: 0,
    instruction: "Press ⌘R to reload the page.",
  },
  {
    name: "Android",
    width: 390,
    platform: "Linux armv8l",
    userAgent: "Mozilla/5.0 (Linux; Android 14)",
    touchPoints: 5,
    instruction: "Refresh the page in your browser.",
  },
  {
    name: "iPad",
    width: 390,
    platform: "MacIntel",
    userAgent: "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)",
    touchPoints: 5,
    instruction: "Refresh the page in your browser.",
  },
]) {
  test(`version popup shows update progress on ${device.name}`, async ({ page }) => {
    const { width } = device;
    await page.addInitScript(({ platform, userAgent, touchPoints }) => {
      Object.defineProperty(navigator, "platform", { get: () => platform });
      Object.defineProperty(navigator, "userAgent", { get: () => userAgent });
      Object.defineProperty(navigator, "maxTouchPoints", { get: () => touchPoints });
    }, device);
    await page.setViewportSize({ width, height: 900 });
    await page.route("**/auth", (route) => route.fulfill({ json: { enabled: false } }));
    await page.route("**/groups?with_rules=true", (route) =>
      route.fulfill({ json: { groups: [] } }),
    );
    await page.route("**/interfaces", (route) => route.fulfill({ json: { interfaces: [] } }));
    let finished = false;
    await page.route("**/system/update", (route) =>
      route.fulfill({
        json: {
          installed_version: finished ? "2.0.0" : "1.0.0",
          installed_revision: 1,
          asset_suffix: "test.ipk",
          can_install: true,
          reason: "",
        },
      }),
    );
    const job = {
      stage: "downloading",
      job_id: "a".repeat(32),
      tag: "2.0.0",
      target_version: "2.0.0",
      target_revision: 1,
    };
    let started = false;
    let installs = 0;
    await page.route("**/system/update/status", (route) =>
      route.fulfill({ json: started ? job : { stage: "idle" } }),
    );
    await page.route("**/system/update/install", (route) => {
      installs++;
      expect(route.request().postDataJSON()).toEqual({
        tag: "2.0.0",
        release_id: 1,
        preview: false,
      });
      job.stage = "downloading";
      started = true;
      return route.fulfill({ json: job });
    });
    await page.route("https://api.github.com/**", (route) =>
      route.fulfill({
        json: [
          {
            id: 1,
            tag_name: "2.0.0",
            draft: false,
            prerelease: false,
            published_at: "2026-10-01T00:00:00Z",
            assets: [
              {
                name: "mt-c_2.0.0-1_test.ipk",
                browser_download_url:
                  "https://github.com/dan0102dan/mt-c/releases/download/2.0.0/mt-c_2.0.0-1_test.ipk",
                state: "uploaded",
                size: 123,
                digest: `sha256:${"a".repeat(64)}`,
              },
            ],
          },
        ],
      }),
    );
    await page.goto("/");
    const trigger = page.getByRole("button", { name: /Software update:.*New version available/ });
    await expect(trigger).toBeVisible();
    await trigger.click();
    const popup = page.getByRole("dialog", { name: "Software update", exact: true });
    await expect(popup).toBeVisible();
    const bounds = await popup.boundingBox();
    expect(bounds!.x).toBeGreaterThanOrEqual(0);
    expect(bounds!.x + bounds!.width).toBeLessThanOrEqual(width);
    await expect(popup.getByRole("button")).toHaveCount(1);
    await expect(popup.getByRole("link", { name: "2.0.0 · Release notes" })).toHaveAttribute(
      "href",
      "https://github.com/dan0102dan/mt-c/releases/tag/2.0.0",
    );
    await expect(popup.getByRole("button", { name: "Update", exact: true })).toBeEnabled();
    await page.screenshot({ path: `/tmp/mt-update-ready-${width}.png` });
    await popup.getByRole("button", { name: "Update", exact: true }).click();
    await expect(popup.getByText("Downloading package", { exact: true })).toBeVisible();
    await expect(popup.getByRole("button", { name: "Updating…", exact: true })).toBeDisabled();
    await expect(popup.getByRole("switch", { name: "dev" })).toBeDisabled();
    await page.keyboard.press("Escape");
    await expect(popup).not.toBeVisible();
    await expect(trigger).toBeFocused();
    await trigger.click();
    await expect(popup.getByText("Downloading package", { exact: true })).toBeVisible();
    expect(installs).toBe(1);
    await page.screenshot({ path: `/tmp/mt-update-busy-${width}.png` });
    if (width === 390) {
      job.stage = "failed";
      await expect(popup.getByText("Update failed", { exact: true })).toBeVisible();
      await popup.getByRole("button", { name: "Update", exact: true }).click();
      await expect(popup.getByRole("button", { name: "Updating…", exact: true })).toBeDisabled();
      expect(installs).toBe(2);
    }
    finished = true;
    job.stage = "succeeded";
    await expect(
      popup.getByText(device.instruction, {
        exact: true,
      }),
    ).toBeVisible();
    await expect(popup.getByRole("button", { name: "Up to date", exact: true })).toBeDisabled();
    await expect(popup.getByRole("button")).toHaveCount(1);
    await page.screenshot({ path: `/tmp/mt-update-done-${width}.png` });
    await page.reload();
    await page.getByRole("button", { name: /Software update:/ }).click();
    await expect(popup.getByRole("button", { name: "Up to date", exact: true })).toBeDisabled();
    await expect(popup.getByText(device.instruction, { exact: true })).toHaveCount(0);
    await expect(popup.getByRole("button")).toHaveCount(1);
  });
}
