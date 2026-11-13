import { readFile } from "node:fs/promises";
import { expect, test, type Page } from "@playwright/test";

import type { Group, Subscription } from "../../src/types";
import { generalServerConfig } from "../mocks/general-settings";

const id = (value: number) => value.toString(16).padStart(8, "0");
const group = (index: number, count = 0): Group => ({
  id: id(0x10000000 + index),
  name: `Virtual group ${index + 1}`,
  color: "#42bdf9",
  interface: "blackhole",
  enable: true,
  priority: 300,
  rules: Array.from({ length: count }, (_, rule) => ({
    id: id(0x20000000 + index * count + rule),
    name: "",
    rule: `item${index}-${rule}.example.test`,
    type: "namespace",
    enable: true,
  })),
});

async function backend(page: Page, groups: Group[] = [], subscriptions: Subscription[] = []) {
  const config = generalServerConfig();
  config.groups = groups;
  config.subscriptions = subscriptions.map(({ lastUpdate, ...item }) => ({
    ...item,
    last_update: lastUpdate ?? 0,
    interval: item.interval ?? 86400,
  }));
  config.profiles = [];
  await page.route("**/api/v1/**", async (route) => {
    if (route.request().method() === "OPTIONS") return route.fulfill({ status: 204 });
    const path = new URL(route.request().url()).pathname.replace("/api/v1", "");
    const fixtures: Record<string, unknown> = {
      "/auth": { enabled: false },
      "/groups": { groups },
      "/subscriptions": { subscriptions },
      "/profiles": { profiles: [] },
      "/system/interfaces": { interfaces: [{ id: "blackhole", name: "" }] },
      "/system/config/snapshot": { revision: "a".repeat(64), config },
      "/system/update/status": { stage: "idle" },
      "/system/update": { installed_version: "test", can_install: false },
    };
    return route.fulfill({ json: fixtures[path] ?? {} });
  });
  await page.route("https://api.github.com/**", (route) => route.fulfill({ json: [] }));
}

async function openTab(page: Page, name: string) {
  const menu = page.locator(".mobile-dropdown-btn");
  if (await menu.isVisible()) await menu.click();
  await page.getByRole("tab", { name, exact: true }).click();
}

async function exported(page: Page) {
  const downloading = page.waitForEvent("download");
  await page.getByRole("button", { name: "Export Config", exact: true }).click();
  const file = await (await downloading).path();
  return JSON.parse(await readFile(file!, "utf8")) as { groups: Group[] };
}

