import { createServer, type ServerResponse } from "node:http";
import type { AddressInfo } from "node:net";
import { test as base, expect, type Locator, type Page } from "@playwright/test";

const frame = (event: string, data: unknown) =>
  `event: ${event}\ndata: ${JSON.stringify(data)}\n\n`;

// Keep enough real table content below the section boundary to scroll past it.
const test = base.extend<{ dnsUrl: string }>({
  dnsUrl: async ({}, use) => {
    const clients = new Set<ServerResponse>();
    const server = createServer((req, res) => {
      res.setHeader("Access-Control-Allow-Origin", "*");
      res.setHeader("Access-Control-Allow-Headers", "Authorization, Content-Type");
      if (req.method === "OPTIONS") {
        res.writeHead(204).end();
        return;
      }
      res.writeHead(200, { "Content-Type": "text/event-stream" });
      const queries = Array.from({ length: 40 }, (_, i) => ({
        id: i + 1,
        time_ms: 1000 * (i + 1),
        client: "192.168.1.10",
        domain: `host${i}.example`,
        qtype: 1,
        protocol: "udp",
      }));
      res.write(frame("snapshot_begin", { capacity: 1000, count: 40, through: 40 }));
      for (let offset = 0; offset < queries.length; offset += 32)
        res.write(frame("snapshot_chunk", { offset, queries: queries.slice(offset, offset + 32) }));
      res.write(frame("snapshot_end", { through: 40 }));
      clients.add(res);
      res.on("close", () => clients.delete(res));
    });
    await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
    try {
      await use(`http://127.0.0.1:${(server.address() as AddressInfo).port}`);
    } finally {
      for (const client of clients) client.destroy();
      server.closeAllConnections();
      await new Promise<void>((resolve) => server.close(() => resolve()));
    }
  },
});

async function setup(page: Page, dnsUrl: string, language: "en" | "ru") {
  await page.addInitScript(
    (value) => localStorage.setItem("locale", JSON.stringify({ value })),
    language,
  );
  const writes: string[] = [];
  await page.route("**/api/v1/**", async (route) => {
    const request = route.request();
    const path = new URL(request.url()).pathname.replace("/api/v1", "").replace(/\/$/, "");
    if (request.method() === "OPTIONS") {
      await route.fulfill({ status: 204 });
      return;
    }
    if (request.method() !== "GET") writes.push(path);
    if (path === "/system/dns/stream") {
      await route.continue({ url: `${dnsUrl}${path}` });
      return;
    }
    const fixtures: Record<string, unknown> = {
      "/auth": { enabled: false },
      "/groups": { groups: [] },
      "/subscriptions": { subscriptions: [] },
      "/profiles": {
        profiles: Array.from({ length: 6 }, (_, i) => ({
          id: `profile${i}`,
          name: `VPN ${i + 1}`,
          interfaces: ["tun0"],
          on_unavailable: "blackhole",
        })),
      },
      "/system/interfaces": { interfaces: [{ id: "blackhole" }, { id: "tun0" }] },
      "/system/update/status": { stage: "idle" },
      "/keenetic/policy": {
        settings: { enabled: false, policy_id: "", mode: "include" },
        runtime: { state: "disabled" },
      },
      "/keenetic/policies": { policies: [] },
    };
    await route.fulfill({ json: fixtures[path] ?? {} });
  });
  await page.goto("/");
  // The mobile menu toggle has a localized name; its existing stable class
  // also works when this regression is run in Russian.
  const menu = page.locator(".mobile-dropdown-btn");
  if (await menu.isVisible()) await menu.click();
  await page
    .getByRole("tab", { name: language === "ru" ? "Настройки" : "Settings", exact: true })
    .click();
  await expect(page.locator(".dns-query-log tbody tr")).toHaveCount(30);
  return writes;
}

async function expectNaturalHeight(toolbar: Locator) {
  await expect(toolbar.locator(".title-layer.current")).toHaveCSS("opacity", "1");
  await expect
    .poll(() =>
      toolbar.evaluate((node) => {
        const title = node.querySelector<HTMLElement>(".settings-title")!;
        const active = node.querySelector<HTMLElement>(".title-layer.current")!;
        const actions = node.querySelector<HTMLElement>(".page-controls-actions")!;
        const text = document.createRange();
        text.selectNodeContents(active);
        const lineCount = new Set(Array.from(text.getClientRects(), (rect) => Math.round(rect.top)))
          .size;
        const lineHeight = parseFloat(getComputedStyle(title).lineHeight);
        // Check painted text lines, not the height of a stretched grid item:
        // the old hidden multiline title incorrectly enlarged that item as well.
        const expected = Math.max(
          lineCount * lineHeight,
          parseFloat(getComputedStyle(title).minHeight) || 0,
          actions.getBoundingClientRect().height,
        );
        return Math.abs(title.getBoundingClientRect().height - expected);
      }),
    )
    .toBeLessThan(1);
}

