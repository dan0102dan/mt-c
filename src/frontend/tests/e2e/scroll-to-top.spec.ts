import { expect, test, type Page } from "@playwright/test";

async function loadGroups(page: Page, manyPanels: boolean) {
  const groups = Array.from({ length: manyPanels ? 1000 : 1 }, (_, index) => ({
    id: (0x10000000 + index).toString(16),
    name: `Scrolling group ${index + 1}`,
    color: "#42bdf9",
    interface: "blackhole",
    enable: true,
    priority: 300,
    rules: Array.from({ length: manyPanels ? 0 : 100000 }, (_, rule) => ({
      id: (0x20000000 + rule).toString(16),
      name: "",
      rule: `item-${rule}.example.test`,
      type: "namespace",
      enable: true,
    })),
  }));
  await page.route("**/api/v1/**", async (route) => {
    if (route.request().method() === "OPTIONS") return route.fulfill({ status: 204 });
    const path = new URL(route.request().url()).pathname.replace("/api/v1", "");
    const fixtures: Record<string, unknown> = {
      "/auth": { enabled: false },
      "/groups": { groups },
      "/subscriptions": { subscriptions: [] },
      "/profiles": { profiles: [] },
      "/system/interfaces": { interfaces: [{ id: "blackhole", name: "" }] },
      "/system/update/status": { stage: "idle" },
      "/system/update": { installed_version: "test", can_install: false },
    };
    return route.fulfill({ json: fixtures[path] ?? {} });
  });
  await page.route("https://api.github.com/**", (route) => route.fulfill({ json: [] }));
  await page.goto("/");
  await expect(page.locator("input.group-name").first()).toHaveValue("Scrolling group 1");
  if (!manyPanels) {
    await page.locator(".group [data-collapsible-trigger]").click();
    await expect(page.locator(".rule").first()).toBeVisible();
  }
}

for (const manyPanels of [true, false]) {
  test(`Back to top repeatedly reaches the document start through virtual ${manyPanels ? "panels" : "100k rules"}`, async ({
    page,
  }) => {
    test.setTimeout(60000);
    await loadGroups(page, manyPanels);
    const button = page.getByRole("button", { name: "Back to top", exact: true });
    for (const fraction of [0.9, 0.5, 0.95]) {
      await page.evaluate(
        (fraction) => window.scrollTo(0, document.documentElement.scrollHeight * fraction),
        fraction,
      );
      await expect.poll(() => page.evaluate(() => window.scrollY)).toBeGreaterThan(1000);
      await expect(button).toBeVisible();
      await button.click();
      await expect
        .poll(() => page.evaluate(() => window.scrollY), { timeout: 8000 })
        .toBeLessThan(1);
      await expect(button).toBeHidden();
      await expect(page.locator("input.group-name").first()).toHaveValue("Scrolling group 1");
    }
  });
}

test("Back to top respects reduced motion", async ({ page }) => {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await loadGroups(page, true);
  await page.evaluate(() => window.scrollTo(0, 10000));
  const button = page.getByRole("button", { name: "Back to top", exact: true });
  await expect(button).toBeVisible();
  await button.click();
  expect(await page.evaluate(() => window.scrollY)).toBe(0);
  await expect(button).toBeHidden();
});

test("manual wheel input stops an ongoing Back to top animation", async ({ page }) => {
  await page.clock.install();
  await loadGroups(page, true);
  await page.evaluate(() => window.scrollTo(0, document.documentElement.scrollHeight * 0.9));
  const button = page.getByRole("button", { name: "Back to top", exact: true });
  await expect(button).toBeVisible();
  const start = await page.evaluate(() => window.scrollY);
  await page.clock.pauseAt(await page.evaluate(() => Date.now() + 1000));
  // The clock is paused, so activate the already visible button without the
  // actionability check waiting for two animation frames that cannot advance.
  await button.click({ force: true });
  await page.clock.runFor(160);
  const during = await page.evaluate(() => window.scrollY);
  expect(during).toBeGreaterThan(1000);
  expect(during).toBeLessThan(start - 1000);
  await page.mouse.wheel(0, 800);
  await expect.poll(() => page.evaluate(() => window.scrollY)).toBeGreaterThan(during + 100);
  // Wait beyond the animation's lifetime: a later frame must not pull the user
  // back to zero. Freeze its clock during the real wheel event so this checks
  // cancellation mid-animation even on a heavily loaded browser.
  await page.clock.runFor(1000);
  expect(await page.evaluate(() => window.scrollY)).toBeGreaterThan(1000);
  await page.clock.resume();
  await expect(button).toBeVisible();
});
