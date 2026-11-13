import { expect, test } from "@playwright/test";

for (const tab of ["Groups", "Subscriptions"]) {
  test(`${tab}: search stays open on desktop and supports system shortcuts`, async ({ page }) => {
    // The layout eagerly mounts subscriptions and the updater beside this search.
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
    // Profiles are loaded during authentication even while the Groups tab is visible.
    await page.route("**/profiles", (route) => route.fulfill({ json: { profiles: [] } }));
    await page.route("**/auth", (route) => route.fulfill({ json: { enabled: false } }));
    await page.route("**/groups?with_rules=true", (route) =>
      route.fulfill({ json: { groups: [] } }),
    );
    await page.route("**/subscriptions", (route) => route.fulfill({ json: { subscriptions: [] } }));
    await page.route("**/interfaces", (route) => route.fulfill({ json: { interfaces: [] } }));
    await page.goto("/");
    await page.getByRole("tab", { name: tab, exact: true }).click();

    const search = page.locator('[data-tabs-content][data-state="active"] .search-input');
    const wrapper = page.locator('[data-tabs-content][data-state="active"] .input-wrapper');
    await expect(search).toHaveCSS("opacity", "1");
    await expect.poll(async () => (await wrapper.boundingBox())?.width ?? 0).toBeGreaterThan(100);

    for (const shortcut of ["Control+f", "Meta+f"]) {
      await page.keyboard.press(shortcut);
      await expect(search).toBeFocused();
      await search.fill("previous query");
      await search.blur();
      await page.keyboard.press(shortcut);
      await expect(search).toBeFocused();
      await page.keyboard.insertText("replacement");
      await expect(search).toHaveValue("replacement");
    }

    await search.fill("");
    await search.blur();
    await expect(search).toHaveCSS("opacity", "1");
    await page.setViewportSize({ width: 390, height: 844 });
    await expect(wrapper).toHaveCSS("width", "0px");
    await expect(search).toHaveCSS("opacity", "0");
    await page.locator('[data-tabs-content][data-state="active"] .search-container').click();
    await expect(search).toBeFocused();
    await expect(search).toHaveCSS("opacity", "1");
    await search.fill("query");
    await search.blur();
    await expect(search).toHaveCSS("opacity", "1");
    await search.fill("");
    await search.blur();
    await expect(wrapper).toHaveCSS("width", "0px");
  });
}
