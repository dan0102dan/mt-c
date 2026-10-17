import { expect, test, type Page } from "@playwright/test";

type Query = {
  id: number;
  time_ms: number;
  client: string;
  domain: string;
  qtype: number;
  protocol: "udp" | "tcp";
};

const sample: Query[] = [
  { id: 3, time_ms: Date.now(), client: "192.168.1.10", domain: "example.com", qtype: 1, protocol: "udp" },
  { id: 2, time_ms: Date.now() - 1000, client: "192.168.1.11", domain: "example.com", qtype: 28, protocol: "tcp" },
  { id: 1, time_ms: Date.now() - 2000, client: "192.168.1.10", domain: "another.net", qtype: 65, protocol: "udp" },
];

async function setup(page: Page, opts: { failFirst?: boolean } = {}) {
  const state = { history: [...sample], reads: 0, clears: 0 };
  await page.route("**/api/v1/**", async (route) => {
    const { pathname } = new URL(route.request().url());
    const path = pathname.slice("/api/v1".length);
    if (route.request().method() === "OPTIONS") {
      await route.fulfill({ status: 204 });
      return;
    }
    if (path === "/auth") {
      await route.fulfill({ json: { enabled: false } });
      return;
    }
    if (path === "/system/dns/queries") {
      if (route.request().method() === "DELETE") {
        state.clears++;
        state.history = [];
        await route.fulfill({ json: { cleared: true } });
        return;
      }
      state.reads++;
      if (opts.failFirst && state.reads === 1) {
        await route.fulfill({ status: 503, json: { error: "temporary unavailable" } });
        return;
      }
      await route.fulfill({ json: { queries: state.history, capacity: 1000 } });
      return;
    }
    if (path === "/groups" || path === "/groups/") {
      await route.fulfill({ json: { groups: [] } });
      return;
    }
    if (path === "/subscriptions" || path === "/subscriptions/") {
      await route.fulfill({ json: { subscriptions: [] } });
      return;
    }
    if (path === "/profiles") {
      await route.fulfill({ json: { profiles: [] } });
      return;
    }
    if (path === "/system/interfaces") {
      await route.fulfill({ json: { interfaces: [] } });
      return;
    }
    if (path === "/system/update/status") {
      await route.fulfill({ json: { stage: "idle" } });
      return;
    }
    await route.fulfill({ json: {} });
  });
  await page.goto("/");
  const menu = page.getByRole("button", { name: "Open menu" });
  if (await menu.isVisible()) await menu.click();
  await page.getByRole("tab", { name: "Settings" }).click();
  const region = page.locator(".dns-query-log");
  await expect(region).toBeVisible();
  return { state, region };
}

test("DNS journal filters by domain and client, then groups all matching rows", async ({ page }) => {
  const { region } = await setup(page);
  await expect(region.locator("tbody tr")).toHaveCount(3);
  await region.getByRole("searchbox").fill("EXAMPLE");
  await expect(region.locator("tbody tr")).toHaveCount(2);
  await region.getByLabel("Device").selectOption("192.168.1.10");
  await expect(region.locator("tbody tr")).toHaveCount(1);
  await region.getByLabel("Device").selectOption("");
  await region.getByRole("searchbox").fill("");
  await region.getByRole("button", { name: "Group by domain" }).click();
  await expect(region.locator("tbody tr")).toHaveCount(2);
  await expect(region.locator("tbody tr").first()).toContainText("example.com");
  await expect(region.locator("tbody tr").first()).toContainText("192.168.1.11");
  await expect(region.locator("tbody tr").first()).toContainText("2");
  await region.getByRole("button", { name: "Individual queries" }).click();
  await expect(region.locator("tbody tr")).toHaveCount(3);
});

test("DNS history can be paused and cleared without stale rows reappearing", async ({ page }) => {
  const { region, state } = await setup(page);
  await expect(region.locator("tbody tr")).toHaveCount(3);
  await region.getByRole("button", { name: "Pause" }).click();
  await expect(region.getByRole("button", { name: "Resume" })).toHaveAttribute("aria-pressed", "true");
  page.once("dialog", async (dialog) => dialog.accept());
  await region.getByRole("button", { name: "Clear history" }).click();
  await expect(region.getByText("No DNS queries captured yet")).toBeVisible();
  expect(state.clears).toBe(1);
});

test("DNS history shows a retry action on a fetch failure", async ({ page }) => {
  const { region } = await setup(page, { failFirst: true });
  await expect(region.getByRole("alert")).toContainText("Failed to load DNS queries");
  await region.getByRole("button", { name: "Retry" }).click();
  await expect(region.locator("tbody tr")).toHaveCount(3);
  await expect(region.getByRole("alert")).toHaveCount(0);
});

test("DNS log controls fit a narrow phone viewport", async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 844 });
  const { region } = await setup(page);
  await expect(region.getByLabel("Device")).toBeVisible();
  await expect(region.getByRole("button", { name: "Group by domain" })).toBeVisible();
  const overflow = await page.evaluate(() => document.documentElement.scrollWidth > window.innerWidth);
  expect(overflow).toBe(false);
});
