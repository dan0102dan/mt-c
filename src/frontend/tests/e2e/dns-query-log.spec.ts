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
  const state = { history: [...sample], through: 3, connections: 0, snapshots: 0, clears: 0, failNext: false, holdEnd: false };
  const clients = new Set<ServerResponse>();
  const pending = new Map<ServerResponse, number>();
  const broadcast = (event: string, data: unknown) => { for (const res of clients) res.write(frame(event, data)); };
  const server = createServer((req, res) => {
    res.setHeader("Access-Control-Allow-Origin", "*");
    res.setHeader("Access-Control-Allow-Headers", "Authorization, Content-Type");
    res.setHeader("Access-Control-Allow-Methods", "GET, DELETE, OPTIONS");
    if (req.method === "OPTIONS") { res.writeHead(204).end(); return; }
    if (req.url?.endsWith("/stream")) {
      state.connections++;
      if (state.failNext) { state.failNext = false; res.writeHead(503).end(); return; }
      res.writeHead(200, { "Content-Type": "text/event-stream", "Cache-Control": "no-store" });
      const queries = [...state.history].reverse();
      res.write(frame("snapshot_begin", { capacity: 1000, count: queries.length, through: state.through }));
      for (let offset = 0; offset < queries.length; offset += 32)
        res.write(frame("snapshot_chunk", { offset, queries: queries.slice(offset, offset + 32) }));
      if (state.holdEnd) pending.set(res, state.through);
      else res.write(frame("snapshot_end", { through: state.through }));
      clients.add(res);
      res.on("close", () => { clients.delete(res); pending.delete(res); });
      return; // A genuinely open HTTP response, not a completed route.fulfill mock.
    }
    res.setHeader("Content-Type", "application/json");
    if (req.method === "DELETE") {
      state.history = []; state.clears++; pending.clear();
      broadcast("clear", {}); res.end(JSON.stringify({ cleared: true }));
    } else {
      state.snapshots++;
      res.end(JSON.stringify({ queries: state.history, capacity: 1000 }));
    }
  });
  return {
    state, server, clients,
    endSnapshot() { for (const [res, through] of pending) res.write(frame("snapshot_end", { through })); pending.clear(); },
    emit(row: DnsQuery) { state.through = row.id; state.history = [row, ...state.history].slice(0, 1000); broadcast("query", row); },
    disconnect() { for (const res of clients) res.destroy(); clients.clear(); pending.clear(); },
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
      backend.disconnect(); backend.server.closeAllConnections();
      await new Promise<void>((resolve) => backend.server.close(() => resolve()));
    }
  },
});
async function setup(page: Page, dns: Backend) {
  await page.route("**/api/v1/**", async (route) => {
    const path = new URL(route.request().url()).pathname.slice("/api/v1".length);
    if (path.startsWith("/system/dns/")) { await route.continue({ url: `${dns.url}${path}` }); return; }
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

function seedHistory(dns: Backend, count: number) {
  dns.state.through = count;
  dns.state.history = Array.from({ length: count }, (_, index) => ({
    ...sample[0], id: count - index, domain: `host${count - index}.example`,
  }));
}

test("one open connection receives immediate updates without 3-second polling", async ({ page, dns }) => {
  const region = await setup(page, dns);
  await expect(region.locator("tbody tr")).toHaveCount(3);
  dns.emit(liveQuery());
  await expect(region.locator("tbody tr")).toHaveCount(4);
  await expect(region).toHaveAttribute("aria-busy", "false");
  await expect(region.getByText("Live", { exact: true })).toHaveCount(0);
  await page.waitForTimeout(3400); // Prove the removed polling interval does not recur.
  expect(dns.state.connections).toBe(1); expect(dns.state.snapshots).toBe(0);
});
test("partial snapshots do not appear until end; reconnect discards unfinished staging", async ({ page, dns }) => {
  dns.state.holdEnd = true;
  const region = await setup(page, dns);
  await expect.poll(() => dns.clients.size).toBe(1);
  await expect(region).toHaveAttribute("aria-busy", "true");
  await expect(region.locator("tbody tr")).toHaveCount(0);
  dns.disconnect();
  dns.state.history = [{ ...sample[0], id: 9, domain: "replacement.example" }]; dns.state.through = 9;
  await expect.poll(() => dns.state.connections).toBe(2);
  await expect.poll(() => dns.clients.size).toBe(1);
  dns.endSnapshot();
  await expect(region.locator("tbody tr")).toHaveCount(1);
  await expect(region.locator("tbody")).toContainText("replacement.example");
  await expect(region).toHaveAttribute("aria-busy", "false");
  expect(dns.state.snapshots).toBe(0);
});
test("DNS journal filters and groups all matching rows", async ({ page, dns }) => {
  const region = await setup(page, dns);
  await expect(region.locator("tbody tr")).toHaveCount(3);
  await region.getByRole("searchbox").fill("EXAMPLE");
  await expect(region.locator("tbody tr")).toHaveCount(2);
  await region.getByRole("combobox", { name: "Device" }).click();
  await page.getByRole("option", { name: "192.168.1.10", exact: true }).click();
  await expect(region.locator("tbody tr")).toHaveCount(1);
  await region.getByRole("combobox", { name: "Device" }).click();
  await page.getByRole("option", { name: "All devices", exact: true }).click();
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
  await expect(region.locator(".dns-summary").getByRole("status")).toHaveText("Paused");
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
  expect(dns.state.clears).toBe(1); expect(dns.state.snapshots).toBe(0);
});
test("DNS streaming controls and long records fit a phone viewport", async ({ page, dns }) => {
  await page.setViewportSize({ width: 390, height: 844 });
  const region = await setup(page, dns);
  await expect(region.locator("tbody tr")).toHaveCount(3);
  await expect(region.getByRole("button", { name: "Group by domain" })).toBeVisible();
  const domain = `${"a".repeat(63)}.${"b".repeat(63)}.${"c".repeat(63)}.example`;
  dns.emit({ ...liveQuery(), domain, client: "2001:db8:1234:5678:abcd:ef01:2345:6789" });
  await expect(region.locator("tbody tr")).toHaveCount(4);
  await expect(region.getByText(domain, { exact: true })).toBeVisible();
  await region.getByRole("combobox", { name: "Device" }).click();
  await expect(page.getByRole("option", { name: "All devices", exact: true })).toBeVisible();
  await page.keyboard.press("Escape");
  await expect(region.getByRole("combobox", { name: "Device" })).toBeFocused();
  expect(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth)).toBe(false);
  await region.getByRole("button", { name: "Group by domain" }).click();
  expect(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth)).toBe(false);
});

for (const width of [1280, 390]) {
  test(`DNS heading joins the shared scroll toolbar without losing filters (${width}px)`, async ({ page, dns }) => {
    await page.setViewportSize({ width, height: 844 });
    await page.emulateMedia({ reducedMotion: "reduce" });
    seedHistory(dns, 90);
    const region = await setup(page, dns);
    await expect(region.locator("tbody tr")).toHaveCount(30);
    const heading = page.locator(".settings-toolbar").getByRole("heading", { level: 2 });
    const inline = region.getByRole("heading", { name: "DNS queries", exact: true });
    const save = page.locator("#save-settings");
    const saveNode = await save.elementHandle();
    await expect(save).toBeDisabled();
    await region.getByRole("searchbox").fill("example");
    await region.getByRole("button", { name: "Group by domain" }).click();
    await expect(region.locator("tbody tr")).toHaveCount(30);
    for (const offset of [36, -8, 44, -16, 36]) {
      await page.evaluate((offset) => {
        const title = document.querySelector("#dns-query-title")!.getBoundingClientRect();
        const toolbar = document.querySelector(".settings-toolbar")!.getBoundingClientRect();
        window.scrollBy(0, title.bottom - toolbar.height - offset);
      }, offset);
      if (offset < 0) await expect(heading).toHaveAccessibleName("DNS queries");
      else await expect(heading).not.toHaveAccessibleName("DNS queries");
      await expect(region.getByRole("searchbox")).toHaveValue("example");
      await expect(region.getByRole("button", { name: "Individual queries" })).toHaveAttribute("aria-pressed", "true");
    }
    await expect(inline).toHaveCSS("position", "static");
    await expect(page.locator(".settings-page .page-controls")).toHaveCount(1);
    await expect(save).toBeDisabled();
    expect(await saveNode!.evaluate((node) => node === document.querySelector("#save-settings"))).toBe(true);
    expect(dns.state.connections).toBe(1);
    expect(dns.state.snapshots).toBe(0);
  });
}

test("shared pagination and empty-search state remain functional", async ({ page, dns }) => {
  seedHistory(dns, 65);
  const region = await setup(page, dns);
  await expect(region.locator("tbody tr")).toHaveCount(30);
  await region.getByRole("button", { name: "Next Page", exact: true }).click();
  await expect(region.locator(".page-number")).toHaveText("2 / 3");
  await expect(region.locator("tbody tr").first()).toContainText("host35.example");
  await region.getByRole("button", { name: "Next Page", exact: true }).click();
  await expect(region.locator("tbody tr")).toHaveCount(5);
  await region.getByRole("searchbox").fill("does-not-exist");
  await expect(region.getByText("No matching DNS queries", { exact: true })).toBeVisible();
  await region.getByRole("searchbox").fill("");
  await expect(region.locator(".page-number")).toHaveText("1 / 3");
  await expect(region.locator("tbody tr")).toHaveCount(30);
});
