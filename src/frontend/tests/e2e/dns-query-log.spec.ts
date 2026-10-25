import { createServer, type ServerResponse } from "node:http";
import type { AddressInfo } from "node:net";
import { test as base, expect, type Page } from "@playwright/test";

import type { DnsQuery } from "../../src/modules/settings/dns/data";

const sample: DnsQuery[] = [
  {
    id: 3,
    time_ms: 3000,
    client: "192.168.1.10",
    domain: "example.com",
    qtype: 1,
    protocol: "udp",
  },
  {
    id: 2,
    time_ms: 2000,
    client: "192.168.1.11",
    domain: "example.com",
    qtype: 28,
    protocol: "tcp",
  },
  {
    id: 1,
    time_ms: 1000,
    client: "192.168.1.10",
    domain: "another.net",
    qtype: 65,
    protocol: "udp",
  },
];
function frame(event: string, data: unknown) {
  return `event: ${event}\ndata: ${JSON.stringify(data)}\n\n`;
}
function createBackend() {
  const state = {
    history: [...sample],
    through: 3,
    connections: 0,
    snapshots: 0,
    clears: 0,
    failNext: false,
    holdEnd: false,
  };
  const clients = new Set<ServerResponse>();
  const pending = new Map<ServerResponse, number>();
  const broadcast = (event: string, data: unknown) => {
    for (const res of clients) res.write(frame(event, data));
  };
  const server = createServer((req, res) => {
    res.setHeader("Access-Control-Allow-Origin", "*");
    res.setHeader("Access-Control-Allow-Headers", "Authorization, Content-Type");
    res.setHeader("Access-Control-Allow-Methods", "GET, DELETE, OPTIONS");
    if (req.method === "OPTIONS") {
      res.writeHead(204).end();
      return;
    }
    if (req.url?.endsWith("/stream")) {
      state.connections++;
      if (state.failNext) {
        state.failNext = false;
        res.writeHead(503).end();
        return;
      }
      res.writeHead(200, { "Content-Type": "text/event-stream", "Cache-Control": "no-store" });
      const queries = [...state.history].reverse();
      res.write(
        frame("snapshot_begin", { capacity: 1000, count: queries.length, through: state.through }),
      );
      for (let offset = 0; offset < queries.length; offset += 32)
        res.write(frame("snapshot_chunk", { offset, queries: queries.slice(offset, offset + 32) }));
      if (state.holdEnd) pending.set(res, state.through);
      else res.write(frame("snapshot_end", { through: state.through }));
      clients.add(res);
      res.on("close", () => {
        clients.delete(res);
        pending.delete(res);
      });
      return; // A genuinely open HTTP response, not a completed route.fulfill mock.
    }
    res.setHeader("Content-Type", "application/json");
    if (req.method === "DELETE") {
      state.history = [];
      state.clears++;
      pending.clear();
      broadcast("clear", {});
      res.end(JSON.stringify({ cleared: true }));
    } else {
      state.snapshots++;
      res.end(JSON.stringify({ queries: state.history, capacity: 1000 }));
    }
  });
  return {
    state,
    server,
    clients,
    endSnapshot() {
      for (const [res, through] of pending) res.write(frame("snapshot_end", { through }));
      pending.clear();
    },
    emit(row: DnsQuery) {
      state.through = row.id;
      state.history = [row, ...state.history].slice(0, 1000);
      broadcast("query", row);
    },
    disconnect() {
      for (const res of clients) res.destroy();
      clients.clear();
      pending.clear();
    },
  };
}
type Backend = ReturnType<typeof createBackend> & { url: string };
const test = base.extend<{ dns: Backend }>({
  dns: async ({}, use) => {
    const backend = createBackend();
    await new Promise<void>((resolve) => backend.server.listen(0, "127.0.0.1", resolve));
    const url = `http://127.0.0.1:${(backend.server.address() as AddressInfo).port}`;
    try {
      await use({ ...backend, url });
    } finally {
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
  const menu = page.getByRole("button", { name: "Open menu" });
  if (await menu.isVisible()) await menu.click();
  await page.getByRole("tab", { name: "Settings" }).click();
  await page.getByRole("tab", { name: "DNS queries", exact: true }).click();
  const region = page.locator(".dns-query-log");
  await expect(region).toBeVisible();
  return region;
}
const liveQuery = (): DnsQuery => ({ ...sample[0], id: 4, domain: "live.example", time_ms: 4000 });

function seedHistory(dns: Backend, count: number) {
  dns.state.through = count;
  dns.state.history = Array.from({ length: count }, (_, index) => ({
    ...sample[0],
    id: count - index,
    domain: `host${count - index}.example`,
  }));
}

test("one open connection receives immediate updates without 3-second polling", async ({
  page,
  dns,
}) => {
  const region = await setup(page, dns);
  await expect(region.locator("tbody tr")).toHaveCount(3);
  dns.emit(liveQuery());
  await expect(region.locator("tbody tr")).toHaveCount(4);
  await expect(region).toHaveAttribute("aria-busy", "false");
  await expect(region.getByText("Live", { exact: true })).toHaveCount(0);
  await page.waitForTimeout(3400); // Prove the removed polling interval does not recur.
  expect(dns.state.connections).toBe(1);
  expect(dns.state.snapshots).toBe(0);
});
test("partial snapshots do not appear until end; reconnect discards unfinished staging", async ({
  page,
  dns,
}) => {
  dns.state.holdEnd = true;
  const region = await setup(page, dns);
  await expect.poll(() => dns.clients.size).toBe(1);
  await expect(region).toHaveAttribute("aria-busy", "true");
  await expect(region.locator("tbody tr")).toHaveCount(0);
  dns.disconnect();
  dns.state.history = [{ ...sample[0], id: 9, domain: "replacement.example" }];
  dns.state.through = 9;
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
  await region.locator('[data-select-trigger][aria-label="Device"]').click();
  await page.getByRole("option", { name: "192.168.1.10", exact: true }).click();
  await expect(region.locator("tbody tr")).toHaveCount(1);
  await region.locator('[data-select-trigger][aria-label="Device"]').click();
  await page.getByRole("option", { name: "All devices", exact: true }).click();
  await region.getByRole("searchbox").fill("");
  await region.getByRole("button", { name: "Group by domain" }).click();
  await expect(region.locator("tbody tr")).toHaveCount(2);
  await expect(region.locator("tbody tr").first()).toContainText("192.168.1.11");
});
test("pause closes the subscription; resume and reconnect replace the snapshot", async ({
  page,
  dns,
}) => {
  const region = await setup(page, dns);
  await expect(region.locator("tbody tr")).toHaveCount(3);
  await region.getByRole("button", { name: "Pause", exact: true }).click();
  await expect.poll(() => dns.clients.size).toBe(0);
  await expect(region.locator(".dns-summary").getByRole("status")).toHaveText(
    "Viewing is paused; recording continues.",
  );
  dns.emit(liveQuery());
  await expect(region.locator("tbody tr")).toHaveCount(3);
  await region.getByRole("button", { name: "Resume", exact: true }).click();
  await expect(region.locator("tbody tr")).toHaveCount(4);
  dns.disconnect();
  await expect(region.locator(".dns-summary").getByRole("status")).toHaveText("Reconnecting...");
  await expect(region.getByRole("button", { name: "Retry", exact: true })).toHaveCount(0);
  await expect.poll(() => dns.state.connections).toBe(3);
  await expect(region.locator(".dns-summary [role='status']")).toHaveText("");
  await expect(region.locator("tbody tr")).toHaveCount(4);
});
test("clear broadcasts and a failed connection retries without a snapshot GET", async ({
  page,
  dns,
}) => {
  dns.state.failNext = true;
  const region = await setup(page, dns);
  await expect(region.locator("tbody tr")).toHaveCount(3);
  expect(dns.state.connections).toBe(2);
  await region.getByRole("button", { name: "Clear history" }).click();
  await page
    .getByRole("dialog", { name: "Clear history", exact: true })
    .getByRole("button", { name: "Clear history", exact: true })
    .click();
  await expect(region.getByText("No DNS queries captured yet")).toBeVisible();
  expect(dns.state.clears).toBe(1);
  expect(dns.state.snapshots).toBe(0);
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
  await region.locator('[data-select-trigger][aria-label="Device"]').click();
  await expect(page.getByRole("option", { name: "All devices", exact: true })).toBeVisible();
  await page.keyboard.press("Escape");
  await expect(region.locator('[data-select-trigger][aria-label="Device"]')).toBeFocused();
  expect(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth)).toBe(false);
  await region.getByRole("button", { name: "Group by domain" }).click();
  expect(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth)).toBe(false);
});

for (const width of [1280, 390]) {
  test(`DNS tab preserves filters when switching sections (${width}px)`, async ({ page, dns }) => {
    await page.setViewportSize({ width, height: 844 });
    await page.emulateMedia({ reducedMotion: "reduce" });
    seedHistory(dns, 90);
    const region = await setup(page, dns);
    await expect(region.locator("tbody tr")).toHaveCount(30);
    const save = page.locator("#save-settings");
    const saveNode = await save.elementHandle();
    await expect(save).toBeDisabled();
    await region.getByRole("searchbox").fill("example");
    await page.keyboard.press("Control+f");
    await expect(region.getByRole("searchbox")).toBeFocused();
    await region.getByRole("button", { name: "Group by domain" }).click();
    await expect(region.locator("tbody tr")).toHaveCount(30);
    for (let i = 0; i < 3; i++) {
      await page.getByRole("tab", { name: "Profiles", exact: true }).click();
      await expect(region).toBeHidden();
      expect(
        await page.evaluate(() => {
          const event = new KeyboardEvent("keydown", {
            key: "f",
            code: "KeyF",
            ctrlKey: true,
            bubbles: true,
            cancelable: true,
          });
          document.dispatchEvent(event);
          return event.defaultPrevented;
        }),
      ).toBe(false);
      await page.getByRole("tab", { name: "DNS queries", exact: true }).click();
      await expect(page.getByRole("tab", { name: "DNS queries", exact: true })).toHaveAttribute(
        "aria-selected",
        "true",
      );
      await expect(region.getByRole("searchbox")).toHaveValue("example");
      await expect(region.getByRole("button", { name: "Individual queries" })).toHaveAttribute(
        "aria-pressed",
        "true",
      );
    }
    await expect(page.locator(".settings-page .page-controls")).toHaveCount(1);
    await expect(save).toBeDisabled();
    expect(
      await saveNode!.evaluate((node) => node === document.querySelector("#save-settings")),
    ).toBe(true);
    expect(dns.state.connections).toBeGreaterThanOrEqual(1);
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

for (const width of [390, 700, 1000, 1160, 1280]) {
  test(`scroll-to-top leaves pagination accessible (${width}px)`, async ({ page, dns }) => {
    await page.setViewportSize({ width, height: 844 });
    seedHistory(dns, 65);
    const region = await setup(page, dns);
    const next = region.getByRole("button", { name: "Next Page", exact: true });
    await next.scrollIntoViewIfNeeded();
    const toTop = page.getByRole("button", { name: "Back to top", exact: true });
    if (width > 668) {
      await expect(toTop).toBeVisible();
      await expect(async () => {
        const paginationBox = await region.locator(".controls").boundingBox();
        const topBox = await toTop.boundingBox();
        expect(paginationBox).not.toBeNull();
        expect(topBox).not.toBeNull();
        expect(paginationBox!.x + paginationBox!.width).toBeLessThan(topBox!.x);
      }).toPass();
    } else {
      await expect(toTop).toBeHidden();
    }
    await next.click();
    await expect(region.locator(".page-number")).toHaveText("2 / 3");
    await region.getByRole("button", { name: "Previous Page", exact: true }).click();
    await expect(region.locator(".page-number")).toHaveText("1 / 3");
    if (width > 668) {
      await toTop.click();
      await expect.poll(() => page.evaluate(() => window.scrollY)).toBe(0);
      await expect(toTop).toBeHidden();
      await next.scrollIntoViewIfNeeded();
      await expect(toTop).toBeVisible();
      await page.setViewportSize({ width: 390, height: 844 });
      await expect(toTop).toBeHidden();
    }
    expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBeLessThanOrEqual(
      width,
    );
  });
}

test("pagination uses the growing page gutter without a sudden horizontal jump", async ({
  page,
  dns,
}) => {
  seedHistory(dns, 65);
  const region = await setup(page, dns);
  const controls = region.locator(".controls");
  const toTop = page.getByRole("button", { name: "Back to top", exact: true });
  let previousRight: number | undefined;
  for (const width of [1040, 1080, 1120, 1140, 1160, 1161, 1200]) {
    await page.setViewportSize({ width, height: 844 });
    await controls.scrollIntoViewIfNeeded();
    await expect(toTop).toBeVisible();
    const box = await controls.boundingBox();
    const button = await toTop.boundingBox();
    expect(box).not.toBeNull();
    expect(button).not.toBeNull();
    const right = box!.x + box!.width;
    expect(right).toBeLessThan(button!.x);
    if (previousRight !== undefined) {
      expect(right).toBeGreaterThan(previousRight);
      expect(right - previousRight).toBeLessThanOrEqual(41);
    }
    previousRight = right;
  }
});

for (const width of [1280, 390]) {
  test(`time header alternates newest/oldest and preserves live sorting (${width}px)`, async ({
    page,
    dns,
  }) => {
    await page.setViewportSize({ width, height: 844 });
    // A router clock adjustment can make arrival IDs differ from wall time.
    dns.state.history = [
      { ...sample[0], id: 3, time_ms: 1000, domain: "late-arrival.example" },
      { ...sample[0], id: 2, time_ms: 9000, domain: "newest.example" },
      { ...sample[0], id: 1, time_ms: 3000, domain: "middle.example" },
    ];
    const region = await setup(page, dns);
    const header = region.locator(".time-sort-header");
    const sort = region.getByRole("button", { name: "Time", exact: true });
    const domains = region.locator("tbody td.domain .cell-value");
    await expect(sort).toBeVisible();
    await expect(header).toHaveAttribute("aria-sort", "descending");
    await expect(sort).toHaveAttribute("title", "Oldest first");
    await expect(domains).toHaveText(["newest.example", "middle.example", "late-arrival.example"]);
    await sort.focus();
    await page.keyboard.press("Enter");
    await expect(header).toHaveAttribute("aria-sort", "ascending");
    await expect(sort).toHaveAttribute("title", "Newest first");
    await expect(domains).toHaveText(["late-arrival.example", "middle.example", "newest.example"]);
    await page.keyboard.press("Space");
    await expect(header).toHaveAttribute("aria-sort", "descending");
    await sort.click(); // Third click returns to ascending, never to an unsorted state.
    await expect(header).toHaveAttribute("aria-sort", "ascending");
    await region.getByRole("searchbox").fill("example");
    dns.emit({ ...liveQuery(), time_ms: 7000 });
    await expect(domains).toHaveText([
      "late-arrival.example",
      "middle.example",
      "live.example",
      "newest.example",
    ]);
    expect(dns.state.connections).toBe(1);
    dns.disconnect();
    await expect.poll(() => dns.state.connections).toBe(2);
    await expect(header).toHaveAttribute("aria-sort", "ascending");
    await expect(domains).toHaveText([
      "late-arrival.example",
      "middle.example",
      "live.example",
      "newest.example",
    ]);
    await expect(region.getByRole("searchbox")).toHaveValue("example");
    expect(dns.state.snapshots).toBe(0);
    await page.locator(".locale button").click();
    const russianSort = region.getByRole("button", { name: "Время", exact: true });
    await expect(russianSort).toHaveAttribute("title", "Сначала новые");
    await russianSort.click();
    await expect(header).toHaveAttribute("aria-sort", "descending");
    await expect(russianSort).toHaveAttribute("title", "Сначала старые");
    await region.getByRole("button", { name: "Группировать по домену" }).click();
    await expect(region.getByRole("button", { name: "Время", exact: true })).toBeVisible();
    await expect(header).toHaveAttribute("aria-sort", "descending");
    expect(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth)).toBe(
      false,
    );
  });
}

test("time sorting happens before pagination and resets only the page", async ({ page, dns }) => {
  seedHistory(dns, 65);
  dns.state.history = dns.state.history.map((row) => ({ ...row, time_ms: row.id * 1000 }));
  const region = await setup(page, dns);
  await region.getByRole("searchbox").fill("host");
  await region.locator('[data-select-trigger][aria-label="Device"]').click();
  await page.getByRole("option", { name: "192.168.1.10", exact: true }).click();
  const rows = region.locator("tbody tr");
  await expect(rows.first()).toContainText("host65.example");
  await region.getByRole("button", { name: "Next Page", exact: true }).click();
  await expect(region.locator(".page-number")).toHaveText("2 / 3");
  await expect(rows.first()).toContainText("host35.example");
  await region.getByRole("button", { name: "Time", exact: true }).click();
  await expect(region.locator(".page-number")).toHaveText("1 / 3");
  await expect(rows.first()).toContainText("host1.example");
  await expect(rows.last()).toContainText("host30.example");
  await expect(region.getByRole("searchbox")).toHaveValue("host");
  await expect(region.locator('[data-select-trigger][aria-label="Device"]')).toContainText(
    "192.168.1.10",
  );
  await region.getByRole("button", { name: "Next Page", exact: true }).click();
  await expect(rows.first()).toContainText("host31.example");
  expect(dns.state.connections).toBe(1);
  expect(dns.state.snapshots).toBe(0);
});

test("domain grouping uses the same time direction rather than request count", async ({
  page,
  dns,
}) => {
  dns.state.through = 4;
  dns.state.history = [
    { ...sample[0], id: 4, time_ms: 9000, domain: "new.example" },
    { ...sample[0], id: 3, time_ms: 3000, domain: "busy.example" },
    { ...sample[0], id: 2, time_ms: 2000, domain: "busy.example" },
    { ...sample[0], id: 1, time_ms: 1000, domain: "old.example" },
  ];
  const region = await setup(page, dns);
  await region.getByRole("button", { name: "Group by domain" }).click();
  const domains = region.locator("tbody td.domain .cell-value");
  const sort = region.getByRole("button", { name: "Time", exact: true });
  await expect(region.locator(".time-sort-header")).toHaveAttribute("aria-sort", "descending");
  await expect(domains).toHaveText(["new.example", "busy.example", "old.example"]);
  await sort.click();
  await expect(domains).toHaveText(["old.example", "busy.example", "new.example"]);
  dns.emit({ ...liveQuery(), id: 5, time_ms: 10000, domain: "old.example" });
  await expect(domains).toHaveText(["busy.example", "new.example", "old.example"]);
  await region.getByRole("button", { name: "Individual queries" }).click();
  await expect(region.locator(".time-sort-header")).toHaveAttribute("aria-sort", "ascending");
  await expect(region.locator("tbody tr").first()).toContainText("old.example");
  expect(dns.state.connections).toBe(1);
});

test("time sorting preserves combined-question membership and details", async ({ page, dns }) => {
  dns.state.through = 4;
  dns.state.history = [
    { ...sample[0], id: 4, time_ms: 6000, domain: "later.example" },
    { ...sample[0], id: 3, time_ms: 1500, domain: "related.example", qtype: 65 },
    { ...sample[0], id: 2, time_ms: 1200, domain: "related.example", qtype: 28 },
    { ...sample[0], id: 1, time_ms: 1000, domain: "related.example", qtype: 1 },
  ];
  const region = await setup(page, dns);
  const rows = region.locator("tbody tr");
  await expect(rows).toHaveCount(2);
  await expect(rows.first()).toContainText("later.example");
  const combined = rows.filter({ hasText: "related.example" });
  const before = await combined.boundingBox();
  await combined.locator(".query-details").click();
  const popup = page.getByRole("dialog", { name: "DNS query details" });
  await expect(popup.locator(".raw-query")).toHaveCount(3);
  expect((await combined.boundingBox())!.height).toBe(before!.height);
  await page.keyboard.press("Escape");
  await expect(popup).not.toBeVisible();
  await expect(combined.locator(".query-details")).toBeFocused();
  await region.getByRole("button", { name: "Time", exact: true }).click();
  await expect(rows.first()).toContainText("related.example");
  await combined.locator(".query-details").click();
  await expect(popup.locator(".raw-query")).toHaveCount(3);
  await page.keyboard.press("Escape");
  await expect(region.getByRole("button", { name: "Combine related queries" })).toHaveCount(0);
  await region.getByRole("button", { name: "Group by domain" }).click();
  await expect(rows).toHaveCount(2);
  await region.getByRole("button", { name: "Individual queries" }).click();
  await expect(rows).toHaveCount(2);
  await expect(rows.first()).toContainText("related.example");
  await rows.filter({ hasText: "related.example" }).locator(".query-details").click();
  await expect(popup.locator(".raw-query")).toHaveCount(3);
  await expect(region.locator(".dns-summary .count")).toHaveText("Stored requests: 4");
  expect(dns.state.connections).toBe(1);
});

for (const width of [1280, 390]) {
  test(`group count sorting covers all pages and live updates at ${width}px`, async ({
    page,
    dns,
  }) => {
    await page.setViewportSize({ width, height: width === 390 ? 844 : 900 });
    seedHistory(dns, 65);
    for (let id = 66; id <= 69; id++)
      dns.state.history.unshift({ ...sample[0], id, time_ms: id * 1000, domain: "host1.example" });
    dns.state.through = 69;
    const region = await setup(page, dns);
    await region.getByRole("button", { name: "Group by domain" }).click();
    const count = region.getByRole("button", { name: "Requests", exact: true });
    const domains = region.locator("tbody td.domain .cell-value");
    await region.getByRole("button", { name: "Next Page" }).click();
    await count.click();
    await expect(domains.first()).toHaveText("host1.example");
    await expect(domains).toHaveCount(30);
    await expect(region.locator(".count-sort-header")).toHaveAttribute("aria-sort", "descending");
    await expect(region.locator(".time-sort-header")).toHaveAttribute("aria-sort", "none");
    await count.click();
    await expect(domains.first()).not.toHaveText("host1.example");
    await expect(region.locator(".count-sort-header")).toHaveAttribute("aria-sort", "ascending");
    await region.getByRole("button", { name: "Next Page" }).click();
    await region.getByRole("button", { name: "Next Page" }).click();
    await expect(domains.last()).toHaveText("host1.example");
    await count.click();
    await expect(domains.first()).toHaveText("host1.example");
    for (let id = 70; id <= 75; id++)
      dns.emit({ ...sample[0], id, time_ms: id * 1000, domain: "host2.example" });
    await expect(domains.first()).toHaveText("host2.example");
    await page.locator(".locale button").click();
    await expect(region.getByRole("button", { name: "Запросы", exact: true })).toHaveAttribute(
      "title",
      "Сначала меньше запросов",
    );
    await region.getByRole("button", { name: "Время", exact: true }).click();
    await expect(region.locator(".count-sort-header")).toHaveAttribute("aria-sort", "none");
    await expect(region.locator(".time-sort-header")).toHaveAttribute("aria-sort", "descending");
    expect(dns.state.connections).toBe(1);
  });
}
