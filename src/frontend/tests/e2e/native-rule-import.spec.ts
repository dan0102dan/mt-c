import { readFile } from "node:fs/promises";
import { expect, test, type Locator, type Page } from "@playwright/test";

import type { Group } from "../../src/types";

async function editor(page: Page) {
  const group: Group = {
    id: "11223344",
    name: "Rule import",
    color: "#42bdf9",
    interface: "blackhole",
    enable: true,
    priority: 300,
    rules: [],
  };
  const writes: string[] = [];
  await page.route("**/api/v1/**", (route) => {
    const request = route.request();
    const path = new URL(request.url()).pathname.replace("/api/v1", "");
    if (request.method() !== "GET") writes.push(path);
    const fixtures: Record<string, unknown> = {
      "/auth": { enabled: false },
      "/groups": { groups: [group] },
      "/subscriptions": { subscriptions: [] },
      "/profiles": { profiles: [] },
      "/system/config/snapshot": {
        revision: "a".repeat(64),
        config: {
          configVersion: "test",
          app: {},
          groups: [group],
          subscriptions: [],
          profiles: [],
        },
      },
      "/system/interfaces": { interfaces: [{ id: "blackhole", name: "" }] },
      "/system/update/status": { stage: "idle" },
      "/system/update": { installed_version: "test", can_install: false },
    };
    return route.fulfill({ json: fixtures[path] ?? {} });
  });
  await page.route("https://api.github.com/**", (route) => route.fulfill({ json: [] }));
  await page.goto("/");
  await expect(page.locator("input.group-name")).toHaveValue(group.name);
  const collapse = page.locator(".group [data-collapsible-trigger]");
  if ((await collapse.getAttribute("aria-expanded")) === "false") await collapse.click();
  const trigger = page.locator('[data-value="Import Rule List"] button');
  const open = async () => {
    await trigger.click();
    const dialog = page.locator("[data-dialog-content]");
    await expect(dialog).toBeVisible();
    return dialog;
  };
  return { open, writes };
}

// Playwright.fill uses native insertText for the complete value; on a plain
// textarea that path itself stalls at 100k lines, independently of this app.
// Seed the bulk value atomically, then exercise real keyboard edits below.
async function seedBulkText(textarea: Locator, text: string) {
  await textarea.evaluate((node: HTMLTextAreaElement, value) => {
    node.focus();
    node.value = value;
    node.dispatchEvent(
      new InputEvent("input", { bubbles: true, inputType: "insertFromPaste", data: null }),
    );
  }, text);
}

const largeText = () =>
  Array.from({ length: 100000 }, (_, index) => `item${index}.example.test`).join("\n");

test("100k native rule import keeps the editor buffer, limits preview DOM and imports edits in full", async ({
  page,
}) => {
  test.setTimeout(120000);
  const { open, writes } = await editor(page);
  const dialog = await open();
  const textarea = dialog.locator("textarea");
  const originalEditor = await textarea.elementHandle();
  await seedBulkText(textarea, largeText());
  await textarea.blur();
  const preview = dialog.locator(".results-view");
  await expect(preview).toBeVisible();
  await expect(dialog.locator(".count-val")).toHaveText("100000");
  expect(await preview.locator(".line-row").count()).toBeLessThan(80);
  const documentY = await page.evaluate(() => scrollY);
  await preview.evaluate((node) => node.scrollTo(0, node.scrollHeight));
  await expect(
    preview.locator(".line-row").filter({ hasText: "item499.example.test" }),
  ).toBeVisible();
  expect(await page.evaluate(() => scrollY)).toBe(documentY);
  await preview.click();
  await expect(textarea).toBeFocused();
  await textarea.blur();
  await expect(preview.locator(".line-row").first()).toContainText("item0.example.test");
  await preview.click();
  await expect(textarea).toBeFocused();
  expect(
    await originalEditor!.evaluate(
      (node) => node === document.querySelector("[data-dialog-content] textarea"),
    ),
  ).toBe(true);
  await expect(textarea).toHaveAttribute("spellcheck", "false");
  await expect(textarea).toHaveAttribute("autocorrect", "off");
  await textarea.press("Control+Home");
  await page.keyboard.insertText("edited.");
  await expect(textarea).toHaveValue(/^edited\.item0\.example\.test/);
  await textarea.blur();
  await expect(preview).toBeVisible();
  await dialog.getByRole("button", { name: "Import", exact: true }).click();
  await expect(dialog).toBeHidden();
  await expect(page.locator("#save-changes")).toBeEnabled();
  const downloading = page.waitForEvent("download");
  await page.getByRole("button", { name: "Export Config", exact: true }).click();
  const path = await (await downloading).path();
  const exported = JSON.parse(await readFile(path!, "utf8")) as { groups: Group[] };
  const rules = exported.groups[0].rules;
  expect(rules).toHaveLength(100000);
  expect(rules[0].rule).toBe("edited.item0.example.test");
  expect(rules.at(-1)!.rule).toBe("item99999.example.test");
  expect(new Set(rules.map((rule) => rule.id)).size).toBe(100000);
  expect(writes).toEqual([]);
});