for (const width of [1280, 390]) {
  test(`100k group keeps bounded rows and edits through document scrolling at ${width}px`, async ({
    page,
  }) => {
    test.setTimeout(120000);
    await page.setViewportSize({ width, height: width === 390 ? 844 : 900 });
    const data = group(0, 100000);
    await backend(page, [data]);
    await page.goto("/");
    const panel = page.locator(".group").first();
    await expect(panel).toBeVisible();
    if (!(await panel.locator(".rule").first().isVisible()))
      await panel.locator("[data-collapsible-trigger]").click();
    const first = page.locator(`.rule[data-uuid="${data.rules[0].id}"]`);
    await expect(first).toBeVisible();
    await first.locator(".name input").fill("Survives virtualization");
    await first.locator(".name input").blur();
    await expect(page.locator(".group .pagination")).toHaveCount(0);
    await expect.poll(() => page.locator(".rule").count()).toBeLessThan(250);
    await page.evaluate(() => window.scrollTo(0, document.documentElement.scrollHeight));
    const last = page.locator(`.rule[data-uuid="${data.rules.at(-1)!.id}"]`);
    await expect(last).toBeVisible();
    await expect.poll(() => page.locator(".rule").count()).toBeLessThan(250);
    await expect(first).toHaveCount(0);
    await page.evaluate(() => window.scrollTo(0, 0));
    await expect(first.locator(".name input")).toHaveValue("Survives virtualization");
    const result = await exported(page);
    expect(result.groups[0].rules).toHaveLength(100000);
    expect(result.groups[0].rules[0].name).toBe("Survives virtualization");
    expect(result.groups[0].rules.at(-1)!.rule).toBe(data.rules.at(-1)!.rule);
  });

  test(`import stages 100k rules with bounded panel DOM and complete export at ${width}px`, async ({
    page,
  }) => {
    test.setTimeout(120000);
    await page.setViewportSize({ width, height: width === 390 ? 844 : 900 });
    await backend(page);
    await page.goto("/");
    const groups = Array.from({ length: 100 }, (_, i) => group(i, 1000));
    const chooser = page.waitForEvent("filechooser");
    await page.getByRole("button", { name: "Import Config", exact: true }).click();
    await (
      await chooser
    ).setFiles({
      name: "virtual-100k.mtrickle",
      mimeType: "application/json",
      buffer: Buffer.from(JSON.stringify({ format: "mt-c-config", schemaVersion: 1, groups })),
    });
    const dialog = page.getByRole("dialog", { name: "Import Config", exact: true });
    await expect(dialog).toBeVisible();
    await dialog.getByRole("button", { name: "Import", exact: true }).click();
    await expect(dialog).toBeHidden();
    await expect(page.locator("input.group-name").first()).toHaveValue("Virtual group 1");
    await expect(page.locator("#save-changes")).toBeEnabled();
    await expect.poll(() => page.locator(".group-wrapper").count()).toBeLessThan(30);
    await expect(page.getByText("Loading groups...", { exact: true })).toHaveCount(0);
    await page.evaluate(() => window.scrollTo(0, document.documentElement.scrollHeight));
    await expect(page.locator("input.group-name").last()).toHaveValue("Virtual group 100");
    const result = await exported(page);
    expect(result.groups).toHaveLength(100);
    expect(result.groups.reduce((count, item) => count + item.rules.length, 0)).toBe(100000);
  });
}

test("search mounts a matching offscreen panel and route popup stays above virtual neighbors", async ({
  page,
}) => {
  await backend(
    page,
    Array.from({ length: 1000 }, (_, index) => group(index, 1)),
  );
  await page.goto("/");
  await expect(page.locator("input.group-name").first()).toHaveValue("Virtual group 1");
  const search = page.locator(".group-controls-search .search-input");
  await search.fill("Virtual group 999");
  await expect(page.locator("input.group-name").first()).toHaveValue("Virtual group 999");
  await search.fill("");
  await page.evaluate(() => window.scrollTo(0, document.documentElement.scrollHeight / 2));
  let visibleGroupId = "";
  await expect
    .poll(async () => {
      visibleGroupId = await page.locator(".group").evaluateAll((groups) => {
        return (
          groups
            .find((element) => {
              const box = element.querySelector(".group-header")!.getBoundingClientRect();
              return box.top > 180 && box.bottom < innerHeight - 50;
            })
            ?.getAttribute("data-uuid") ?? ""
        );
      });
      return visibleGroupId;
    })
    .not.toBe("");
  const trigger = page.locator(
    `.group[data-uuid="${visibleGroupId}"] .group-header [data-select-trigger]`,
  );
  await trigger.click();
  const popup = page.getByRole("listbox");
  await expect(popup).toBeVisible();
  await expect
    .poll(() =>
      popup.evaluate((element) => {
        const box = element.getBoundingClientRect();
        return element.contains(
          document.elementFromPoint(box.left + box.width / 2, box.top + box.height / 2),
        );
      }),
    )
    .toBe(true);
  await page.keyboard.press("Escape");
  await expect(popup).toHaveCount(0);
  await expect(trigger).toBeFocused();
});

