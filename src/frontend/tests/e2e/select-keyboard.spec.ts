import { expect, test } from "@playwright/test";

for (const width of [1280, 390]) {
  test(`Select preserves selected highlight, keyboard choice and Escape on lazy open at ${width}px`, async ({
    page,
  }) => {
    await page.setViewportSize({ width, height: width === 390 ? 844 : 900 });
    const groups = Array.from({ length: 25 }, (_, index) => ({
      id: (0x10000000 + index).toString(16),
      name: `Keyboard group ${index + 1}`,
      color: "#42bdf9",
      interface: "eth2",
      enable: true,
      priority: 300,
      rules: [],
    }));
    await page.route("**/api/v1/**", (route) => {
      const path = new URL(route.request().url()).pathname.replace("/api/v1", "");
      const fixtures: Record<string, unknown> = {
        "/auth": { enabled: false },
        "/groups": { groups },
        "/subscriptions": { subscriptions: [] },
        "/profiles": { profiles: [] },
        "/system/interfaces": {
          interfaces: Array.from({ length: 5 }, (_, index) => ({
            id: `eth${index}`,
            name: `Network ${index}`,
          })),
        },
        "/system/update/status": { stage: "idle" },
        "/system/update": { installed_version: "test", can_install: false },
      };
      return route.fulfill({ json: fixtures[path] ?? {} });
    });
    await page.route("https://api.github.com/**", (route) => route.fulfill({ json: [] }));
    await page.goto("/");
    const trigger = page.locator(".group-header [data-select-trigger]").first();
    await expect(trigger).toContainText("eth2");
    await trigger.focus();
    await trigger.press("ArrowDown");
    const list = page.getByRole("listbox");
    await expect(list).toBeVisible();
    await expect(trigger).toBeFocused();
    const selected = page.getByRole("option", { name: "eth2 Network 2", exact: true });
    await expect
      .poll(
        async () =>
          (await trigger.getAttribute("aria-activedescendant")) ===
          (await selected.getAttribute("id")),
      )
      .toBe(true);
    await trigger.press("ArrowDown");
    await trigger.press("Enter");
    await expect(list).toHaveCount(0);
    await expect(trigger).toContainText("eth3");
    await expect(trigger).toBeFocused();

    const documentTop = await page.evaluate(() => scrollY);
    await trigger.press("Enter");
    await expect(list).toBeVisible();
    await trigger.press("End");
    const last = page.getByRole("option", { name: "eth4 Network 4", exact: true });
    await expect
      .poll(
        async () =>
          (await trigger.getAttribute("aria-activedescendant")) === (await last.getAttribute("id")),
      )
      .toBe(true);
    expect(await page.evaluate(() => scrollY)).toBe(documentTop);
    await trigger.press("Escape");
    await expect(list).toHaveCount(0);
    await expect(trigger).toContainText("eth3");
    await expect(trigger).toBeFocused();
  });
}