test("closing a pending native rule preview cannot replace the next session", async ({ page }) => {
  const { open, writes } = await editor(page);
  let dialog = await open();
  await seedBulkText(dialog.locator("textarea"), largeText());
  await dialog.locator("textarea").blur();
  await page.keyboard.press("Escape");
  await expect(dialog).toBeHidden();
  dialog = await open();
  await dialog.locator("textarea").fill("fresh.example.test\n[\nfresh.example.test");
  await dialog.locator("textarea").blur();
  await expect(dialog.locator(".results-view")).toBeVisible();
  await expect(dialog.locator(".line-row")).toHaveCount(3);
  await expect(dialog.locator(".count-badge", { hasText: "namespace" })).toContainText("2");
  await expect(dialog.locator(".count-badge", { hasText: "INVALID" })).toContainText("1");
  await dialog.getByRole("button", { name: "Import", exact: true }).click();
  await expect(dialog).toBeHidden();
  await expect(page.locator(".pattern-input")).toHaveCount(1);
  await expect(page.locator(".pattern-input")).toHaveValue("fresh.example.test");
  expect(writes).toEqual([]);
});

test("changing the type while editing invalidates a cached native preview", async ({ page }) => {
  const { open } = await editor(page);
  const dialog = await open();
  const textarea = dialog.locator("textarea");
  await textarea.fill("first.example.test\nsecond.example.test");
  await textarea.blur();
  await expect(dialog.locator(".count-badge", { hasText: "namespace" })).toContainText("2");
  await dialog.locator(".results-view").click();
  await expect(textarea).toBeFocused();
  await dialog.locator("[data-select-trigger]").click();
  await page.getByRole("option", { name: "Domain", exact: true }).click();
  await expect(dialog.locator(".count-badge", { hasText: "domain" })).toContainText("2");
  await dialog.getByRole("button", { name: "Import", exact: true }).click();
  await expect(dialog).toBeHidden();
  await expect(page.locator(".rule")).toHaveCount(2);
  await expect(page.locator(".rule").first()).toContainText("Domain");
});

test("a failed native bulk apply retains its parsed input for retry", async ({ page }) => {
  test.setTimeout(120000);
  const { open } = await editor(page);
  const dialog = await open();
  await seedBulkText(dialog.locator("textarea"), largeText());
  await dialog.locator("textarea").blur();
  await expect(dialog.locator(".results-view")).toBeVisible();
  const submit = dialog.getByRole("button", { name: "Import", exact: true });
  await submit.click();
  await expect(submit).toBeDisabled();
  // A background editor mutation during cooperative preparation invalidates
  // that transaction. The modal must retain the user's complete source.
  await page.locator("input.group-name").evaluate((node: HTMLInputElement) => {
    node.value = "Changed during import";
    node.dispatchEvent(new Event("input", { bubbles: true }));
  });
  await expect(submit).toBeEnabled();
  await expect(dialog).toBeVisible();
  await expect(dialog.locator(".results-view")).toBeVisible();
  await expect(dialog.locator(".count-val")).toHaveText("100000");
  await dialog.locator(".results-view").click();
  await expect(dialog.locator("textarea")).toBeFocused();
  await expect(dialog.locator("textarea")).toHaveValue(/^item0\.example\.test/);
  expect(
    await dialog
      .locator("textarea")
      .evaluate((node: HTMLTextAreaElement) => node.value.split("\n").length),
  ).toBe(100000);
});