for (const width of [1280, 390]) {
  test(`100k subscription preserves toggles and final row through scrolling at ${width}px`, async ({
    page,
  }) => {
    test.setTimeout(120000);
    await page.setViewportSize({ width, height: width === 390 ? 844 : 900 });
    const subscription: Subscription = {
      id: "aabbccdd",
      name: "Virtual subscription",
      url: "https://example.test/list.txt",
      interface: "blackhole",
      enable: true,
      priority: 100,
      interval: 86400,
      lastUpdate: 1,
      rules: Array.from({ length: 100000 }, (_, i) => ({
        id: id(i + 1),
        rule: `subscription${i}.example.test`,
        type: "namespace",
        enable: true,
      })),
    };
    await backend(page, [], [subscription]);
    await page.goto("/");
    await openTab(page, "Subscriptions");
    const panel = page.locator(".subscription-panel").first();
    await expect(panel).toBeVisible();
    if (!(await panel.locator(".subscription-rule").first().isVisible()))
      await panel.locator("[data-collapsible-trigger]").click();
    const first = page.locator('.subscription-rule[data-index="0"]');
    await expect(first).toBeVisible();
    await first.getByRole("switch").click();
    await first.getByRole("switch").blur();
    await expect(page.locator(".subscription-panel .pagination")).toHaveCount(0);
    await expect.poll(() => page.locator(".subscription-rule").count()).toBeLessThan(250);
    await page.evaluate(() => window.scrollTo(0, document.documentElement.scrollHeight));
    await expect(page.locator('.subscription-rule[data-index="99999"]')).toBeVisible();
    await expect.poll(() => page.locator(".subscription-rule").count()).toBeLessThan(250);
    await page.evaluate(() => window.scrollTo(0, 0));
    await expect(first.getByRole("switch")).not.toBeChecked();
    await expect(page.locator("#save-subscriptions")).toBeEnabled();
  });
}

test("an invalid offscreen rule blocks Save until corrected through search", async ({ page }) => {
  const groups = Array.from({ length: 1000 }, (_, index) => group(index, 1));
  const invalid = groups.at(-1)!.rules[0];
  invalid.type = "subnet";
  invalid.rule = "invalid-offscreen-subnet";
  await backend(page, groups);
  await page.goto("/");
  await page.locator("input.group-name").first().fill("Dirty draft");
  await expect(page.locator("#save-changes")).toBeDisabled();
  const search = page.locator(".group-controls-search .search-input");
  await search.fill("invalid-offscreen-subnet");
  const panel = page.locator(".group").first();
  if (!(await panel.locator(".rule").first().isVisible()))
    await panel.locator("[data-collapsible-trigger]").click();
  const invalidInput = page.locator(`.rule[data-uuid="${invalid.id}"] .pattern input`);
  await expect(invalidInput).toHaveAttribute("aria-invalid", "true");
  await expect(invalidInput).toHaveClass(/invalid/);
  await invalidInput.fill("192.0.2.0/24");
  await expect(page.locator("#save-changes")).toBeEnabled();
});

test("subscription validity includes unmounted rules and can be corrected through search", async ({
  page,
}) => {
  const subscription: Subscription = {
    id: "aabbccdd",
    name: "Validation list",
    url: "https://example.test/list.txt",
    interface: "blackhole",
    enable: true,
    priority: 100,
    interval: 86400,
    lastUpdate: 1,
    rules: Array.from({ length: 100000 }, (_, index) => ({
      id: id(index + 1),
      rule: `item${index}.example.test`,
      type: "namespace",
      enable: true,
    })),
  };
  subscription.rules.at(-1)!.type = "subnet";
  subscription.rules.at(-1)!.rule = "invalid-offscreen-subnet";
  await backend(page, [], [subscription]);
  await page.goto("/");
  await openTab(page, "Subscriptions");
  await page.locator(".subscription-url-input").fill("https://example.test/edited.txt");
  await expect(page.locator("#save-subscriptions")).toBeDisabled();
  await page
    .locator(".subscription-controls-search .search-input")
    .fill("invalid-offscreen-subnet");
  const panel = page.locator(".subscription-panel").first();
  if (!(await panel.locator(".subscription-rule").first().isVisible()))
    await panel.locator("[data-collapsible-trigger]").click();
  // Search is debounced and indexes the entire collection. Wait for the
  // requested logical row before acting, rather than an arbitrary mounted row.
  const invalidRow = panel.locator('.subscription-rule[data-index="99999"]');
  await expect(invalidRow.locator(".pattern-value")).toHaveText("invalid-offscreen-subnet");
  await expect(panel.locator(".subscription-rule")).toHaveCount(1);
  await invalidRow.locator("[data-select-trigger]").click();
  await page.getByRole("option", { name: "Regex", exact: true }).click();
  await expect(page.locator("#save-subscriptions")).toBeEnabled();
});

