import { expect, test, type Page } from "@playwright/test";

async function expectNativeScrolling(page: Page) {
  await expect(page.locator(".overlay-scrollbar")).toHaveCount(0);
  const styles = await page.evaluate(() => {
    const root = document.documentElement;
    const style = getComputedStyle(root);
    return {
      overflow: style.overflowY,
      scrollbar: style.scrollbarWidth,
      webkitScrollbar: getComputedStyle(root, "::-webkit-scrollbar").display,
    };
  });
  expect(styles.overflow).not.toBe("hidden");
  expect(styles.scrollbar).not.toBe("none");
  expect(styles.webkitScrollbar).not.toBe("none");
}

async function expectDocumentEdge(page: Page, edge: "start" | "end") {
  await expect
    .poll(() =>
      page.evaluate(
        (edge) =>
          edge === "start"
            ? scrollY
            : document.documentElement.scrollHeight - innerHeight - scrollY,
        edge,
      ),
    )
    .toBeLessThan(1);
}

for (const viewport of [
  { width: 1280, height: 900 },
  { width: 390, height: 844 },
]) {
  for (const tab of ["Groups", "Subscriptions", "Settings"]) {
    test(`${tab}: native document scrolling retains the sticky toolbar at ${viewport.width}px`, async ({
      page,
    }) => {
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
            : '.settings-page .settings-content[data-state="active"]';
      await expect(page.locator(content)).toBeVisible();
      await expectNativeScrolling(page);
      const toolbar = page.locator(
        tab === "Settings"
          ? ".settings-toolbar"
          : '.app-tab-content[data-state="active"] .page-controls',
      );
      await page.mouse.move(viewport.width / 2, viewport.height / 2);
      await page.mouse.wheel(0, 500);
      await expect.poll(() => page.evaluate(() => scrollY)).toBeGreaterThan(100);
      const toolbarAfter = await toolbar.boundingBox();
      expect(toolbarAfter).not.toBeNull();
      expect(Math.abs(toolbarAfter!.y)).toBeLessThan(1);

      // The browser owns document-edge keys as well as wheel input. A virtual
      // range change must not leave the last panel beyond the requested edge.
      await page.evaluate(() => (document.activeElement as HTMLElement | null)?.blur());
      await page.keyboard.press("End");
      await expectDocumentEdge(page, "end");
      await page.keyboard.press("Home");
      await expectDocumentEdge(page, "start");

      if (viewport.width < 700 && tab !== "Settings") {
        const search = page.locator('[data-tabs-content][data-state="active"] [data-page-search]');
        await search.focus();
        await expectNativeScrolling(page);
        await search.blur();
      }
      if (tab === "Groups") {
        const list = page.locator(content);
        const before = await list.boundingBox();
        expect(before).not.toBeNull();
        async function expectUnshiftedContent() {
          await expect
            .poll(async () => {
              const box = await list.boundingBox();
              return box
                ? Math.max(Math.abs(box.x - before!.x), Math.abs(box.width - before!.width))
                : Infinity;
            })
            .toBeLessThan(1);
        }
        const route = page.locator(".group-header [data-select-trigger]").first();
        await route.click();
        await expect(page.getByRole("listbox")).toBeVisible();
        await expectUnshiftedContent();
        await page.keyboard.press("Escape");
        await expect(page.getByRole("listbox")).toHaveCount(0);
        await expect(route).toBeFocused();
        await expectUnshiftedContent();

        const info = page.getByRole("button", { name: "About", exact: true });
        await info.click();
        const dialog = page.getByRole("dialog", { name: "About", exact: true });
        await expect(dialog).toBeVisible();
        await expectUnshiftedContent();
        await page.keyboard.press("Escape");
        await expect(dialog).toBeHidden();
        await expectUnshiftedContent();
        await expectNativeScrolling(page);
      }
      if (tab === "Settings") {
        // A short subtab releases the scroll offset, without reclaiming the
        // scrollbar gutter and shifting the toolbar's controls sideways.
        const before = await toolbar.boundingBox();
        await page.getByRole("tab", { name: "DNS queries", exact: true }).click();
        await expectDocumentEdge(page, "start");
        await expectNativeScrolling(page);
        const after = await toolbar.boundingBox();
        expect(Math.abs(after!.width - before!.width)).toBeLessThan(1);
        await page.getByRole("tab", { name: "Profiles", exact: true }).click();
        await expect(page.locator(content)).toBeVisible();
        await expectNativeScrolling(page);
      }
    });
  }
}
