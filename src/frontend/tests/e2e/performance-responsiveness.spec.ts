import { mkdir, readFile, writeFile } from "node:fs/promises";
import { expect, test, type Page } from "@playwright/test";

import type { Group } from "../../src/types";
import { generalServerConfig } from "../mocks/general-settings";

function makeGroup(index: number, count = 0): Group {
  return {
    id: (0x10000000 + index).toString(16),
    name: `Responsiveness group ${index + 1}`,
    color: "#42bdf9",
    interface: "blackhole",
    enable: true,
    priority: 300,
    rules: Array.from({ length: count }, (_, rule) => ({
      id: (0x20000000 + index * Math.max(count, 1) + rule).toString(16),
      name: "",
      rule: `item${index}-${rule}.example.test`,
      type: "namespace",
      enable: true,
    })),
  };
}

async function backend(page: Page, groups: Group[]) {
  const config = generalServerConfig();
  config.groups = groups;
  config.subscriptions = [];
  config.profiles = [];
  const writes: string[] = [];
  await page.route("**/api/v1/**", (route) => {
    const request = route.request();
    if (request.method() === "OPTIONS") return route.fulfill({ status: 204 });
    const path = new URL(request.url()).pathname.replace("/api/v1", "");
    if (request.method() !== "GET") writes.push(path);
    const fixtures: Record<string, unknown> = {
      "/auth": { enabled: false },
      "/groups": { groups },
      "/subscriptions": { subscriptions: [] },
      "/profiles": { profiles: [] },
      "/system/interfaces": { interfaces: [{ id: "blackhole", name: "" }] },
      "/system/config/snapshot": { revision: "a".repeat(64), config },
      "/system/update/status": { stage: "idle" },
      "/system/update": { installed_version: "test", can_install: false },
    };
    return route.fulfill({ json: fixtures[path] ?? {} });
  });
  await page.route("https://api.github.com/**", (route) => route.fulfill({ json: [] }));
  return writes;
}

async function exportDraft(page: Page) {
  const downloading = page.waitForEvent("download");
  await page.getByRole("button", { name: "Export Config", exact: true }).click();
  const path = await (await downloading).path();
  return JSON.parse(await readFile(path!, "utf8")) as { groups: Group[] };
}

for (const manyPanels of [false, true]) {
  test(`wheel scrolling keeps viewport content and bounded DOM across ${manyPanels ? "many panels" : "100k rules"}`, async ({
    page,
  }, testInfo) => {
    test.setTimeout(120000);
    const groups = Array.from({ length: manyPanels ? 1000 : 1 }, (_, index) =>
      makeGroup(index, manyPanels ? 0 : 100000),
    );
    await backend(page, groups);
    await page.goto("/");
    await expect(page.locator("input.group-name").first()).toHaveValue(groups[0].name);
    if (!manyPanels) {
      await page.locator(".group [data-collapsible-trigger]").click();
      await expect(page.locator(".rule").first()).toBeVisible();
    }
    const selector = manyPanels ? ".group-wrapper" : ".rule";
    // Raw frame samples help diagnose blank paints without a machine-dependent
    // frame-time threshold in CI. The assertions below cover actual content.
    await page.evaluate((selector) => {
      const state = {
        running: true,
        samples: [] as { y: number; covered: boolean; gap: number }[],
      };
      (window as typeof window & { responsivenessSamples?: typeof state }).responsivenessSamples =
        state;
      let previous = performance.now();
      function sample(now: number) {
        if (!state.running) return;
        const middle = innerHeight / 2;
        const covered = [...document.querySelectorAll(selector)].some((element) => {
          const box = element.getBoundingClientRect();
          return box.top <= middle && box.bottom >= middle;
        });
        state.samples.push({ y: scrollY, covered, gap: now - previous });
        previous = now;
        requestAnimationFrame(sample);
      }
      requestAnimationFrame(sample);
    }, selector);
    await page.mouse.move(600, 450);
    for (const delta of [500, 700, 1100, 1400, 900, 600, -700, -900]) {
      const previousY = await page.evaluate(() => scrollY);
      await page.mouse.wheel(0, delta);
      await expect
        .poll(() => page.evaluate(() => scrollY), { intervals: [16, 32, 50] })
        .not.toBe(previousY);
      await expect
        .poll(
          () =>
            page.locator(selector).evaluateAll((items) => {
              const middle = innerHeight / 2;
              return items.some((element) => {
                const box = element.getBoundingClientRect();
                return box.top <= middle && box.bottom >= middle;
              });
            }),
          { intervals: [16, 32, 50] },
        )
        .toBe(true);
      expect(await page.locator(selector).count()).toBeLessThan(manyPanels ? 50 : 250);
    }
    const samples = await page.evaluate(() => {
      const state = (
        window as typeof window & {
          responsivenessSamples?: { running: boolean; samples: unknown[] };
        }
      ).responsivenessSamples!;
      state.running = false;
      return state.samples;
    });
    await mkdir(testInfo.outputDir, { recursive: true });
    const samplePath = testInfo.outputPath("scroll-frame-samples.json");
    await writeFile(samplePath, JSON.stringify(samples));
    await testInfo.attach("scroll-frame-samples.json", {
      path: samplePath,
      contentType: "application/json",
    });
    await page.getByRole("button", { name: "Back to top", exact: true }).click();
    await expect.poll(() => page.evaluate(() => scrollY)).toBeLessThan(1);
    await expect(page.locator("input.group-name").first()).toHaveValue(groups[0].name);
  });
}

