import { createServer, type ServerResponse } from "node:http";
import type { AddressInfo } from "node:net";
import { test as base, expect, type Page } from "@playwright/test";

import type { DnsQuery } from "../../src/modules/settings/dns-query-data";

function frame(event: string, data: unknown) {
  return `event: ${event}\ndata: ${JSON.stringify(data)}\n\n`;
}
function backend() {
  const state = { enabled: false, queries: [] as DnsQuery[], through: 0, writes: 0 };
  const clients = new Set<ServerResponse>();
  const status = () => ({
    enabled: state.enabled,
    memory_limit: 1048576,
    memory_used: state.enabled ? 524288 + state.queries.length * 64 : 400,
    count: state.queries.length,
    capacity: 8192,
  });
  const snapshot = (res: ServerResponse) => {
    res.write(frame("capture", status()));
    res.write(
      frame("snapshot_begin", {
        capacity: 8192,
        count: state.queries.length,
        through: state.through,
      }),
    );
    const queries = [...state.queries].reverse();
    for (let offset = 0; offset < queries.length; offset += 32)
      res.write(frame("snapshot_chunk", { offset, queries: queries.slice(offset, offset + 32) }));
    res.write(frame("snapshot_end", { through: state.through }));
  };
  const server = createServer(async (req, res) => {
    res.setHeader("Access-Control-Allow-Origin", "*");
    res.setHeader("Access-Control-Allow-Headers", "Authorization, Content-Type");
    res.setHeader("Access-Control-Allow-Methods", "GET, PUT, DELETE, OPTIONS");
    if (req.method === "OPTIONS") {
      res.writeHead(204).end();
      return;
    }
    if (req.url?.endsWith("/stream")) {
      res.writeHead(200, { "Content-Type": "text/event-stream", "Cache-Control": "no-store" });
      clients.add(res);
      res.on("close", () => clients.delete(res));
      snapshot(res);
      return;
    }
    if (req.method === "PUT" && req.url?.endsWith("/capture")) {
      let body = "";
      for await (const chunk of req) body += chunk;
      state.writes++;
      state.enabled = JSON.parse(body).enabled;
      if (!state.enabled) state.queries = [];
      for (const client of clients) snapshot(client);
    }
    if (req.method === "DELETE") {
      state.queries = [];
      for (const client of clients) client.write(frame("clear", status()));
    }
    res.setHeader("Content-Type", "application/json");
    res.end(JSON.stringify({ ...status(), cleared: true }));
  });
  return {
    state,
    server,
    emit(qtype: number, time: number) {
      if (!state.enabled) return;
      const row: DnsQuery = {
        id: ++state.through,
        time_ms: time,
        client: "192.168.1.20",
        client_name: "Laptop",
        domain: "example.com",
        qtype,
        protocol: "udp",
      };
      state.queries.unshift(row);
      for (const res of clients)
        res.write(frame("query", { ...row, retained_from: 1, memory_used: status().memory_used }));
    },
    close() {
      for (const res of clients) res.destroy();
      server.closeAllConnections();
    },
  };
}
type Backend = ReturnType<typeof backend> & { url: string };
const test = base.extend<{ dns: Backend }>({
  dns: async ({}, use) => {
    const dns = backend();
    await new Promise<void>((resolve) => dns.server.listen(0, "127.0.0.1", resolve));
    const url = `http://127.0.0.1:${(dns.server.address() as AddressInfo).port}`;
    try {
      await use({ ...dns, url });
    } finally {
      dns.close();
      await new Promise<void>((resolve) => dns.server.close(() => resolve()));
    }
  },
});
async function setup(page: Page, dns: Backend) {
  await page.route("**/api/v1/**", async (route) => {
    const path = new URL(route.request().url()).pathname.slice("/api/v1".length);
    if (path.startsWith("/system/dns/")) {
      await route.continue({ url: `${dns.url}${path}` });
      return;
    }
    if (route.request().method() === "OPTIONS") {
      await route.fulfill({ status: 204 });
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
  await settings(page);
  return page.locator(".dns-query-log");
}
async function settings(page: Page) {
  const menu = page.getByRole("button", { name: "Open menu" });
  if (await menu.isVisible()) await menu.click();
  await page.getByRole("tab", { name: "Settings" }).click();
}
async function start(page: Page) {
  await page.getByRole("button", { name: "Start recording", exact: true }).click();
  const dialog = page.getByRole("dialog", { name: "Enable DNS recording?", exact: true });
  await expect(dialog).toContainText("1 MiB");
  await expect(dialog).toContainText("RAM");
  await dialog.getByRole("button", { name: "Start recording", exact: true }).click();
  await expect(dialog).not.toBeVisible();
  await expect(page.getByRole("button", { name: "Stop recording", exact: true })).toBeVisible();
}

test("recording is off by default; cancelling consent never sends PUT", async ({ page, dns }) => {
  const region = await setup(page, dns);
  await expect(region.getByText("DNS recording is off", { exact: true })).toBeVisible();
  dns.emit(1, 1000);
  expect(dns.state.queries).toHaveLength(0);
  await region.getByRole("button", { name: "Start recording" }).click();
  const dialog = page.getByRole("dialog", { name: "Enable DNS recording?", exact: true });
  await expect(dialog).toBeVisible();
  expect(dns.state.writes).toBe(0);
  await dialog.getByRole("button", { name: "Cancel" }).click();
  await expect(dialog).not.toBeVisible();
  await expect(region.getByRole("button", { name: "Start recording" })).toBeFocused();
  await region.getByRole("button", { name: "Start recording" }).click();
  await page.keyboard.press("Escape");
  await expect(dialog).not.toBeVisible();
  expect(dns.state.writes).toBe(0);
  await page.reload();
  await settings(page);
  await expect(region.getByText("DNS recording is off", { exact: true })).toBeVisible();
  expect(dns.state.writes).toBe(0);
});
test("explicit capture combines related DNS types but preserves all raw records", async ({
  page,
  dns,
}) => {
  const region = await setup(page, dns);
  await start(page);
  dns.emit(1, 1000);
  dns.emit(65, 1010);
  dns.emit(28, 1020);
  await expect(region.locator("tbody tr")).toHaveCount(1);
  await expect(region.locator("summary")).toContainText("IPv4 · HTTPS parameters · IPv6");
  await region.locator("summary").click();
  await expect(region.getByText(/HTTPS \(TYPE 65\)/)).toBeVisible();
  expect(dns.state.queries).toHaveLength(3);
  await expect(region.getByText("Stored requests: 3", { exact: true })).toBeVisible();
  await expect(region.getByRole("button", { name: "Combine related queries" })).toHaveCount(0);
  await expect(region.locator(".raw-query")).toHaveCount(3);
  await expect(region.locator(".capture-toolbar")).not.toContainText("1024 / 1024 KiB");
  await expect(
    region.locator(".capture-toolbar").getByRole("button", { name: "History RAM" }),
  ).toHaveCount(0);
  await expect(region.locator(".dns-summary").getByRole("button")).toHaveCount(0);
  await page.reload();
  await settings(page);
  await expect(region.locator("tbody tr")).toHaveCount(1);
  expect(dns.state.writes).toBe(1); // Restoring an enabled session never auto-starts another.
});
test("stop frees history for all viewers and does not resume on reload", async ({
  page,
  context,
  dns,
}) => {
  const region = await setup(page, dns);
  await start(page);
  dns.emit(1, 1000);
  await expect(region.locator("tbody tr")).toHaveCount(1);
  const other = await context.newPage();
  const otherRegion = await setup(other, dns);
  await expect(otherRegion.locator("tbody tr")).toHaveCount(1);
  await region.getByRole("button", { name: "Stop recording" }).click();
  await page
    .getByRole("dialog", { name: "Stop recording?", exact: true })
    .getByRole("button", { name: "Stop recording", exact: true })
    .click();
  await expect(region.getByText("DNS recording is off", { exact: true })).toBeVisible();
  await expect(otherRegion.getByText("DNS recording is off", { exact: true })).toBeVisible();
  expect(dns.state.queries).toHaveLength(0);
  expect(dns.state.enabled).toBe(false);
  dns.emit(65, 1001);
  expect(dns.state.queries).toHaveLength(0);
  await page.reload();
  await settings(page);
  await expect(region.getByText("DNS recording is off", { exact: true })).toBeVisible();
  expect(dns.state.writes).toBe(2);
  await other.close();
});