async function placeDnsBoundary(page: Page, offset: number) {
  await page.evaluate((offset) => {
    const toolbar = document.querySelector(".settings-toolbar")!;
    const title = toolbar.querySelector(".settings-title")!;
    const tallest = Math.max(
      ...Array.from(
        toolbar.querySelectorAll<HTMLElement>(".title-layer"),
        (layer) => layer.scrollHeight,
      ),
    );
    const line = Math.ceil(
      toolbar.getBoundingClientRect().height +
        Math.max(0, tallest - title.getBoundingClientRect().height),
    );
    const anchor = document.querySelector("#dns-query-title")!.getBoundingClientRect();
    window.scrollBy(0, anchor.bottom - line - offset);
  }, offset);
}

for (const width of [320, 390, 1280]) {
  for (const language of ["en", "ru"] as const) {
    test(`settings header uses only current text height (${width}px, ${language})`, async ({
      page,
      dnsUrl,
    }) => {
      await page.setViewportSize({ width, height: 844 });
      if (language === "ru") await page.emulateMedia({ reducedMotion: "reduce" });
      const writes = await setup(page, dnsUrl, language);
      const toolbar = page.locator(".settings-toolbar");
      const heading = toolbar.getByRole("heading", { level: 2 });
      const profiles =
        language === "ru" ? "Профили приоритета интерфейсов" : "Interface priority profiles";
      const dns = language === "ru" ? "DNS-запросы" : "DNS queries";
      await expect(heading).toHaveAccessibleName(profiles);
      await expectNaturalHeight(toolbar);
      const tall = (await toolbar.boundingBox())!.height;
      const save = page.locator("#save-settings");
      const saveNode = await save.elementHandle();
      const name = page
        .getByPlaceholder(language === "ru" ? "Название профиля" : "Profile name", { exact: true })
        .first();
      await name.fill("Unsaved header regression");
      await expect(save).toBeEnabled();

      await placeDnsBoundary(page, -4);
      await expect(heading).toHaveAccessibleName(dns);
      await expectNaturalHeight(toolbar);
      if (width === 390) expect((await toolbar.boundingBox())!.height).toBeLessThan(tall - 5);
      await expect(save).toBeInViewport();
      expect(
        await saveNode!.evaluate((node) => node === document.querySelector("#save-settings")),
      ).toBe(true);

      // Sample two fade durations at rest: a ResizeObserver/scroll-anchor
      // feedback loop must not alternate the active header without scrolling.
      const labels = await heading.evaluate(async (node) => {
        const labels: (string | null)[] = [];
        const until = performance.now() + 500;
        while (performance.now() < until) {
          await new Promise<void>((resolve) => requestAnimationFrame(() => resolve()));
          labels.push(node.getAttribute("aria-label"));
        }
        return [...new Set(labels)];
      });
      expect(labels).toEqual([dns]);
      for (const offset of [36, -4, 36, -4]) {
        await placeDnsBoundary(page, offset);
        // Generic has profiles -> DNS. Keenetic's intermediate policy section
        // is covered by its existing dedicated scroll-transition tests.
        await expect(heading).toHaveAccessibleName(
          offset < 0
            ? dns
            : process.env.VITE_ENTWARE_KN === "1"
              ? language === "ru"
                ? "Политика Keenetic"
                : "Keenetic policy"
              : profiles,
        );
        await expectNaturalHeight(toolbar);
      }
      await page.setViewportSize({ width: width === 1280 ? 390 : 1280, height: 844 });
      await placeDnsBoundary(page, -40);
      await expect(heading).toHaveAccessibleName(dns);
      await expectNaturalHeight(toolbar);
      await page.setViewportSize({ width, height: 844 });
      await page.evaluate(() => window.scrollTo(0, 0));
      await expect(heading).toHaveAccessibleName(profiles);
      await expectNaturalHeight(toolbar);
      await expect(name).toHaveValue("Unsaved header regression");
      await expect(save).toBeEnabled();
      expect(writes).toEqual([]);
      expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBeLessThanOrEqual(
        width,
      );
    });
  }
}
