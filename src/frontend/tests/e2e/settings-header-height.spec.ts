import { createServer, type ServerResponse } from "node:http";
import type { AddressInfo } from "node:net";
import { test as base, expect, type Page } from "@playwright/test";

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
  return writes;
}

for (const width of [320, 390, 1280]) {
  for (const language of ["en", "ru"] as const) {
    test(`settings tabs move actions together (${width}px, ${language})`, async ({
      page,
      dnsUrl,
    }) => {
      await page.setViewportSize({ width, height: 844 });
      if (language === "ru") await page.emulateMedia({ reducedMotion: "reduce" });
      const writes = await setup(page, dnsUrl, language);
      const toolbar = page.locator(".settings-toolbar");
      await expect(toolbar.getByRole("heading")).toHaveCount(0);
      const profiles = language === "ru" ? "Профили" : "Profiles";
      const dns = language === "ru" ? "DNS-запросы" : "DNS queries";
      const labels = [
        profiles,
        ...(process.env.VITE_ENTWARE_KN === "1" ? ["Keenetic"] : []),
        dns,
        profiles,
      ];
      const name = page
        .getByPlaceholder(language === "ru" ? "Название профиля" : "Profile name", { exact: true })
        .first();
      await name.fill("Unsaved header regression");
      const save = page.locator("#save-settings");
      const saveNode = await save.elementHandle();
      const actions = toolbar.locator(".page-controls-actions");
      const positions = await actions.getByRole("button").evaluateAll((nodes) =>
        nodes.slice(0, 3).map((node) => {
          const { x, y } = node.getBoundingClientRect();
          return { x, y };
        }),
      );
      const shift = await toolbar
        .locator(".context-add")
        .evaluate(
          (node) =>
            node.getBoundingClientRect().width +
            parseFloat(getComputedStyle(node.parentElement!).gap),
        );
      const initialHeight = (await toolbar.boundingBox())!.height;
      for (const label of labels) {
        await page.getByRole("tab", { name: label, exact: true }).click();
        await expect(page.getByRole("tab", { name: label, exact: true })).toHaveAttribute(
          "aria-selected",
          "true",
        );
        await expect(actions.getByRole("button")).toHaveCount(label === profiles ? 4 : 3);
        await expect(save).toBeEnabled();
        await expect(save).toBeInViewport();
        const offset = label === profiles ? 0 : shift;
        await expect
          .poll(async () => Math.abs((await save.boundingBox())!.x - positions[0].x - offset))
          .toBeLessThan(1);
        const current = await actions.getByRole("button").evaluateAll((nodes) =>
          nodes.slice(0, 3).map((node) => {
            const { x, y } = node.getBoundingClientRect();
            return { x, y };
          }),
        );
        current.forEach((position, i) => {
          expect(Math.abs(position.x - positions[i].x - offset)).toBeLessThan(1);
          expect(Math.abs(position.y - positions[i].y)).toBeLessThan(1);
        });
        expect(Math.abs((await toolbar.boundingBox())!.height - initialHeight)).toBeLessThan(1);
        const description = page.locator(
          '.settings-content[data-state="active"] .settings-description',
        );
        await expect(description).toBeVisible();
        expect((await description.innerText()).length).toBeGreaterThan(30);
        expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBeLessThanOrEqual(
          width,
        );
      }
      await expect(name).toHaveValue("Unsaved header regression");
      expect(
        await saveNode!.evaluate((node) => node === document.querySelector("#save-settings")),
      ).toBe(true);
      expect(writes).toEqual([]);
    });
  }
}

