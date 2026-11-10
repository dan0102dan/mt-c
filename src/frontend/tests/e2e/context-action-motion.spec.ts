import { expect, test } from "@playwright/test";

for (const width of [1280, 390]) {
  test(`context action keeps a continuous plus-to-dot morph at ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 900 });
    await page.emulateMedia({ reducedMotion: "no-preference" });
    await page.route("https://api.github.com/**", (route) => route.fulfill({ json: [] }));
    await page.route("**/api/v1/**", (route) => {
      const path = new URL(route.request().url()).pathname.slice("/api/v1".length);
      if (path === "/system/dns/stream") {
        const capture = {
          enabled: false,
          memory_limit: 1024 * 1024,
          memory_used: 0,
          count: 0,
          capacity: 1000,
        };
        return route.fulfill({
          contentType: "text/event-stream",
          body: [
            `event: capture\ndata: ${JSON.stringify(capture)}\n\n`,
            'event: snapshot_begin\ndata: {"capacity":1000,"count":0,"through":0}\n\n',
            'event: snapshot_end\ndata: {"through":0}\n\n',
          ].join(""),
        });
      }
      const fixtures: Record<string, unknown> = {
        "/auth": { enabled: false },
        "/groups": { groups: [] },
        "/subscriptions": { subscriptions: [] },
        "/profiles": { profiles: [] },
        "/interfaces": { interfaces: [] },
        "/system/interfaces": { interfaces: [] },
        "/system/update/status": { stage: "idle" },
      };
      return route.fulfill({ json: fixtures[path] ?? {} });
    });
    await page.goto("/");
    const menu = page.getByRole("button", { name: "Open menu" });
    if (await menu.isVisible()) await menu.click();
    await page.getByRole("tab", { name: "Settings", exact: true }).click();
    const profiles = page.getByRole("tab", { name: "Profiles", exact: true });
    const dns = page.getByRole("tab", { name: "DNS queries", exact: true });
    await profiles.click();
    const toolbar = page.locator(".settings-page .context-add");
    const action = toolbar.locator("button");
    const arm = action.locator(".horizontal");
    await expect(action).toBeEnabled();
    await expect(toolbar).not.toHaveClass(/resetting/);
    await expect.poll(() => arm.evaluate((node) => getComputedStyle(node).width)).toBe("10px");

    // Slow only this icon so a busy CI browser can reliably observe an
    // intermediate frame. Reverse via the actual tab handlers, not CSS classes.
    await action.locator("svg").evaluate((svg) => {
      svg.style.setProperty("--morph-duration", "1000ms");
    });
    const sampled = await action.evaluate(
      async (button, tabs) => {
        if (!(tabs.dns instanceof HTMLElement) || !(tabs.profiles instanceof HTMLElement))
          throw new Error("Missing settings tabs");
        const svg = button.querySelector("svg")!;
        const bar = svg.querySelector(".horizontal")!;
        const symbol = svg.querySelector(".symbol")!;
        const readWidth = () => parseFloat(getComputedStyle(bar).width);
        const frame = () => new Promise<void>((resolve) => requestAnimationFrame(() => resolve()));
        tabs.dns.click();
        let middle = readWidth();
        const deadline = performance.now() + 2000;
        while ((middle >= 9 || middle <= 4) && performance.now() < deadline) {
          await frame();
          middle = readWidth();
        }
        const opacity = Number(getComputedStyle(symbol).opacity);
        const animations = svg.getAnimations({ subtree: true });
        tabs.profiles.click();
        while (svg.classList.contains("record") && performance.now() < deadline) await frame();
        return {
          middle,
          opacity,
          reversed: readWidth(),
          resetting: button.closest(".context-add")!.classList.contains("resetting"),
          sameNode: bar === button.querySelector(".horizontal"),
          hasBounce: animations.some((animation) => animation instanceof CSSAnimation),
        };
      },
      { dns: await dns.elementHandle(), profiles: await profiles.elementHandle() },
    );
    expect(sampled.middle).toBeGreaterThan(4);
    expect(sampled.middle).toBeLessThan(9);
    expect(sampled.opacity).toBe(1);
    expect(Math.abs(sampled.reversed - sampled.middle)).toBeLessThan(0.5);
    expect(sampled.resetting).toBe(false);
    expect(sampled.sameNode).toBe(true);
    expect(sampled.hasBounce).toBe(true);
    await expect.poll(() => arm.evaluate((node) => getComputedStyle(node).width)).toBe("10px");

    await dns.click();
    await expect.poll(() => arm.evaluate((node) => getComputedStyle(node).width)).toBe("4px");
    await expect.poll(() => arm.evaluate((node) => getComputedStyle(node).height)).toBe("4px");
    await expect(action).toHaveAttribute("aria-label", "Start recording");
    await expect
      .poll(() => action.locator(".symbol").evaluate((node) => getComputedStyle(node).animationName))
      .toContain("dot-bounce");
    await expect(action).toBeEnabled();

    await page.emulateMedia({ reducedMotion: "reduce" });
    await expect
      .poll(() => action.locator(".symbol").evaluate((node) => getComputedStyle(node).animationName))
      .toBe("none");
    await profiles.click();
    await expect.poll(() => arm.evaluate((node) => getComputedStyle(node).width)).toBe("10px");
    const durations = await action.locator(".bar, .symbol, .outline").evaluateAll((nodes) =>
      nodes.map((node) => getComputedStyle(node).transitionDuration),
    );
    expect(durations.every((duration) => duration === "0s")).toBe(true);
    expect(
      await action.locator("svg").evaluate((svg) => svg.getAnimations({ subtree: true }).length),
    ).toBe(0);
  });
}