test("resizing a scrolled list remeasures stacked rows without overlap", async ({ page }) => {
  const data = group(0, 1000);
  await backend(page, [data]);
  await page.setViewportSize({ width: 1280, height: 900 });
  await page.goto("/");
  const panel = page.locator(".group").first();
  await expect(panel).toBeVisible();
  if (!(await panel.locator(".rule").first().isVisible()))
    await panel.locator("[data-collapsible-trigger]").click();
  await page.evaluate(() => window.scrollTo(0, document.documentElement.scrollHeight / 2));
  await page.setViewportSize({ width: 390, height: 844 });
  await expect
    .poll(() =>
      page.locator(".rule").evaluateAll((nodes) => {
        const boxes = nodes.map((node) => node.getBoundingClientRect());
        return (
          boxes.length > 1 &&
          boxes.every((box, index) => index === 0 || box.top >= boxes[index - 1].bottom - 1)
        );
      }),
    )
    .toBe(true);
  await page.evaluate(() => window.scrollTo(0, document.documentElement.scrollHeight));
  await expect(page.locator(`.rule[data-uuid="${data.rules.at(-1)!.id}"]`)).toBeVisible();
  await page.setViewportSize({ width: 1280, height: 900 });
  await page.evaluate(() => window.scrollTo(0, document.documentElement.scrollHeight));
  await expect(page.locator(`.rule[data-uuid="${data.rules.at(-1)!.id}"]`)).toBeVisible();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
});

test("Tab advances from a retained editor to its logical next rule after scrolling", async ({
  page,
}) => {
  const data = group(0, 1000);
  await backend(page, [data]);
  await page.goto("/");
  const panel = page.locator(".group").first();
  await expect(panel).toBeVisible();
  if (!(await panel.locator(".rule").first().isVisible()))
    await panel.locator("[data-collapsible-trigger]").click();
  const first = page.locator(`.rule[data-uuid="${data.rules[0].id}"]`);
  await first.locator("button").last().focus();
  await page.evaluate(() => window.scrollTo(0, document.documentElement.scrollHeight));
  await expect(first.locator("button").last()).toBeFocused();
  await page.keyboard.press("Tab");
  await expect(page.locator(`.rule[data-uuid="${data.rules[1].id}"] .name input`)).toBeFocused();
});

test("a popup remains anchored through scrolling and an unmounted hovered row leaves no tooltip", async ({
  page,
}) => {
  const data = group(0, 1000);
  for (const rule of data.rules.slice(1)) rule.enable = false;
  await backend(page, [data]);
  await page.goto("/");
  const panel = page.locator(".group").first();
  await expect(panel).toBeVisible();
  if (!(await panel.locator(".rule").first().isVisible()))
    await panel.locator("[data-collapsible-trigger]").click();
  const first = page.locator(`.rule[data-uuid="${data.rules[0].id}"]`);
  const trigger = first.locator("[data-select-trigger]");
  await trigger.click();
  const popup = page.getByRole("listbox");
  await expect(popup).toBeVisible();
  await page.evaluate(() => window.scrollTo(0, document.documentElement.scrollHeight));
  await expect(trigger).toBeAttached();
  await expect(popup).toBeVisible();
  await page.keyboard.press("Escape");
  await expect(popup).toHaveCount(0);
  await expect(trigger).toBeFocused();
  await trigger.blur();
  await page.evaluate(() => window.scrollTo(0, 0));
  await first.getByRole("switch").hover();
  await expect(page.locator("#global-tooltip")).toHaveText("Disable Rule");
  await page.evaluate(() => window.scrollTo(0, document.documentElement.scrollHeight));
  await expect(first).toHaveCount(0);
  await expect(page.locator("#global-tooltip").filter({ hasText: "Disable Rule" })).toHaveCount(0);
});