test("trusted 100k paste retains native Undo and Redo after returning from preview", async ({
  page,
  browserName,
}) => {
  test.skip(browserName !== "chromium", "Clipboard permission automation is available in Chromium");
  test.setTimeout(120000);
  await page.context().grantPermissions(["clipboard-read", "clipboard-write"]);
  const { open } = await editor(page);
  const dialog = await open();
  const textarea = dialog.locator("textarea");
  await page.evaluate((text) => navigator.clipboard.writeText(text), largeText());
  await textarea.focus();
  await page.keyboard.press("Control+V");
  await expect(dialog.locator(".results-view")).toBeVisible();
  await expect(dialog.locator(".count-val")).toHaveText("100000");
  await dialog.locator(".results-view").click();
  await expect(textarea).toBeFocused();
  await textarea.press("Control+Z");
  await expect(textarea).toHaveValue("");
  await textarea.press("Control+Shift+Z");
  await expect(textarea).toHaveValue(/^item0\.example\.test/);
  await textarea.blur();
  await expect(dialog.locator(".count-val")).toHaveText("100000");
});

test("typing while a bulk preview is pending cancels it and keeps the latest editor text", async ({
  page,
}) => {
  test.setTimeout(120000);
  let releaseFirstWorker!: () => void;
  const held = new Promise<void>((resolve) => {
    releaseFirstWorker = resolve;
  });
  let first = true;
  await page.route("**/rule-list-parser.worker*", async (route) => {
    if (first) {
      first = false;
      await held;
    }
    try {
      await route.continue();
    } catch (error) {
      if (!(error instanceof Error) || !/closed|cancell?ed|already handled/i.test(error.message))
        throw error;
    }
  });
  try {
    const { open } = await editor(page);
    const dialog = await open();
    const textarea = dialog.locator("textarea");
    await seedBulkText(textarea, largeText());
    await textarea.dispatchEvent("paste");
    await expect(dialog.locator(".parsing-overlay")).toBeVisible();
    await expect.poll(() => first).toBe(false);
    await expect(textarea).toBeEnabled();
    await expect(textarea).toBeFocused();
    await textarea.press("Control+Home");
    await page.keyboard.insertText("latest.");
    await expect(dialog.locator(".parsing-overlay")).toBeHidden();
    await expect(textarea).toBeFocused();
    await expect(textarea).toHaveValue(/^latest\.item0\.example\.test/);
    releaseFirstWorker();
    await textarea.blur();
    await expect(dialog.locator(".results-view")).toBeVisible();
    await expect(dialog.locator(".results-view .line-row").first()).toContainText(
      "latest.item0.example.test",
    );
    await expect(dialog.locator(".count-val")).toHaveText("100000");
  } finally {
    releaseFirstWorker();
  }
});

test("continued typing cancels the scheduled automatic paste preview", async ({ page }) => {
  await page.clock.install();
  const { open } = await editor(page);
  const dialog = await open();
  const textarea = dialog.locator("textarea");
  await textarea.fill("pasted.example.test");
  await page.clock.pauseAt(await page.evaluate(() => Date.now() + 1000));
  await textarea.dispatchEvent("paste");
  await textarea.press("End");
  await page.keyboard.insertText("x");
  await page.clock.runFor(100);
  await expect(dialog.locator(".parsing-overlay")).toBeHidden();
  await expect(dialog.locator(".results-view")).toBeHidden();
  await expect(textarea).toBeFocused();
  await expect(textarea).toHaveValue("pasted.example.testx");
  await page.clock.resume();
  await textarea.blur();
  await expect(dialog.locator(".results-view")).toBeVisible();
  await expect(dialog.locator(".line-row")).toContainText("pasted.example.testx");
});