test("large import applies only the latest selection and mode without early server writes", async ({
  page,
}) => {
  test.setTimeout(120000);
  const existing = makeGroup(9, 1);
  const writes = await backend(page, [existing]);
  await page.goto("/");
  await expect(page.locator("input.group-name").first()).toHaveValue(existing.name);
  const imported = [makeGroup(0, 50000), makeGroup(1, 50000)];
  const chooser = page.waitForEvent("filechooser");
  await page.getByRole("button", { name: "Import Config", exact: true }).click();
  await (
    await chooser
  ).setFiles({
    name: "responsive-100k.mtrickle",
    mimeType: "application/json",
    buffer: Buffer.from(
      JSON.stringify({ format: "mt-c-config", schemaVersion: 1, groups: imported }),
    ),
  });
  const dialog = page.getByRole("dialog", { name: "Import Config", exact: true });
  await expect(dialog).toBeVisible();
  const first = dialog.getByRole("checkbox", { name: imported[0].name, exact: true });
  const second = dialog.getByRole("checkbox", { name: imported[1].name, exact: true });
  const mode = dialog.getByRole("switch", { name: "Replace", exact: true });
  const apply = dialog.getByRole("button", { name: "Import", exact: true });
  await expect(apply).toBeEnabled();
  await first.uncheck();
  await mode.click();
  await second.uncheck();
  await first.check();
  await mode.click();
  await mode.click();
  await expect(first).toBeChecked();
  await expect(second).not.toBeChecked();
  await expect(mode).toBeChecked();
  await expect(apply).toBeEnabled();
  await apply.click();
  await expect(dialog).toBeHidden();
  await expect(page.locator("input.group-name").first()).toHaveValue(imported[0].name);
  await expect(page.locator("#save-changes")).toBeEnabled();
  expect(writes).toEqual([]);
  const draft = await exportDraft(page);
  expect(draft.groups).toHaveLength(1);
  expect(draft.groups[0].id).toBe(imported[0].id);
  expect(draft.groups[0].rules).toHaveLength(50000);
  expect(draft.groups[0].rules.at(-1)!.rule).toBe(imported[0].rules.at(-1)!.rule);
});

test("closing a pending large preview cannot affect a later import session", async ({ page }) => {
  test.setTimeout(120000);
  await backend(page, []);
  await page.goto("/");
  await expect(page.getByText("No groups yet", { exact: true })).toBeVisible();
  async function openFile(groups: Group[]) {
    const chooser = page.waitForEvent("filechooser");
    await page.getByRole("button", { name: "Import Config", exact: true }).click();
    await (
      await chooser
    ).setFiles({
      name: "session.mtrickle",
      mimeType: "application/json",
      buffer: Buffer.from(JSON.stringify({ format: "mt-c-config", schemaVersion: 1, groups })),
    });
    const dialog = page.getByRole("dialog", { name: "Import Config", exact: true });
    await expect(dialog).toBeVisible();
    return dialog;
  }
  const abandoned = [makeGroup(0, 50000), makeGroup(1, 50000)];
  const firstDialog = await openFile(abandoned);
  await expect(firstDialog.getByRole("button", { name: "Import", exact: true })).toBeEnabled();
  await firstDialog.getByRole("checkbox", { name: abandoned[0].name, exact: true }).uncheck();
  await firstDialog.getByRole("button", { name: "Cancel", exact: true }).click();
  await expect(firstDialog).toBeHidden();
  const latest = makeGroup(2, 2);
  const nextDialog = await openFile([latest]);
  const apply = nextDialog.getByRole("button", { name: "Import", exact: true });
  await expect(apply).toBeEnabled();
  await apply.click();
  await expect(nextDialog).toBeHidden();
  const draft = await exportDraft(page);
  expect(draft.groups.map((group) => group.id)).toEqual([latest.id]);
  expect(draft.groups[0].rules.map((rule) => rule.rule)).toEqual(
    latest.rules.map((rule) => rule.rule),
  );
});
