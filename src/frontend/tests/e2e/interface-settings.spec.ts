import fs from "node:fs/promises";
import { expect, test, type Page } from "@playwright/test";

import type { PortableConfig } from "../../src/modules/config-transfer/model";
import type { Group, Profile, Subscription } from "../../src/types";
import { generalServerConfig, generalSettings } from "../mocks/general-settings";
import { mockBackgroundApi } from "./fixtures/background-api";
import { openInterface, selectLanguage } from "./helpers/interface";

async function backend(page: Page) {
  const config = generalServerConfig();
  config.groups[0].priority = 650;
  config.subscriptions[0].priority = 175;
  const groups = structuredClone(config.groups);
  let profiles = structuredClone(config.profiles ?? []);
  const subscriptions: Subscription[] = config.subscriptions.map(({ last_update, ...item }) => ({
    ...item,
    lastUpdate: last_update,
    rules: item.rules ?? [],
  }));
  const writes: { path: string; body: unknown }[] = [];

  await mockBackgroundApi(page);
  await page.route("**/api/v1/**", async (route) => {
    const request = route.request();
    const path = new URL(request.url()).pathname.replace("/api/v1", "");
    if (request.method() === "OPTIONS") return route.fulfill({ status: 204 });
    if (!["GET", "HEAD"].includes(request.method()))
      writes.push({ path, body: request.postDataJSON() });

    if (path === "/auth") return route.fulfill({ json: { enabled: false } });
    if (path === "/system/interfaces")
      return route.fulfill({ json: { interfaces: [{ id: "tun0" }, { id: "tun1" }] } });
    if (path === "/profiles") {
      if (request.method() === "PUT")
        profiles = (request.postDataJSON() as { profiles: Profile[] }).profiles;
      return route.fulfill({ json: { profiles } });
    }
    if (path === "/keenetic/policy")
      return route.fulfill({ json: { enabled: false, policy_id: "", mode: "include" } });
    if (path === "/keenetic/policies") return route.fulfill({ json: { policies: [] } });
    if (path === "/system/settings")
      return route.fulfill({
        json: { settings: generalSettings(), revision: "1".repeat(64), operation: null },
      });
    if (path === "/system/config/snapshot")
      return route.fulfill({ json: { revision: "1".repeat(64), config } });
    if (path === "/groups") {
      if (request.method() === "PUT") {
        const incoming = (request.postDataJSON() as { groups: Group[] }).groups[0];
        groups[0] = { ...groups[0], ...incoming, rules: groups[0].rules };
      }
      return route.fulfill({ json: { groups } });
    }
    if (path === "/subscriptions") {
      if (request.method() === "PUT") {
        const incoming = (request.postDataJSON() as { subscriptions: Subscription[] })
          .subscriptions[0];
        subscriptions[0] = {
          ...subscriptions[0],
          ...incoming,
          rules: subscriptions[0].rules,
        };
        return route.fulfill({ json: { status: "ok" } });
      }
      return route.fulfill({ json: { subscriptions } });
    }
    return route.fallback();
  });
  await page.goto("/");
  await expect(page.locator(".group-name")).toHaveValue("Routing group");
  return { writes };
}

async function navigate(page: Page, name: string) {
  const tab = page.locator("nav").getByRole("tab", { name, exact: true });
  await tab.click();
}

async function exportConfig(page: Page): Promise<PortableConfig> {
  const pending = page.waitForEvent("download");
  await page.getByRole("button", { name: "Export Config", exact: true }).click();
  const file = await pending;
  expect(file.suggestedFilename()).toBe("config.mtrickle");
  return JSON.parse(await fs.readFile((await file.path())!, "utf8"));
}

for (const width of [1280, 390]) {
  test(`priority visibility is independent, local and persistent at ${width}px`, async ({
    page,
  }) => {
    await page.setViewportSize({ width, height: 900 });
    const { writes } = await backend(page);
    const groupPriority = page.getByRole("button", { name: "Group priority: Routing group" });
    const subscriptionPriority = page.getByRole("button", {
      name: "Subscription priority: Domain subscription",
    });
    await expect(groupPriority).toHaveText("650");
    await navigate(page, "Subscriptions");
    await expect(subscriptionPriority).toHaveText("175");
    await openInterface(page);

    const groups = page.getByRole("switch", { name: "Show group priorities", exact: true });
    const subscriptions = page.getByRole("switch", {
      name: "Show subscription priorities",
      exact: true,
    });
    await expect(groups).toBeChecked();
    await expect(subscriptions).toBeChecked();
    await groups.click();
    await expect(groups).not.toBeChecked();
    await expect(page.locator("#save-settings")).not.toBeVisible();
    await expect(page.getByRole("button", { name: "Import Config", exact: true })).toHaveCount(0);
    await expect(page.getByRole("button", { name: "Export Config", exact: true })).toHaveCount(0);

    await navigate(page, "Groups");
    await expect(groupPriority).toHaveCount(0);
    await expect(page.locator("#save-changes")).toBeDisabled();
    await navigate(page, "Subscriptions");
    await expect(subscriptionPriority).toHaveText("175");
    await expect(page.locator("#save-subscriptions")).toBeDisabled();
    await openInterface(page);
    await subscriptions.click();
    await expect(subscriptions).not.toBeChecked();

    await page.reload();
    await expect(page.getByRole("tab", { name: "Interface", exact: true })).toHaveAttribute(
      "aria-selected",
      "true",
    );
    await expect(groups).not.toBeChecked();
    await expect(subscriptions).not.toBeChecked();
    await navigate(page, "Groups");
    await expect(groupPriority).toHaveCount(0);
    await navigate(page, "Subscriptions");
    await expect(subscriptionPriority).toHaveCount(0);

    await openInterface(page);
    await groups.click();
    await navigate(page, "Groups");
    await expect(groupPriority).toHaveText("650");
    await navigate(page, "Subscriptions");
    await expect(subscriptionPriority).toHaveCount(0);
    await openInterface(page);
    await subscriptions.click();
    await navigate(page, "Subscriptions");
    await expect(subscriptionPriority).toHaveText("175");
    expect(writes).toEqual([]);
  });
}

