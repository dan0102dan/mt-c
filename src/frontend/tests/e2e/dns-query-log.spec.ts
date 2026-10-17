import { expect, test as base, type Page } from "@playwright/test";
import { createServer, type ServerResponse } from "node:http";
import type { AddressInfo } from "node:net";
import type { DnsQuery } from "../../src/modules/settings/dns-query-data";

const sample: DnsQuery[] = [
  { id: 3, time_ms: 3000, client: "192.168.1.10", domain: "example.com", qtype: 1, protocol: "udp" },
  { id: 2, time_ms: 2000, client: "192.168.1.11", domain: "example.com", qtype: 28, protocol: "tcp" },
  { id: 1, time_ms: 1000, client: "192.168.1.10", domain: "another.net", qtype: 65, protocol: "udp" },
];
function frame(event: string, data: unknown) { return `event: ${event}\ndata: ${JSON.stringify(data)}\n\n`; }

function createBackend() {
  const state = { history: [...sample], connections: 0, snapshots: 0, clears: 0, failNext: false };
  const clients = new Set<ServerResponse>();
  const broadcast = (event: string, data: unknown) => {
    for (const res of clients) res.write(frame(event, data));
  };
  const server = createServer((req, res) => {
    res.setHeader("Access-Control-Allow-Origin", "*");
    res.setHeader("Access-Control-Allow-Headers", "Authorization, Content-Type");
    res.setHeader("Access-Control-Allow-Methods", "GET, DELETE, OPTIONS");
    if (req.method === "OPTIONS") { res.writeHead(204).end(); return; }
    if (req.url?.endsWith("/stream")) {
      state.connections++;
      if (state.failNext) { state.failNext = false; res.writeHead(503).end(); return; }
      res.writeHead(200, { "Content-Type": "text/event-stream", "Cache-Control": "no-store" });
      res.write(frame("snapshot", { queries: state.history, capacity: 1000 }));
      clients.add(res);
      res.on("close", () => clients.delete(res));
      return; // A genuinely open HTTP response, not a completed route.fulfill mock.
    }
    res.setHeader("Content-Type", "application/json");
    if (req.method === "DELETE") {
      state.history = []; state.clears++;
      broadcast("clear", {}); res.end(JSON.stringify({ cleared: true }));
    } else {
      state.snapshots++;
      res.end(JSON.stringify({ queries: state.history, capacity: 1000 }));
    }
  });
  return {
    state, server, clients,
    emit(row: DnsQuery) { state.history = [row, ...state.history].slice(0, 1000); broadcast("query", row); },
    disconnect() { for (const res of clients) res.destroy(); clients.clear(); },
  };
}

type Backend = ReturnType<typeof createBackend> & { url: string };
const test = base.extend<{ dns: Backend }>({
  dns: async ({}, use) => {
    const backend = createBackend();
    await new Promise<void>((resolve) => backend.server.listen(0, "127.0.0.1", resolve));
    const url = `http://127.0.0.1:${(backend.server.address() as AddressInfo).port}`;
    try { await use({ ...backend, url }); }
    finally {
      backend.disconnect();
      backend.server.closeAllConnections();
      await new Promise<void>((resolve) => backend.server.close(() => resolve()));
    }
  },
});

async function setup(page: Page, dns: Backend) {
  await page.route("**/api/v1/**", async (route) => {
    const path = new URL(route.request().url()).pathname.slice("/api/v1".length);
    if (path.startsWith("/system/dns/")) {
      await route.continue({ url: `${dns.url}${path}` }); return;
    }
    if (route.request().method() === "OPTIONS") { await route.fulfill({ status: 204 }); return; }
    const fixtures: Record<string, unknown> = {
      "/auth": { enabled: false }, "/groups": { groups: [] },
      "/subscriptions": { subscriptions: [] }, "/profiles": { profiles: [] },
      "/system/interfaces": { interfaces: [] }, "/system/update/status": { stage: "idle" },
    };
    await route.fulfill({ json: fixtures[path.replace(/\/$/, "")] ?? {} });
  });
  await page.goto("/");
  const menu = page.getByRole("button", { name: "Open menu" });
  if (await menu.isVisible()) await menu.click();
  await page.getByRole("tab", { name: "Settings" }).click();
  const region = page.locator(".dns-query-log");
  await expect(region).toBeVisible();
  return region;
}

const liveQuery = (): DnsQuery => ({ ...sample[0], id: 4, domain: "live.example", time_ms: 4000 });

test("one open connection receives immediate updates without 3-second polling", async ({ page, dns }) => {
  const region = await setup(page, dns);
  await expect(region.locator("tbody tr")).toHaveCount(3);
  dns.emit(liveQuery());
  await expect(region.locator("tbody tr")).toHaveCount(4);
  await expect(region.getByRole("status")).toHaveText("Live");
  await page.waitForTimeout(3400); // Explicitly prove the removed polling interval does not recur.
  expect(dns.state.connections).toBe(1);
  expect(dns.state.snapshots).toBe(0);
});

test("DNS journal filters and groups all matching rows", async ({ page, dns }) => {
  const region = await setup(page, dns);
  await expect(region.locator("tbody tr")).toHaveCount(3);
  await region.getByRole("searchbox").fill("EXAMPLE");
  await expect(region.locator("tbody tr")).toHaveCount(2);
  await region.getByLabel("Device").selectOption("192.168.1.10");
  await expect(region.locator("tbody tr")).toHaveCount(1);
  await region.getByLabel("Device").selectOption("");
  await region.getByRole("searchbox").fill("");
  await region.getByRole("button", { name: "Group by domain" }).click();
  await expect(region.locator("tbody tr")).toHaveCount(2);
  await expect(region.locator("tbody tr").first()).toContainText("192.168.1.11");
});

test("pause closes the subscription; resume and reconnect replace the snapshot", async ({ page, dns }) => {
  const region = await setup(page, dns);
  await expect(region.locator("tbody tr")).toHaveCount(3);
  await region.getByRole("button", { name: "Pause", exact: true }).click();
  await expect.poll(() => dns.clients.size).toBe(0);
  dns.emit(liveQuery());
  await expect(region.locator("tbody tr")).toHaveCount(3);
  await region.getByRole("button", { name: "Resume", exact: true }).click();
  await expect(region.locator("tbody tr")).toHaveCount(4);
  dns.disconnect();
  await expect.poll(() => dns.state.connections).toBe(3);
  await expect(region.locator("tbody tr")).toHaveCount(4);
});

test("clear broadcasts and a failed connection retries without a snapshot GET", async ({ page, dns }) => {
  dns.state.failNext = true;
  const region = await setup(page, dns);
  await expect(region.locator("tbody tr")).toHaveCount(3);
  expect(dns.state.connections).toBe(2);
  page.once("dialog", (dialog) => dialog.accept());
  await region.getByRole("button", { name: "Clear history" }).click();
  await expect(region.getByText("No DNS queries captured yet")).toBeVisible();
  expect(dns.state.clears).toBe(1);
  expect(dns.state.snapshots).toBe(0);
});

test("DNS streaming controls fit a phone viewport", async ({ page, dns }) => {
  await page.setViewportSize({ width: 390, height: 844 });
  const region = await setup(page, dns);
  await expect(region.locator("tbody tr")).toHaveCount(3);
  await expect(region.getByRole("button", { name: "Group by domain" })).toBeVisible();
  expect(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth)).toBe(false);
});
