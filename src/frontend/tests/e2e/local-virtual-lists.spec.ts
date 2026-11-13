import { expect, test } from "@playwright/test";

import type { Group } from "../../src/types";

for (const width of [1280, 390]) {
  test(`large duplicate popup scrolls locally and opens an offscreen conflict at ${width}px`, async ({
    page,
  }) => {
    await page.setViewportSize({ width, height: width === 390 ? 844 : 900 });
    const pattern = "shared-popup.example.test";
    const sourceId = "10000000";
    const targetId = "10000001";
    const rules = Array.from({ length: 1200 }, (_, index) => ({
      id: (0x20000100 + index).toString(16),
      name: `Target conflict ${String(index).padStart(4, "0")}`,
      rule: pattern,
      type: "namespace",
      enable: true,
    }));
    const groups: Group[] = [
      {
        id: sourceId,
        name: "Popup source",
        color: "#42bdf9",
        interface: "blackhole",
        enable: true,
        priority: 300,
        rules: [
          {
            id: "20000000",
            name: "Source duplicate",
            rule: pattern,
            type: "namespace",
            enable: true,
          },
        ],
      },
      {
        id: targetId,
        name: "Popup target",
        color: "#42bdf9",
        interface: "blackhole",
        enable: true,
        priority: 300,
        rules,
      },
      ...Array.from({ length: 30 }, (_, index) => ({
        id: (0x10000100 + index).toString(16),
        name: `Other group ${index}`,
        color: "#42bdf9",
        interface: "blackhole",
        enable: true,
        priority: 300,
        rules: [],
      })),
    ];
    await page.route("**/api/v1/**", (route) => {
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
    const source = page.locator(`.group[data-uuid="${sourceId}"]`);
    await expect(source.locator(".group-name")).toHaveValue("Popup source");
    await source.locator(".group-duplicate-indicator").click();
    const popup = page.locator(".group-duplicate-menu-list");
    await expect(popup).toBeVisible();
    const documentTop = await page.evaluate(() => scrollY);
    expect(await page.evaluate(() => document.documentElement.scrollHeight)).toBeGreaterThan(
      width === 390 ? 844 : 900,
    );
    const buttons = popup.locator(".duplicate-conflict-button");
    await expect.poll(() => buttons.count()).toBeLessThan(50);
    await expect(buttons.filter({ hasText: "Target conflict 0000" })).toBeVisible();
    await expect(buttons.filter({ hasText: "Source duplicate" })).toHaveCount(0);

    await popup.evaluate((element) => (element.scrollTop = element.scrollHeight));
    // Measurements may refine the estimated last page; keep the local scroll
    // intent at its end until the actual final logical record is mounted.
    await expect
      .poll(async () => {
        await popup.evaluate((element) => (element.scrollTop = element.scrollHeight));
        return buttons.filter({ hasText: "Source duplicate" }).count();
      })
      .toBe(1);
    await expect.poll(() => buttons.count()).toBeLessThan(50);
    expect(await page.evaluate(() => scrollY)).toBe(documentTop);
    const lastTarget = buttons.filter({ hasText: "Target conflict 1199" });
    await expect(lastTarget).toBeVisible();
    await lastTarget.click();

    await expect(popup).toBeHidden();
    const lastRule = page.locator(
      `.rule[data-group-uuid="${targetId}"][data-uuid="${rules.at(-1)!.id}"]`,
    );
    await expect(lastRule).toBeVisible();
    await expect(lastRule).toHaveAttribute("data-duplicate-highlighted", "true");
    await expect(lastRule.locator(".pattern-input")).toHaveValue(pattern);
    await expect
      .poll(() =>
        lastRule.evaluate((element) => {
          const box = element.getBoundingClientRect();
          return box.top >= 0 && box.bottom <= innerHeight;
        }),
      )
      .toBe(true);
  });
}