for (const kind of ["groups", "subscriptions"] as const) {
  test(`${kind}: local visibility changes preserve a routing draft and its saved priority`, async ({
    page,
  }) => {
    const { writes } = await backend(page);
    const label = kind === "groups" ? "Groups" : "Subscriptions";
    await navigate(page, label);
    const route = page
      .locator(kind === "groups" ? ".group-header" : ".subscription-actions")
      .getByLabel("Route", { exact: true });
    await route.click();
    await page.getByRole("option", { name: "tun1", exact: true }).click();
    const save = page.locator(kind === "groups" ? "#save-changes" : "#save-subscriptions");
    await expect(save).toBeEnabled();

    await openInterface(page);
    const toggle = page.getByRole("switch", {
      name: kind === "groups" ? "Show group priorities" : "Show subscription priorities",
      exact: true,
    });
    await toggle.click();
    await expect(toggle).not.toBeChecked();
    await page.keyboard.press("Control+s");
    await navigate(page, label);
    await expect(route).toHaveText("tun1");
    await expect(save).toBeEnabled();
    expect(writes).toEqual([]);

    await save.click();
    await expect(save).toBeDisabled();
    expect(writes).toHaveLength(1);
    expect(writes[0]).toMatchObject({
      path: `/${kind}`,
      body: {
        [kind]: [
          {
            interface: "tun1",
            priority: kind === "groups" ? 650 : 175,
            ruleChanges: [],
          },
        ],
      },
    });
    await page.reload();
    await expect(route).toHaveText("tun1");
    await expect(save).toBeDisabled();
    await expect(page.locator(".priority-trigger:visible")).toHaveCount(0);
    await openInterface(page);
    await toggle.click();
    await navigate(page, label);
    await expect(page.locator(".priority-trigger:visible")).toHaveText(
      kind === "groups" ? "650" : "175",
    );
  });
}

test("Ctrl+S in Interface does not save an existing profile draft", async ({ page }) => {
  const { writes } = await backend(page);
  await navigate(page, "Settings");
  await page.getByRole("tab", { name: "Profiles", exact: true }).click();
  const name = page.getByLabel("Profile name", { exact: true });
  await name.fill("Unsaved VPN");
  await expect(page.locator("#save-settings")).toBeEnabled();
  await openInterface(page);
  await page.getByRole("switch", { name: "Show group priorities", exact: true }).click();
  await page.keyboard.press("Control+s");
  await page.getByRole("tab", { name: "Profiles", exact: true }).click();
  await expect(name).toHaveValue("Unsaved VPN");
  await expect(page.locator("#save-settings")).toBeEnabled();
  expect(writes).toEqual([]);
  await page.locator("#save-settings").click();
  await expect(page.locator("#save-settings")).toBeDisabled();
  expect(writes).toHaveLength(1);
  expect(writes[0]).toMatchObject({
    path: "/profiles",
    body: { profiles: [{ id: "11223344", name: "Unsaved VPN", interfaces: ["tun0"] }] },
  });
});

test("malformed local preferences keep priorities visible without changing router data", async ({
  page,
}) => {
  await page.addInitScript(() => {
    localStorage.setItem("show_group_priority", "null");
    localStorage.setItem("show_subscription_priority", JSON.stringify({ value: "false" }));
  });
  const { writes } = await backend(page);
  await expect(page.getByRole("button", { name: "Group priority: Routing group" })).toHaveText(
    "650",
  );
  await navigate(page, "Subscriptions");
  await expect(
    page.getByRole("button", { name: "Subscription priority: Domain subscription" }),
  ).toHaveText("175");
  await openInterface(page);
  await expect(
    page.getByRole("switch", { name: "Show group priorities", exact: true }),
  ).toBeChecked();
  await expect(
    page.getByRole("switch", { name: "Show subscription priorities", exact: true }),
  ).toBeChecked();
  expect(writes).toEqual([]);
});

test("language is saved locally and interface preferences never enter a configuration export", async ({
  page,
}) => {
  const { writes } = await backend(page);
  const { exportedAt: beforeTime, ...before } = await exportConfig(page);
  expect(beforeTime).toBeDefined();
  await openInterface(page);
  await page.getByRole("switch", { name: "Show group priorities", exact: true }).click();
  await page.getByRole("switch", { name: "Show subscription priorities", exact: true }).click();
  await selectLanguage(page, "Русский");
  await expect(page.getByRole("tab", { name: "Интерфейс", exact: true })).toHaveAttribute(
    "aria-selected",
    "true",
  );
  await page.reload();
  await expect(page.getByRole("tab", { name: "Интерфейс", exact: true })).toHaveAttribute(
    "aria-selected",
    "true",
  );
  const language = page.locator("#interface").getByLabel("Язык", { exact: true });
  await expect(language).toHaveText("Русский");
  await selectLanguage(page, "English");
  await navigate(page, "Groups");
  const { exportedAt: afterTime, ...after } = await exportConfig(page);
  expect(afterTime).toBeDefined();
  expect(after).toEqual(before);
  expect(writes).toEqual([]);
});
