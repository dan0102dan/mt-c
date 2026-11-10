import { createServer, type ServerResponse } from "node:http";
import type { AddressInfo } from "node:net";
import { test as base, expect, type Page } from "@playwright/test";

import type { DnsQuery } from "../../src/modules/settings/dns/data";

const frame = (event: string, data: unknown) =>
  `event: ${event}\ndata: ${JSON.stringify(data)}\n\n`;
function createBackend() {
  const state = {
    manualGets: 0,
    snapshots: 0,
    history: [
      {
        id: 3,
        time_ms: 3000,
        client: "::ffff:192.168.1.42",
        client_name: "Phone",
        domain: "example.org",
        qtype: 1,
        protocol: "udp",
      },
      {
        id: 2,
        time_ms: 2000,
        client: "192.168.1.43",
        client_name: "Phone",
        domain: "example.org",
        qtype: 28,
        protocol: "udp",
      },
      {
        id: 1,
        time_ms: 1000,
        client: "2001:db8::7",
        domain: "other.example",
        qtype: 65,
        protocol: "tcp",
      },
    ] as DnsQuery[],
  };
  const streams = new Set<ServerResponse>();
  function snapshot(res: ServerResponse) {
    state.snapshots++;
    const rows = [...state.history].reverse().map(({ client_name: _name, ...row }) => row);
    res.write(frame("snapshot_begin", { count: rows.length, capacity: 1000, through: 3 }));
    res.write(frame("snapshot_chunk", { offset: 0, queries: rows }));
    res.write(frame("snapshot_end", { through: 3 }));
  }
  function labels(res: ServerResponse) {
    const clients = state.history
      .filter((row) => row.client_name)
      .map((row) => ({ ip: row.client, name: row.client_name }));
    res.write(frame("clients_begin", { count: clients.length }));
    if (clients.length) res.write(frame("clients_chunk", { offset: 0, clients }));
    res.write(frame("clients_end", {}));
  }
  const server = createServer((req, res) => {
    res.setHeader("Access-Control-Allow-Origin", "*");
    res.setHeader("Access-Control-Allow-Headers", "Authorization, Content-Type");
    res.setHeader("Access-Control-Allow-Methods", "GET, DELETE, OPTIONS");
    if (req.method === "OPTIONS") {
      res.writeHead(204).end();
      return;
    }
    if (req.url?.endsWith("/stream")) {
      res.writeHead(200, { "Content-Type": "text/event-stream", "Cache-Control": "no-store" });
      labels(res);
      snapshot(res);
      streams.add(res);
      res.on("close", () => streams.delete(res));
      return;
    }
    state.manualGets++;
    res.setHeader("Content-Type", "application/json");
    res.end(JSON.stringify({ queries: state.history, capacity: 1000 }));
  });
  return {
    state,
    streams,
    server,
    updateLabels() {
      for (const res of streams) labels(res);
    },
  };
}
type Backend = ReturnType<typeof createBackend> & { url: string };
const test = base.extend<{ dns: Backend }>({
  dns: async ({}, use) => {
    const b = createBackend();
    await new Promise<void>((resolve) => b.server.listen(0, "127.0.0.1", resolve));
    const url = `http://127.0.0.1:${(b.server.address() as AddressInfo).port}`;
    try {
      await use({ ...b, url });
    } finally {
      for (const res of b.streams) res.destroy();
      b.server.closeAllConnections();
      await new Promise<void>((resolve) => b.server.close(() => resolve()));
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
  const menu = page.getByRole("button", { name: "Open menu", exact: true });
  if (await menu.isVisible()) await menu.click();
  await page.getByRole("tab", { name: "Settings", exact: true }).click();
  await page.getByRole("tab", { name: "DNS queries", exact: true }).click();
  const region = page.locator(".dns-query-log");
  await expect(region.locator("tbody tr")).toHaveCount(3);
  return region;
}

test("DNS has no manual Refresh and resumes via SSE only", async ({ page, dns }) => {
  const region = await setup(page, dns);
  await expect(region.getByRole("button", { name: "Refresh", exact: true })).toHaveCount(0);
  await region.getByRole("button", { name: "Pause", exact: true }).click();
  await expect.poll(() => dns.streams.size).toBe(0);
  await expect(region.getByRole("button", { name: "Refresh", exact: true })).toHaveCount(0);
  await region.getByRole("button", { name: "Resume", exact: true }).click();
  await expect.poll(() => dns.streams.size).toBe(1);
  expect(dns.state.manualGets).toBe(0);
});

for (const width of [1280, 390]) {
  test(`aliases, IP fallback and address-keyed filtering work at ${width}px`, async ({
    page,
    dns,
  }) => {
    await page.setViewportSize({ width, height: 844 });
    const region = await setup(page, dns);
    await expect(region.locator("tbody tr").first().locator(".client-identity")).toContainText(
      "Phone",
    );
    await expect(region.locator("tbody tr").first().locator(".client-address")).toHaveText(
      "192.168.1.42",
    );
    await expect(region.locator("tbody tr").last()).toContainText("2001:db8::7");
    await expect(region).not.toContainText("::ffff:");
    await region.locator('[data-select-trigger][aria-label="Device"]').click();
    await page.getByRole("option", { name: /Phone.*192\.168\.1\.42/ }).click();
    await expect(region.locator("tbody tr")).toHaveCount(1);
    await expect(region.locator("tbody")).not.toContainText("192.168.1.43");
    const snapshotsBeforeRename = dns.state.snapshots;
    dns.state.history[0].client_name = "Renamed phone";
    dns.updateLabels();
    await expect(region.locator("tbody tr").first()).toContainText("Renamed phone");
    await expect(region.locator('[data-select-trigger][aria-label="Device"]')).toContainText(
      "Renamed phone",
    );
    await expect(region.locator("tbody tr")).toHaveCount(1);
    delete dns.state.history[0].client_name;
    dns.updateLabels();
    await expect(region.locator("tbody tr").first()).not.toContainText("Renamed phone");
    await expect(region.locator('[data-select-trigger][aria-label="Device"]')).toContainText(
      "192.168.1.42",
    );
    await expect(region.locator("tbody tr")).toHaveCount(1);
    expect(dns.state.manualGets).toBe(0);
    expect(dns.state.snapshots).toBe(snapshotsBeforeRename);
    expect(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth)).toBe(
      false,
    );
  });
}

for (const width of [1280, 390]) {
  test(`grouped rows keep their height through client label changes at ${width}px`, async ({
    page,
    dns,
  }) => {
    await page.setViewportSize({ width, height: 844 });
    for (const row of dns.state.history) delete row.client_name;
    const region = await setup(page, dns);
    await region.getByRole("button", { name: "Group by domain" }).click();
    const rows = region.locator("tbody tr");
    await expect(rows).toHaveCount(2);
    await page.evaluate(() => document.fonts.ready.then(() => undefined));
    const heights = () =>
      rows.evaluateAll((elements) =>
        elements.map((element) => element.getBoundingClientRect().height),
      );
    const original = await heights();

    dns.state.history[0].client_name = "ubuntu-desktop";
    dns.state.history[1].client_name = "redosHyperV";
    dns.updateLabels();
    await expect(rows.first()).toContainText("ubuntu-desktop");
    await expect(rows.first()).toContainText("192.168.1.42");
    await expect.poll(heights).toEqual(original);

    dns.state.history[0].client_name = "a".repeat(126);
    dns.updateLabels();
    await expect(rows.first()).toContainText("a".repeat(126));
    await expect.poll(heights).toEqual(original);

    // A domain with more than two clients stays compact, but every IP is
    // still reachable from a keyboard-accessible detail popover.
    const extra = {
      id: 4,
      time_ms: 4000,
      client: "192.168.1.44",
      domain: "example.org",
      qtype: 1,
      protocol: "udp" as const,
    };
    dns.state.history.unshift(extra);
    for (const stream of dns.streams) stream.write(frame("query", { ...extra, retained_from: 1 }));
    await expect(rows.first().locator(".dns-more-clients")).toHaveText("+2");
    await expect.poll(heights).toEqual(original);
    await rows.first().getByRole("button", { name: /Show all clients/ }).click();
    const details = page.getByRole("dialog", { name: "Clients" });
    await expect(details).toContainText("192.168.1.42");
    await expect(details).toContainText("192.168.1.43");
    await expect(details).toContainText("192.168.1.44");
    await page.keyboard.press("Escape");

    delete dns.state.history[1].client_name;
    dns.updateLabels();
    await expect(rows.first()).not.toContainText("a".repeat(126));
    await expect.poll(heights).toEqual(original);
    expect(dns.state.manualGets).toBe(0);
    expect(dns.state.snapshots).toBe(1);
  });
}

test("a burst uses bounded history and preserves the newest requests", async ({ page, dns }) => {
  const region = await setup(page, dns);
  const wire = Array.from({ length: 10000 }, (_, index) => {
    const id = index + 4;
    return frame("query", {
      id,
      time_ms: id * 2000,
      client: "192.168.1.42",
      domain: `burst-${id}.example`,
      qtype: 1,
      protocol: "udp",
      retained_from: Math.max(1, id - 999),
    });
  }).join("");
  for (const stream of dns.streams) stream.write(wire);
  await expect(region.locator("tbody tr").first()).toContainText("burst-10003.example");
  await expect(region.locator(".dns-summary .count")).toHaveText("Stored requests: 1000");
  // A byte-bounded journal does not promise a fixed row quota. Check the
  // actual retained IDs too, not only the visible counter/newest record.
  await region.getByRole("searchbox").fill("burst-9003.example");
  await expect(region.locator("tbody tr")).toHaveCount(0);
  await region.getByRole("searchbox").fill("burst-9004.example");
  await expect(region.locator("tbody tr")).toHaveCount(1);
  expect(dns.state.manualGets).toBe(0);
  expect(dns.state.snapshots).toBe(1);
});