test("Add slides right and fades while the other actions move with it", async ({
  page,
  dnsUrl,
}) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await page.emulateMedia({ reducedMotion: "no-preference" });
  await setup(page, dnsUrl, "en");
  await expect(page.getByRole("button", { name: "Add profile", exact: true })).toBeVisible();
  const [frames] = await Promise.all([
    page.evaluate(async () => {
      const add = document.querySelector<HTMLElement>(
        '.app-tab-content[data-state="active"] .context-add-motion',
      )!;
      const save = document.querySelector("#save-settings")!;
      const frames: { opacity: number; x: number; saveX: number; width: number }[] = [];
      const until = performance.now() + 450;
      while (performance.now() < until) {
        await new Promise<void>((resolve) => requestAnimationFrame(() => resolve()));
        frames.push({
          opacity: Number(getComputedStyle(add).opacity),
          x: add.getBoundingClientRect().x,
          saveX: save.getBoundingClientRect().x,
          width: document.documentElement.scrollWidth,
        });
      }
      return frames;
    }),
    page.getByRole("tab", { name: "DNS queries", exact: true }).click(),
  ]);
  expect(frames.some((frame) => frame.opacity > 0 && frame.opacity < 1)).toBe(true);
  expect(frames.at(-1)!.opacity).toBe(0);
  expect(frames.at(-1)!.x - frames[0].x).toBeGreaterThan(10);
  expect(frames.at(-1)!.saveX - frames[0].saveX).toBeGreaterThan(40);
  expect(
    frames.some(
      (frame) => frame.saveX > frames[0].saveX + 2 && frame.saveX < frames.at(-1)!.saveX - 2,
    ),
  ).toBe(true);
  for (const frame of frames) {
    expect(frame.saveX).toBeGreaterThanOrEqual(frames[0].saveX - 1);
    expect(frame.width).toBeLessThanOrEqual(390);
  }
});

for (const width of [390, 1280]) {
  for (const section of ["Groups", "Subscriptions"]) {
    test(`Add transition follows ${section} and Settings (${width}px)`, async ({
      page,
      dnsUrl,
    }) => {
      await page.setViewportSize({ width, height: 844 });
      await page.emulateMedia({ reducedMotion: "no-preference" });
      await setup(page, dnsUrl, "en");
      await page.getByRole("tab", { name: "DNS queries", exact: true }).click();
      const activeRow = page.locator(
        '.app-tab-content[data-state="active"] .page-controls-actions',
      );
      await expect(activeRow.locator(".context-add-motion")).toHaveCSS("opacity", "0");

      async function switchSection(name: string) {
        const menu = page.getByRole("button", { name: "Open menu", exact: true });
        if (await menu.isVisible()) await menu.click();
        const [frames] = await Promise.all([
          page.evaluate(async () => {
            const frames: { x: number; opacity: number; width: number }[] = [];
            const until = performance.now() + 450;
            while (performance.now() < until) {
              await new Promise<void>((resolve) => requestAnimationFrame(() => resolve()));
              const row = document.querySelector(
                '.app-tab-content[data-state="active"] .page-controls-actions',
              )!;
              frames.push({
                x: row.querySelector("button")!.getBoundingClientRect().x,
                opacity: Number(
                  getComputedStyle(row.querySelector(".context-add-motion")!).opacity,
                ),
                width: document.documentElement.scrollWidth,
              });
            }
            return frames;
          }),
          page.getByRole("tab", { name, exact: true }).click(),
        ]);
        expect(frames.some((frame) => frame.opacity > 0 && frame.opacity < 1)).toBe(true);
        expect(
          frames.some(
            (frame) =>
              Math.abs(frame.x - frames[0].x) > 2 && Math.abs(frame.x - frames.at(-1)!.x) > 2,
          ),
        ).toBe(true);
        for (const frame of frames) expect(frame.width).toBeLessThanOrEqual(width);
        return frames;
      }

      const entering = await switchSection(section);
      expect(entering.at(-1)!.opacity).toBe(1);
      expect(entering.at(-1)!.x - entering[0].x).toBeLessThan(-40);
      const leaving = await switchSection("Settings");
      expect(leaving.at(-1)!.opacity).toBe(0);
      expect(leaving.at(-1)!.x - leaving[0].x).toBeGreaterThan(40);
      await expect(page.getByRole("tab", { name: "DNS queries", exact: true })).toHaveAttribute(
        "aria-selected",
        "true",
      );
    });
  }
}
