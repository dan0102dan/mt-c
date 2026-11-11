import { once } from "node:events";
import { createServer, type ServerResponse } from "node:http";
import type { AddressInfo } from "node:net";
import { expect, test, type Page } from "@playwright/test";

const frame = (event: string, data: unknown) => `event: ${event}\ndata: ${JSON.stringify(data)}\n\n`;
async function backend() {
  let enabled = false;
  let session = "";
  let generation = 0;
  let records: Buffer[] = [];
  let online = true;
  let streams = 0;
  let writes = 0;
  const peers = new Set<ServerResponse>();
  const capture = () => ({ enabled, session, through: String(records.length), dropped: "0" });
  const frames = (id: number, row: Buffer) => {
    const result: string[] = [];
    for (let offset = 0; offset < row.length; offset += 12288) {
      const bytes = row.subarray(offset, offset + 12288);
      result.push(frame("log", { session, id: String(id), offset, length: row.length, level: 2,
        end: offset + bytes.length === row.length, data: bytes.toString("base64") }));
    }
    return result;
  };
  const set = (next: boolean) => {
    if (next === enabled) return;
    enabled = next;
    records = [];
    if (next) session = (++generation).toString(16).padStart(32, "0");
    for (const peer of peers) {
      peer.write(frame("capture", capture()));
      peer.write(frame("ready", { session, through: "0" }));
    }
  };
  const emit = (text: string) => {
    if (!enabled) return;
    const row = Buffer.from(`2026-11-11T00:00:00Z INF ${text}\n`);
    records.push(row);
    for (const peer of peers) for (const text of frames(records.length, row)) peer.write(text);
  };
  const server = createServer(async (req, res) => {
    res.setHeader("Access-Control-Allow-Origin", "*");
    res.setHeader("Access-Control-Allow-Headers", "Authorization, Content-Type");
    res.setHeader("Access-Control-Allow-Methods", "GET,PUT,OPTIONS");
    if (req.method === "OPTIONS") { res.writeHead(204).end(); return; }
    if (!online) { res.writeHead(503).end(); return; }
    if (req.method === "PUT") {
      let body = "";
      for await (const chunk of req) body += chunk;
      writes++;
      set(JSON.parse(body).enabled);
      res.writeHead(200, { "Content-Type": "application/json" }).end(JSON.stringify(capture()));
      return;
    }
    const url = new URL(req.url!, "http://localhost");
    if (!url.pathname.endsWith("/stream")) { res.writeHead(404).end(); return; }
    streams++;
    peers.add(res);
    res.on("close", () => peers.delete(res));
    res.writeHead(200, { "Content-Type": "text/event-stream", "Cache-Control": "no-store" });
    try {
      const write = async (text: string) => { if (!res.write(text)) await once(res, "drain"); };
      const after = url.searchParams.get("session") === session ? Number(url.searchParams.get("after")) : 0;
      await write(frame("capture", capture()));
      const through = records.length;
      for (let i = after; i < through; i++) for (const text of frames(i + 1, records[i])) await write(text);
      await write(frame("ready", { session, through: String(through) }));
    } catch { res.destroy(); }
  });
  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
  return {
    url: `http://127.0.0.1:${(server.address() as AddressInfo).port}`,
    set, emit,
    get streams() { return streams; },
    get writes() { return writes; },
    get enabled() { return enabled; },
    disconnect() { online = false; for (const peer of peers) peer.destroy(); },
    recover() { online = true; },
    async close() {
      for (const peer of peers) peer.destroy();
      server.closeAllConnections();
      await new Promise<void>((resolve) => server.close(() => resolve()));
    },
  };
}
async function setup(page: Page, url: string) {
  await page.route("**/api/v1/**", async (route) => {
    const request = new URL(route.request().url());
    const path = request.pathname.slice("/api/v1".length);
    if (path.startsWith("/logs/")) {
      await route.continue({ url: `${url}${path}${request.search}` });
      return;
    }
    const fixtures: Record<string, unknown> = {
      "/auth": { enabled: false }, "/groups": { groups: [] },
      "/subscriptions": { subscriptions: [] }, "/profiles": { profiles: [] },
      "/system/interfaces": { interfaces: [] }, "/system/update/status": { stage: "idle" },
    };
    await route.fulfill({ json: fixtures[path.replace(/\/$/, "")] ?? {} });
  });
  await page.goto("/");
}
async function openLogs(page: Page) {
  await page.getByRole("tab", { name: "Settings", exact: true }).click();
  await page.getByRole("tab", { name: "Logs", exact: true }).click();
}

