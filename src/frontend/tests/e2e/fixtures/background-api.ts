import type { Page } from "@playwright/test";

/** Eagerly mounted layout dependencies must stay offline in isolated UI tests.
 * Register before feature routes so their fixtures can override these defaults. */
export async function mockBackgroundApi(page: Page) {
  await page.route("**/profiles", (route) => route.fulfill({ json: { profiles: [] } }));
  await page.route("**/subscriptions", (route) => route.fulfill({ json: { subscriptions: [] } }));
  await page.route("**/system/update/status", (route) =>
    route.fulfill({ json: { stage: "idle" } }),
  );
  await page.route("**/system/update", (route) =>
    route.fulfill({
      json: {
        installed_version: "1.0.0",
        installed_revision: 1,
        asset_suffix: "test.ipk",
        can_install: false,
        reason: "",
      },
    }),
  );
  await page.route("https://api.github.com/**", (route) => route.fulfill({ json: [] }));
}
