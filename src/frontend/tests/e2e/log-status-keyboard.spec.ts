import { createServer, type ServerResponse } from "node:http";
import type { AddressInfo } from "node:net";
import { expect, test, type Page } from "@playwright/test";

const frame = (event: string, data: unknown) =>
  `event: ${event}\ndata: ${JSON.stringify(data)}\n\n`;

async function statusBackend(unsupported: boolean) {
  const peers = new Set<ServerResponse>();
  let online = true;
  let dropped = "0";
  const capture = () => ({ enabled: false, session: "", through: "0", dropped });
  const server = createServer((req, res) => {
    res.setHeader("Access-Control-Allow-Origin", "*");
    res.setHeader("Access-Control-Allow-Headers", "Authorization, Content-Type");
    if (req.method === "OPTIONS") {
      res.writeHead(204).end();
      return;
    }
    if (unsupported || !online) {
      res.writeHead(unsupported ? 404 : 503).end();
      return;
    }
    peers.add(res);
    res.on("close", () => peers.delete(res));
    res.writeHead(200, { "Content-Type": "text/event-stream" });
    res.write(frame("capture", capture()));
    res.write(frame("ready", { session: "", through: "0" }));
  });
  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
  return {
    url: `http://127.0.0.1:${(server.address() as AddressInfo).port}`,
    drop(count: string) {
      dropped = count;
      for (const peer of peers) peer.write(frame("capture", capture()));
    },
    disconnect() {
      online = false;
      for (const peer of peers) peer.destroy();
    },
    async close() {
      for (const peer of peers) peer.destroy();
      server.closeAllConnections();
      await new Promise<void>((resolve) => server.close(() => resolve()));
    },
  };
}

async function openLogs(page: Page, url: string, russian: boolean) {
  await page.addInitScript(
    (locale) => {
      localStorage.setItem("locale", JSON.stringify({ value: locale }));
    },
    russian ? "ru" : "en",
  );
  await page.route("**/api/v1/**", async (route) => {
    const request = new URL(route.request().url());
    const path = request.pathname.slice("/api/v1".length);
    if (path.startsWith("/logs/")) {
      await route.continue({ url: `${url}${path}${request.search}` });
      return;
    }
    const fixtures: Record<string, unknown> = {
      "/auth": { enabled: false },
      "/groups": { groups: [] },
      "/subscriptions": { subscriptions: [] },
      "/profiles": { profiles: [] },
      "/system/interfaces": { interfaces: [] },
      "/system/update/status": { stage: "idle" },
    };
    await route.fulfill({ json: fixtures[path.replace(/\/$/, "")] ?? {} });
  });
  await page.goto("/");
  await page.getByRole("tab", { name: russian ? "Настройки" : "Settings", exact: true }).click();
  await page.getByRole("tab", { name: russian ? "Логи" : "Logs", exact: true }).click();
}

for (const russian of [false, true]) {
  for (const scenario of ["disconnect", "unsupported", "dropped"] as const) {
    test(`log ${scenario} explanation is visible from the keyboard (${russian ? "ru" : "en"})`, async ({
      page,
    }) => {
      const api = await statusBackend(scenario === "unsupported");
      try {
        await openLogs(page, api.url, russian);
        const panel = page.locator(".application-logs");
        if (scenario !== "unsupported") {
          await expect(
            panel.getByRole("button", {
              name: russian ? "Начать запись" : "Start log recording",
              exact: true,
            }),
          ).toBeEnabled();
          if (scenario === "disconnect") api.disconnect();
          else api.drop("3");
        }
        const message =
          scenario === "disconnect"
            ? russian
              ? "Соединение потеряно"
              : "Connection lost"
            : scenario === "unsupported"
              ? russian
                ? "Обновите сервис для просмотра логов"
                : "Update the service to view logs"
              : russian
                ? "Не удалось сохранить записей: 3"
                : "Records not saved: 3";
        const trigger = panel.getByRole("button", { name: message, exact: true });
        await expect(trigger).toBeVisible();
        await page.mouse.move(0, 0);
        await panel.getByRole("searchbox").focus();
        await page.keyboard.press("Tab");
        await expect(trigger).toBeFocused();
        const tooltip = page.locator("#global-tooltip");
        await expect(tooltip).toBeVisible();
        await expect(tooltip).toHaveText(message);
        await expect(tooltip).toHaveCSS("opacity", "1");
        expect(
          await trigger.evaluate((element) => getComputedStyle(element).outlineStyle),
        ).not.toBe("none");

        // Leaving with the keyboard dismisses the explanation; returning opens it.
        await page.keyboard.press("Tab");
        await expect(trigger).not.toBeFocused();
        await expect(tooltip).not.toHaveText(message);
        await page.keyboard.press("Shift+Tab");
        await expect(trigger).toBeFocused();
        await expect(tooltip).toBeVisible();
        await expect(tooltip).toHaveText(message);
        await expect(tooltip).toHaveCSS("opacity", "1");

        // Pointer leave must not dismiss keyboard help, and live text stays fresh.
        await trigger.hover();
        await page.mouse.move(0, 0);
        await expect(tooltip).toBeVisible();
        if (scenario === "dropped") {
          api.drop("7");
          await expect(tooltip).toHaveText(
            russian ? "Не удалось сохранить записей: 7" : "Records not saved: 7",
          );
        }
        await page.keyboard.press("Escape");
        await expect(tooltip).toHaveCount(0);
        await expect(panel.locator(".status-trigger")).toBeFocused();
        await expect(panel.locator(".settings-description, .status")).toHaveCount(0);
      } finally {
        await api.close();
      }
    });
  }
}