test("log recording is opt-in; stopping and tab changes keep received records", async ({ page }) => {
  const api = await backend();
  try {
    await setup(page, api.url);
    await expect(page.getByRole("tab", { name: "Settings", exact: true })).toBeVisible();
    expect(api.streams).toBe(0);
    expect(api.writes).toBe(0);
    api.emit("before start must not be captured");
    await openLogs(page);
    const panel = page.locator(".application-logs");
    await expect(panel.getByRole("button", { name: "Start log recording", exact: true })).toBeEnabled();
    expect(api.enabled).toBe(false);
    await panel.getByRole("button", { name: "Start log recording", exact: true }).click();
    await expect(panel.getByRole("button", { name: "Stop log recording", exact: true })).toBeEnabled();
    api.emit("captured after start");
    await expect(panel.getByRole("status")).toContainText("Received records: 1");
    await page.getByRole("tab", { name: "Groups", exact: true }).click();
    api.emit("captured on another tab");
    await openLogs(page);
    await expect(panel.getByRole("status")).toContainText("Received records: 2");
    await panel.getByRole("button", { name: "Stop log recording", exact: true }).click();
    await expect(panel.getByRole("button", { name: "Start log recording", exact: true })).toBeEnabled();
    await expect(panel.getByRole("status")).toContainText("Received records: 2");
    api.emit("not captured after stop");
    await panel.getByRole("button", { name: "Start log recording", exact: true }).click();
    await expect(panel.getByRole("button", { name: "Stop log recording", exact: true })).toBeEnabled();
    api.emit("new recording session");
    await expect(panel.getByRole("status")).toContainText("Received records: 3");
    await expect(panel.locator(".line.session")).toHaveCount(2);
    expect(api.writes).toBe(3);
  } finally { await api.close(); }
});

test("replay, backend loss, offline export and resume do not erase or duplicate history", async ({ page }) => {
  const api = await backend();
  try {
    api.set(true);
    api.emit("earliest recorded message");
    api.emit("Ж🙂".repeat(22000) + " LONG-END");
    await setup(page, api.url);
    await openLogs(page);
    const panel = page.locator(".application-logs");
    await expect(panel.getByRole("status")).toContainText("Received records: 2");
    api.disconnect();
    await expect(panel.getByRole("status")).toContainText("Connection lost");
    await panel.getByRole("searchbox").fill("earliest");
    await expect(panel.locator(".line")).toContainText("earliest recorded message");
    const downloadEvent = page.waitForEvent("download");
    await panel.getByRole("button", { name: "Save application logs", exact: true }).click();
    const download = await downloadEvent;
    const stream = await download.createReadStream();
    expect(stream).toBeTruthy();
    const chunks: Buffer[] = [];
    for await (const chunk of stream!) chunks.push(Buffer.from(chunk));
    const content = Buffer.concat(chunks).toString("utf8");
    expect(content).toContain("earliest recorded message");
    expect(content).toContain("Ж🙂".repeat(22000) + " LONG-END\n");
    api.recover();
    await expect(panel.getByRole("status")).toContainText("Recording application logs", { timeout: 15000 });
    await expect(panel.getByRole("status")).toContainText("Received records: 2");
    await panel.getByRole("button", { name: "Clear local logs", exact: true }).click();
    await expect(panel.getByRole("status")).toContainText("Received records: 0");
    await panel.getByRole("searchbox").fill("");
    api.emit("after local clear");
    await expect(panel.getByRole("status")).toContainText("Received records: 1");
    await expect(panel.locator(".line")).not.toContainText(["earliest recorded message"]);
  } finally { await api.close(); }
});