for (const width of [1280, 390]) {
  test(`subscription panel gaps and drag destination retain geometry at ${width}px`, async ({
    page,
  }) => {
    await page.setViewportSize({ width, height: width === 390 ? 844 : 900 });
    const subscriptions: Subscription[] = ["First", "Second", "Third"].map((name, index) => ({
      id: id(index + 1),
      name,
      url: `https://example.test/${index}`,
      interface: "blackhole",
      enable: true,
      priority: 100,
      interval: 86400,
      lastUpdate: 1,
      rules: [],
    }));
    await backend(page, [], subscriptions);
    await page.goto("/");
    await openTab(page, "Subscriptions");
    const panels = page.locator(".subscription-panel");
    await expect(panels).toHaveCount(3);
    const boxes = await panels.evaluateAll((nodes) =>
      nodes.map((node) => {
        const box = node.getBoundingClientRect();
        return { top: box.top, bottom: box.bottom };
      }),
    );
    for (let index = 1; index < boxes.length; index++)
      expect(Math.abs(boxes[index].top - boxes[index - 1].bottom - 16)).toBeLessThan(1);
    if (width === 1280) {
      const source = (await page.locator(".subscription-grip").first().boundingBox())!;
      await page.mouse.move(source.x + source.width / 2, source.y + source.height / 2);
      await page.mouse.down();
      const target = (await page
        .locator(".subscription-wrapper")
        .nth(1)
        .locator(".subscription-drop-slot--bottom")
        .boundingBox())!;
      await page.mouse.move(target.x + target.width / 2, target.y + target.height / 2, {
        steps: 10,
      });
      await page.mouse.up();
      await expect(page.locator(".subscription-name").nth(0)).toHaveValue("Second");
      await expect(page.locator(".subscription-name").nth(1)).toHaveValue("First");
    }
  });
}

test("drag source survives a document scroll and drops onto a formerly unmounted rule", async ({
  page,
}) => {
  const data = group(0, 1000);
  await backend(page, [data]);
  await page.goto("/");
  const panel = page.locator(".group").first();
  await expect(panel).toBeVisible();
  await panel.locator("[data-collapsible-trigger]").click();
  const source = panel.locator(`.rule[data-uuid="${data.rules[0].id}"]`);
  const handle = source.locator(".grip");
  const start = await handle.boundingBox();
  if (!start) throw new Error("Missing drag handle");
  await page.mouse.move(start.x + start.width / 2, start.y + start.height / 2);
  await page.mouse.down();
  await page.mouse.move(start.x + start.width / 2 + 15, start.y + start.height / 2 + 10, {
    steps: 5,
  });
  await expect(page.locator("html")).toHaveAttribute("data-dnd-scope", "rule");
  await page.evaluate(() => window.scrollTo(0, document.documentElement.scrollHeight));
  const target = panel.locator(`.rule[data-uuid="${data.rules.at(-1)!.id}"]`);
  await expect(target).toBeInViewport();
  await expect(source).toHaveCount(1);
  const end = await target.boundingBox();
  if (!end) throw new Error("Missing drop target");
  await page.mouse.move(end.x + end.width / 2, end.y + end.height * 0.75, { steps: 10 });
  await page.mouse.up();
  const result = await exported(page);
  expect(result.groups[0].rules.at(-1)!.id).toBe(data.rules[0].id);
  expect(result.groups[0].rules[0].id).toBe(data.rules[1].id);
});
