import { once } from "node:events";
import { createServer, type ServerResponse } from "node:http";
import type { AddressInfo } from "node:net";
import { expect, test, type Page } from "@playwright/test";

import { DNS_HISTORY_MEMORY_LIMIT, MAX_DNS_CAPACITY } from "../../src/modules/settings/dns/data";

const frame = (event: string, data: unknown) =>
  `event: ${event}\ndata: ${JSON.stringify(data)}\n\n`;
const row = (id: number) => ({
  id,
  time_ms: id,
  client: "192.0.2.4",
  domain: `history-${id}.example`,
  qtype: 1,
  protocol: "udp",
});

/** A real chunked response, not a buffered route.fulfill pretending to be SSE. */
test("a 100k byte-budget snapshot, retention and time sorting use one live stream", async ({
  page,
}) => {
  test.setTimeout(60000);
  let opened = 0;
  let manualGets = 0;
  const peers = new Set<ServerResponse>();
  const server = createServer((req, res) => {
    res.setHeader("Access-Control-Allow-Origin", "*");
    res.setHeader("Access-Control-Allow-Headers", "Authorization, Content-Type");
    if (req.method === "OPTIONS") {
      res.writeHead(204).end();
      return;
    }
    if (!req.url?.endsWith("/stream")) {
      manualGets++;
      res.writeHead(404).end();
      return;
    }
    opened++;
    peers.add(res);
    res.on("close", () => peers.delete(res));
    res.writeHead(200, { "Content-Type": "text/event-stream", "Cache-Control": "no-store" });
    void (async () => {
      const send = async (event: string, data: unknown) => {
        if (!res.write(frame(event, data))) await once(res, "drain");
      };
      await send("capture", {
        enabled: true,
        memory_used: DNS_HISTORY_MEMORY_LIMIT,
        memory_limit: DNS_HISTORY_MEMORY_LIMIT,
        count: 100000,
        capacity: MAX_DNS_CAPACITY,
      });
      await send("snapshot_begin", { count: 100000, through: 100000, capacity: MAX_DNS_CAPACITY });
      for (let offset = 0; offset < 100000; offset += 32) {
        await send("snapshot_chunk", {
          offset,
          queries: Array.from({ length: Math.min(32, 100000 - offset) }, (_, i) =>
            row(offset + i + 1),
          ),
        });
      }
      await send("snapshot_end", { through: 100000 });
    })().catch(() => res.destroy());
  });
  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
  const url = `http://127.0.0.1:${(server.address() as AddressInfo).port}`;
  try {
    await setup(page, url);
    const region = page.locator(".dns-query-log");
    const count = region.locator(".dns-summary .count");
    await expect(count).toHaveText("Stored requests: 100000", { timeout: 30000 });
    await expect(region.locator("tbody tr").first()).toContainText("history-100000.example");
    await region.getByRole("button", { name: "Time", exact: true }).click();
    await expect(region.locator("tbody tr").first()).toContainText("history-1.example");

    // Page recycling can evict multiple oldest records, without reaching any row quota.
    for (const peer of peers)
      peer.write(
        frame("query", {
          ...row(100001),
          retained_from: 409,
          memory_used: DNS_HISTORY_MEMORY_LIMIT,
        }),
      );
    await expect(count).toHaveText("Stored requests: 99593");
    await expect(region.locator("tbody tr").first()).toContainText("history-409.example");
    await region.getByRole("searchbox").fill("history-408.example");
    await expect(region.locator("tbody tr")).toHaveCount(0);
    await region.getByRole("searchbox").fill("history-409.example");
    await expect(region.locator("tbody tr")).toHaveCount(1);
    await region.getByRole("searchbox").fill("");
    await region.getByRole("button", { name: "Time", exact: true }).click();
    await expect(region.locator("tbody tr").first()).toContainText("history-100001.example");
    expect(opened).toBe(1);
    expect(manualGets).toBe(0);
  } finally {
    for (const peer of peers) peer.destroy();
    server.closeAllConnections();
    await new Promise<void>((resolve) => server.close(() => resolve()));
  }
});

async function setup(page: Page, url: string) {
  await page.route("**/api/v1/**", async (route) => {
    const path = new URL(route.request().url()).pathname.slice("/api/v1".length);
    if (path.startsWith("/system/dns/")) {
      await route.continue({ url: `${url}${path}` });
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
  await page.getByRole("tab", { name: "Settings" }).click();
  await page.getByRole("tab", { name: "DNS queries", exact: true }).click();
}
