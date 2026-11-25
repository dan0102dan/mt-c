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

const subscription = (index: number, count = 0): Subscription => ({
  ...group(index, count),
  name: `Virtual subscription ${index + 1}`,
  url: `https://example.test/list-${index}.txt`,
  interval: 86400,
  lastUpdate: 1,
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
  for (const kind of ["groups", "subscriptions"] as const) {
    test(`one boundary request reaches the last row across remounted expanded ${kind} at ${width}px`, async ({
      page,
    }) => {
      test.setTimeout(60000);
      await page.setViewportSize({ width, height: 844 });
      const groups = Array.from({ length: kind === "groups" ? 64 : 0 }, (_, index) =>
        group(index, index === 0 || index === 63 ? 1000 : 0),
      );
      const subscriptions = Array.from({ length: kind === "subscriptions" ? 64 : 0 }, (_, index) =>
        subscription(index, index === 0 || index === 63 ? 1000 : 0),
      );
      const items = kind === "groups" ? groups : subscriptions;
      await backend(page, groups, subscriptions);
      await page.goto("/");
      if (kind === "subscriptions") await openTab(page, "Subscriptions");
      const panel = kind === "groups" ? ".group" : ".subscription-panel";
      const row = kind === "groups" ? ".rule" : ".subscription-rule";
      const first = page.locator(`${panel}[data-uuid="${items[0].id}"]`);
      const last = page.locator(`${panel}[data-uuid="${items[63].id}"]`);
      const lastRule = last.locator(`${row}[data-index="999"]`);
      await first.locator("[data-collapsible-trigger]").click();
      await expect(first.locator(row).first()).toBeVisible();
      await page.evaluate(() => window.scrollTo(0, document.documentElement.scrollHeight));
      await last.locator("[data-collapsible-trigger]").click();
      await page.evaluate(() => {
        (document.activeElement as HTMLElement | null)?.blur();
        window.scrollTo(0, document.documentElement.scrollHeight);
      });
      await expect(lastRule).toBeInViewport();
      for (const reducedMotion of ["no-preference", "reduce"] as const) {
        await page.emulateMedia({ reducedMotion });
        for (const modifier of ["Meta", "Control"]) {
          await page.keyboard.press(`${modifier}+ArrowUp`);
          await expect.poll(() => page.evaluate(() => scrollY)).toBeLessThan(1);
          await expect(first.locator(row).first()).toBeInViewport();
          await page.keyboard.press(`${modifier}+ArrowDown`);
          await expect
            .poll(() =>
              page.evaluate(() => document.documentElement.scrollHeight - innerHeight - scrollY),
            )
            .toBeLessThan(1);
          await expect(lastRule).toBeInViewport();
          // Check the settled destination, including the nested list's next measurements.
          await page.waitForTimeout(250);
          expect(
            await page.evaluate(
              () => document.documentElement.scrollHeight - innerHeight - scrollY,
            ),
          ).toBeLessThan(1);
        }
      }
    });
  }
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

  test(`100k fractional-height rules keep document height across virtual ranges at ${width}px`, async ({
    page,
  }) => {
    test.setTimeout(120000);
    await page.setViewportSize({ width, height: width === 390 ? 844 : 900 });
    await backend(page, [group(0, 100000)]);
    await page.goto("/");
    const panel = page.locator(".group").first();
    await panel.locator("[data-collapsible-trigger]").click();
    const first = panel.locator(".rule").first();
    await expect(first).toBeVisible();
    await page.evaluate(async () => {
      await document.fonts.ready;
      (document.activeElement as HTMLElement | null)?.blur();
    });
    // Exercise fractional geometry without depending on a particular font's
    // intrinsic row size. The small minimum only adds empty space to each row.
    const rowHeight = await first.evaluate(
      (node) => Math.ceil(node.getBoundingClientRect().height) + 0.375,
    );
    await page.addStyleTag({ content: `.group .rule { min-height: ${rowHeight}px; }` });

    async function settledGeometry() {
      return page.evaluate(
        () =>
          new Promise<{
            documentHeight: number;
            listHeight: number;
            mounted: number;
            first: number;
            last: number;
            rowHeight: number;
          }>((resolve, reject) => {
            let previous = "";
            let stable = 0;
            let frame = 0;
            const timeout = setTimeout(() => {
              cancelAnimationFrame(frame);
              reject(new Error("Fractional virtual geometry did not settle"));
            }, 8000);
            function sample() {
              const rule = document.querySelector(".group .rule");
              const list = rule?.closest("[data-virtual-index]")?.parentElement;
              const items = list?.querySelectorAll<HTMLElement>(":scope > [data-virtual-index]");
              if (!list || !items?.length) {
                frame = requestAnimationFrame(sample);
                return;
              }
              const result = {
                documentHeight: document.documentElement.scrollHeight,
                listHeight: list.scrollHeight,
                mounted: items.length,
                first: Number(items[0].dataset.virtualIndex),
                last: Number(items[items.length - 1].dataset.virtualIndex),
                rowHeight: items[0].getBoundingClientRect().height,
              };
              const signature = JSON.stringify(result);
              stable = signature === previous ? stable + 1 : 0;
              previous = signature;
              if (stable < 6) frame = requestAnimationFrame(sample);
              else {
                clearTimeout(timeout);
                resolve(result);
              }
            }
            frame = requestAnimationFrame(sample);
          }),
      );
    }

    const initial = await settledGeometry();
    expect(initial.rowHeight).toBe(rowHeight);
    const mountedCounts = new Set([initial.mounted]);
    for (const fraction of [0.01, 0.17, 0.53, 0.91, 1, 0.5, 0]) {
      await page.evaluate(
        (fraction) =>
          window.scrollTo({
            top: (document.documentElement.scrollHeight - innerHeight) * fraction,
            behavior: "instant",
          }),
        fraction,
      );
      const current = await settledGeometry();
      const context = JSON.stringify({ fraction, initial, current });
      expect(current.documentHeight, context).toBe(initial.documentHeight);
      expect(current.listHeight, context).toBe(initial.listHeight);
      expect(current.mounted).toBeGreaterThan(0);
      expect(current.mounted).toBeLessThan(250);
      mountedCounts.add(current.mounted);
    }
    expect(mountedCounts.size).toBeGreaterThan(1);
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

test("repeated Add in a tall group keeps the visible header and pattern stationary", async ({
  page,
}) => {
  const data = group(0, 72);
  await backend(page, [data]);
  await page.goto("/");
  const panel = page.locator(".group").first();
  await panel.locator("[data-collapsible-trigger]").click();
  const pattern = panel.locator('.rule[data-index="0"] .pattern-input');
  await expect(pattern).toBeInViewport();
  const add = panel.locator('[data-value="Add Rule"] button');
  const offsets = await page.evaluateHandle(() => {
    const positions = [scrollY];
    window.addEventListener("scroll", () => positions.push(scrollY));
    return positions;
  });
  for (let i = 0; i < 3; i++) {
    await add.click();
    await expect(pattern).toBeFocused();
    await page.keyboard.insertText(`added${i}.example`);
  }
  await add.dblclick();
  await expect(pattern).toBeFocused();
  await expect(pattern).toHaveValue("");
  await expect(panel.locator('.rule[data-index="2"] .pattern-input')).toHaveValue("added2.example");
  const positions = await offsets.jsonValue();
  expect(Math.max(...positions) - Math.min(...positions)).toBeLessThanOrEqual(1);
  await expect(panel.locator('.rule[data-index="5"] .pattern-input')).toHaveValue(
    data.rules[0].rule,
  );
});

test("only yellow Save pulses the group name with a single underline", async ({ page }) => {
  await backend(page, [group(0)]);
  await page.goto("/");
  const name = page.locator(".group-name");
  const save = page.locator("#save-changes");
  await name.fill("");
  await expect(name).toBeFocused();
  await expect(name).toHaveCSS("animation-name", "none");
  await expect(name).toHaveCSS("background-image", "none");

  await save.click();
  await expect(name).toBeFocused();
  await expect(name).toHaveCSS("animation-name", "save-error-underline");
  await expect(name).toHaveCSS("background-repeat", "no-repeat");
  await expect(name).toHaveCSS("border-bottom-color", "rgba(0, 0, 0, 0)");
  await expect(name).toHaveCSS("background-size", "100% 1px");
  await expect(name).toHaveCSS("outline-style", "none");

  await name.press("Tab");
  await name.click();
  await expect(name).toHaveCSS("animation-name", "none");
  await expect(name).toHaveCSS("background-image", "none");
  await save.click();
  await expect(name).toHaveCSS("animation-name", "save-error-underline");
  await name.fill("Corrected");
  await expect(name).toHaveCSS("animation-name", "none");
  await expect(name).toHaveCSS("background-image", "none");
  await name.fill("");
  await expect(name).toHaveCSS("animation-name", "none");
});

test("yellow Save cycles through invalid rules in collapsed groups outside the search results", async ({
  page,
}) => {
  const groups = Array.from({ length: 1000 }, (_, index) => group(index, 1));
  const invalidRules = groups.slice(-2).map(({ rules: [rule] }) => {
    rule.type = "subnet";
    rule.rule = "invalid-offscreen-subnet";
    return rule;
  });
  await backend(page, groups);
  await page.goto("/");
  await page.locator("input.group-name").first().fill("Dirty draft");
  const search = page.locator(".group-controls-search .search-input");
  await search.fill("Dirty draft");
  await expect(page.locator(".group")).toHaveCount(1);
  const save = page.locator("#save-changes");
  await expect(save).toHaveAttribute("data-save-blocked");
  await expect(save).toHaveAccessibleDescription("Can’t save changes yet");
  let writes = 0;
  page.on("request", (request) => {
    if (request.method() === "PUT") writes++;
  });

  const [firstError, secondError] = invalidRules.map(({ id }) =>
    page.locator(`.rule[data-uuid="${id}"] .pattern-input`),
  );
  await save.press("Enter");
  await expect(firstError).toBeFocused();
  await expect(firstError).toBeInViewport();
  await expect(firstError).toHaveAttribute("aria-invalid", "true");
  await expect(firstError).toHaveCSS("animation-name", "save-error-underline");
  await expect(search).toHaveValue("Dirty draft");

  await save.click();
  await expect(secondError).toBeFocused();
  await save.click();
  await expect(firstError).toBeFocused();
  await firstError.fill("192.0.2.0/24");
  await save.click();
  await expect(secondError).toBeFocused();
  await secondError.fill("198.51.100.0/24");
  await expect(save).not.toHaveAttribute("data-save-blocked");
  await expect(save).toBeEnabled();
  expect(writes).toBe(0);
});

test("yellow Save focuses an invalid rule at the end of a large subscription on mobile", async ({
  page,
}) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await page.emulateMedia({ reducedMotion: "reduce" });
  const sub = subscription(0, 100000);
  sub.rules.at(-1)!.type = "subnet";
  sub.rules.at(-1)!.rule = "invalid-offscreen-subnet";
  await backend(page, [], [sub]);
  await page.goto("/");
  await openTab(page, "Subscriptions");
  const url = page.locator(".subscription-url-input");
  await url.fill("invalid URL");
  const save = page.locator("#save-subscriptions");
  await expect(save).toHaveAttribute("data-save-blocked");
  await save.click();
  await expect(url).toBeFocused();
  await expect(url).toHaveAttribute("aria-invalid", "true");
  await url.fill("https://example.test/edited.txt");

  const invalidRow = page.locator('.subscription-rule[data-index="99999"]');
  const type = invalidRow.locator("[data-select-trigger]");
  await expect(invalidRow).toHaveCount(0);
  await save.click();
  await expect(type).toBeFocused();
  await expect(type).toBeInViewport();
  await expect(type).toHaveAttribute("aria-invalid", "true");
  await expect(type).toHaveCSS("background-size", "100% 1px");
  await expect(type).toHaveCSS("animation-name", "none");
  await expect(invalidRow.locator(".pattern-value")).toHaveText("invalid-offscreen-subnet");
  await expect.poll(() => page.locator(".subscription-rule").count()).toBeLessThan(250);
  await type.click();
  await page.getByRole("option", { name: "Regex", exact: true }).click();
  await expect(save).not.toHaveAttribute("data-save-blocked");
  await expect(save).toBeEnabled();
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

for (const width of [1280, 390]) {
  for (const kind of ["group", "subscription"] as const) {
    test(`expanded ${kind} rule lists retain their measured height after panel remounts at ${width}px`, async ({
      page,
    }) => {
      await page.setViewportSize({ width, height: width === 390 ? 844 : 900 });
      // Two panels can legitimately stay inside a retained virtual range.
      // Separate the expanded panels far enough to require real eviction.
      const counts = Array.from({ length: 64 }, (_, index) =>
        index === 0 || index === 63 ? 1000 : 0,
      );
      const groups = kind === "group" ? counts.map((count, index) => group(index, count)) : [];
      const subscriptions =
        kind === "subscription" ? counts.map((count, index) => subscription(index, count)) : [];
      const data = kind === "group" ? groups : subscriptions;
      const last = data.at(-1)!;
      await backend(page, groups, subscriptions);
      await page.goto("/");
      if (kind === "subscription") await openTab(page, "Subscriptions");
      const panelSelector = kind === "group" ? ".group" : ".subscription-panel";
      const firstPanel = page.locator(`${panelSelector}[data-uuid="${data[0].id}"]`);
      const secondPanel = page.locator(`${panelSelector}[data-uuid="${last.id}"]`);
      const rows = kind === "group" ? ".rule" : ".subscription-rule";
      const firstRule = firstPanel.locator(
        kind === "group" ? `.rule[data-uuid="${data[0].rules[0].id}"]` : `${rows}[data-index="0"]`,
      );
      const lastRule = secondPanel.locator(
        kind === "group"
          ? `.rule[data-uuid="${last.rules.at(-1)!.id}"]`
          : `${rows}[data-index="999"]`,
      );
      await firstPanel.locator("[data-collapsible-trigger]").click();
      await expect(firstRule).toBeVisible();
      await page.evaluate(() => (document.activeElement as HTMLElement | null)?.blur());
      await page.evaluate(() => window.scrollTo(0, document.documentElement.scrollHeight));
      await expect(secondPanel).toBeVisible();
      await secondPanel.locator("[data-collapsible-trigger]").click();
      await page.evaluate(() => (document.activeElement as HTMLElement | null)?.blur());
      await page.evaluate(() => window.scrollTo(0, document.documentElement.scrollHeight));
      await expect(lastRule).toBeInViewport();
      await expect(firstPanel).toHaveCount(0);

      async function settledHeight() {
        return page.evaluate(
          () =>
            new Promise<number>((resolve, reject) => {
              let last = document.documentElement.scrollHeight;
              let stable = 0;
              const timeout = setTimeout(
                () => reject(new Error("Virtual heights did not settle")),
                8000,
              );
              function sample() {
                const height = document.documentElement.scrollHeight;
                stable = height === last ? stable + 1 : 0;
                last = height;
                if (stable < 6) requestAnimationFrame(sample);
                else {
                  clearTimeout(timeout);
                  resolve(height);
                }
              }
              requestAnimationFrame(sample);
            }),
        );
      }

      const height = await settledHeight();
      for (let round = 0; round < 2; round++) {
        await page.evaluate(() => window.scrollTo(0, 0));
        await expect(firstRule).toBeInViewport();
        await expect(secondPanel).toHaveCount(0);
        expect(Math.abs((await settledHeight()) - height)).toBeLessThan(1);
        await page.evaluate(() => window.scrollTo(0, document.documentElement.scrollHeight));
        await expect(lastRule).toBeInViewport();
        await expect(firstPanel).toHaveCount(0);
        expect(Math.abs((await settledHeight()) - height)).toBeLessThan(1);
        await expect.poll(() => page.locator(rows).count()).toBeLessThan(250);
      }
    });

    test(`${kind} rules expand and collapse without height animation at ${width}px`, async ({
      page,
    }) => {
      await page.setViewportSize({ width, height: width === 390 ? 844 : 900 });
      await page.emulateMedia({ reducedMotion: "no-preference" });
      await backend(
        page,
        kind === "group" ? [group(0, 12)] : [],
        kind === "subscription" ? [subscription(0, 12)] : [],
      );
      await page.goto("/");
      if (kind === "subscription") await openTab(page, "Subscriptions");
      const panelSelector = kind === "group" ? ".group" : ".subscription-panel";
      const rowSelector = kind === "group" ? ".rule" : ".subscription-rule";
      const trigger = page.locator(`${panelSelector} [data-collapsible-trigger]`);
      await expect(trigger).toHaveAttribute("aria-expanded", "false");
      for (const expanded of [true, false, true, false]) {
        const result = await trigger.evaluate(
          async (button, { panelSelector, rowSelector }) => {
            (button as HTMLButtonElement).click();
            // Observe the first rendered frame, before a slide transition
            // could complete. Closing must already have unmounted rule nodes.
            await new Promise<void>((resolve) => requestAnimationFrame(() => resolve()));
            const panel = button.closest(panelSelector)!;
            const animations = panel.getAnimations({ subtree: true }).filter((animation) => {
              if (!(animation.effect instanceof KeyframeEffect)) return false;
              return animation.effect
                .getKeyframes()
                .some(
                  (frame) =>
                    "height" in frame || "maxHeight" in frame || "gridTemplateRows" in frame,
                );
            });
            return {
              expanded: button.getAttribute("aria-expanded"),
              controlledContentExists: Boolean(
                document.getElementById(button.getAttribute("aria-controls") ?? ""),
              ),
              rows: panel.querySelectorAll(rowSelector).length,
              heightAnimations: animations.length,
            };
          },
          { panelSelector, rowSelector },
        );
        expect(result.expanded).toBe(String(expanded));
        expect(result.controlledContentExists).toBe(true);
        expect(result.heightAnimations).toBe(0);
        if (expanded) expect(result.rows).toBeGreaterThan(0);
        else expect(result.rows).toBe(0);
      }
    });
  }
}
