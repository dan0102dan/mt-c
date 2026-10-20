import { expect, test, type Page } from "@playwright/test";

async function expectTrackBelowHeader(page: Page, content: string) {
  await expect
    .poll(() =>
      page.evaluate((selector) => {
        const track = document.querySelector<HTMLElement>(".overlay-scrollbar")!;
        const header = document.querySelector<HTMLElement>(
          '[data-tabs-content][data-state="active"] .page-controls',
        )!;
        const target = document.querySelector<HTMLElement>(selector)!;
        return Math.abs(
          track.getBoundingClientRect().top -
            Math.max(8, header.getBoundingClientRect().bottom, target.getBoundingClientRect().top),
        );
      }, content),
    )
    .toBeLessThan(1);
}

for (const viewport of [
  { width: 1280, height: 900 },
  { width: 390, height: 844 },
]) {
  for (const tab of ["Groups", "Subscriptions", "Settings"]) {
    test(`${tab}: scrollbar excludes header at ${viewport.width}px`, async ({ page }) => {
      await page.setViewportSize(viewport);
      await page.route("**/api/v1/**", (route) => {
        const path = new URL(route.request().url()).pathname.split("/").pop();
        const items = Array.from({ length: 20 }, (_, i) => ({
          id: (i + 1).toString(16).padStart(8, "0"),
          name: `Item ${i + 1}`,
          interface: "eth0",
          color: "#ffffff",
          enable: true,
          rules: [],
          url: `https://example.com/${i}.txt`,
          interval: 86400,
          lastUpdate: 1700000000,
        }));
        const fixtures: Record<string, unknown> = {
          auth: { enabled: false },
          groups: { groups: items },
          subscriptions: { subscriptions: items },
          interfaces: { interfaces: [{ id: "eth0" }] },
          profiles: {
            profiles: items.map(({ id, name }) => ({
              id,
              name,
              interfaces: ["eth0"],
              on_unavailable: "blackhole",
            })),
          },
          status: { stage: "idle" },
        };
        return route.fulfill({ json: fixtures[path ?? ""] ?? {} });
      });
      await page.route("https://api.github.com/**", (route) => route.fulfill({ json: [] }));
      await page.goto("/");
      await expect(page.locator(".group-list.visible")).toBeVisible();
      if (tab !== "Groups") {
        if (viewport.width < 700) await page.locator(".mobile-dropdown-btn").click();
        await page.getByRole("tab", { name: tab, exact: true }).click();
      }
      const content =
        tab === "Groups"
          ? ".group-list"
          : tab === "Subscriptions"
            ? ".subscription-list"
            : ".settings-sections";
      await expect(page.locator(content)).toBeVisible();
      await expectTrackBelowHeader(page, content);
      await page.evaluate(() => window.scrollTo(0, 300));
      await expectTrackBelowHeader(page, content);
      if (viewport.width < 700 && tab !== "Settings") {
        const search = page.locator('[data-tabs-content][data-state="active"] [data-page-search]');
        await search.focus();
        await expectTrackBelowHeader(page, content);
        await search.blur();
      }
      // The shortened track must still allow dragging to both document edges.
      await page.evaluate(() => window.scrollTo(0, 500));
      await expectTrackBelowHeader(page, content);
      await expect(page.locator(".overlay-scrollbar")).toHaveClass(/is-visible/);
      const box = (await page.locator(".overlay-scrollbar__thumb").boundingBox())!;
      await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
      await page.mouse.down();
      await page.mouse.move(box.x + box.width / 2, -viewport.height * 10);
      await expect.poll(() => page.evaluate(() => window.scrollY)).toBe(0);
      await page.mouse.move(box.x + box.width / 2, viewport.height * 100);
      await expect
        .poll(() =>
          page.evaluate(
            () => document.documentElement.scrollHeight - window.innerHeight - window.scrollY,
          ),
        )
        .toBeLessThan(2);
      await page.mouse.up();
      await expectTrackBelowHeader(page, content);
    });
  }
}
