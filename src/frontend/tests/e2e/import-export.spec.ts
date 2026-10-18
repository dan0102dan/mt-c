import fs from "fs";
import path from "path";
import { expect, test } from "@playwright/test";

import { GroupsPage } from "./pages/GroupsPage";

test.describe("Import/Export", () => {
  let groupsPage: GroupsPage;
  let serverGroups: any[];
  let applied = 0;

  test.beforeEach(async ({ page }) => {
    serverGroups = [];
    applied = 0;
    groupsPage = new GroupsPage(page);
    const app = {
      httpWeb: {
        enabled: true,
        auth: { enabled: false },
        host: { address: "127.0.0.1", port: 6969 },
        skin: "",
      },
      dnsProxy: {
        host: { address: "0.0.0.0", port: 53 },
        upstream: { address: "8.8.8.8", port: 53 },
        disableRemap53: false,
        disableFakePTR: false,
        disableDropAAAA: false,
        maxIdleConns: 128,
        maxConcurrent: 128,
        timeout: "10s",
      },
      netfilter: {
        iptables: { chainPrefix: "MT_" },
        ipset: { tablePrefix: "mt_", additionalTTL: "30s" },
        disableIPv4: false,
        disableIPv6: false,
        startMarkTableIndex: 150,
      },
      link: [],
      showAllInterfaces: false,
      logLevel: "info",
    };
    let serverSubs: any[] = [];
    let serverProfiles: any[] = [];
    await page.route("**/auth", async (route) => route.fulfill({ json: { enabled: false } }));
    await page.route("**/groups?with_rules=true", async (route) =>
      route.fulfill({ json: { groups: serverGroups } }),
    );
    await page.route("**/subscriptions", async (route) =>
      route.fulfill({ json: { subscriptions: serverSubs } }),
    );
    await page.route("**/profiles", async (route) =>
      route.fulfill({ json: { profiles: serverProfiles } }),
    );
    await page.route("**/system/interfaces", async (route) =>
      route.fulfill({ json: { interfaces: [{ id: "wg0" }, { id: "blackhole" }] } }),
    );
    await page.route("**/system/config/snapshot", async (route) =>
      route.fulfill({
        json: {
          revision: String(applied),
          config: {
            configVersion: "0.8.5",
            app,
            groups: serverGroups,
            profiles: serverProfiles,
            subscriptions: serverSubs,
          },
        },
      }),
    );
    await page.route("**/system/config/apply", async (route) => {
      const body = route.request().postDataJSON();
      if (body.revision !== String(applied)) {
        await route.fulfill({ status: 409, json: { error: "preview stale" } });
        return;
      }
      serverGroups = body.config.groups;
      serverProfiles = body.config.profiles;
      serverSubs = body.config.subscriptions.map((s: any) => ({
        ...s,
        lastUpdate: s.last_update ?? 0,
      }));
      Object.assign(app, body.config.app);
      applied++;
      await route.fulfill({ json: { ok: true } });
    });
    await groupsPage.goto();
  });

  test("exports unsaved group draft immediately, without confirmation", async ({ page }) => {
    await groupsPage.createGroup();
    const downloadPromise = page.waitForEvent("download");
    await page.locator(".group-controls-actions button").nth(2).click();
    const download = await downloadPromise;
    expect(download.suggestedFilename()).toBe("config.mtrickle");
    const payload = JSON.parse(fs.readFileSync(await download.path(), "utf8"));
    expect(payload.format).toBe("mt-c-config");
    expect(payload.groups.length).toBe(1);
    expect(payload).toHaveProperty("profiles");
    expect(payload).toHaveProperty("subscriptions");
    expect(payload).not.toHaveProperty("settings");
    expect(payload.subscriptions).toEqual([]);
    expect(payload.profiles).toEqual([]);
    expect(applied).toBe(0);
    await expect(page.getByRole("dialog")).toHaveCount(0);
  });

  test("previews and applies legacy group-only import without erasing other sections", async ({
    page,
  }) => {
    const configData = {
      groups: [
        {
          id: "12345678",
          name: "Imported Group",
          rules: [],
          color: "#123456",
          enable: true,
          interface: "wg0",
        },
      ],
    };
    const filePath = path.join("tests", "e2e", "fixtures", "config.mtrickle");
    const fileChooserPromise = page.waitForEvent("filechooser");
    await page.locator(".group-controls-actions button").nth(1).click();
    const fileChooser = await fileChooserPromise;
    await fileChooser.setFiles({
      name: path.basename(filePath),
      mimeType: "application/json",
      buffer: Buffer.from(JSON.stringify(configData)),
    });

    const dialog = page.getByRole("dialog");
    await expect(dialog).toBeVisible();
    await expect(dialog.getByRole("heading", { name: "Import Config" })).toBeVisible();
    await expect(dialog.getByRole("tab", { name: "Groups", exact: true })).toBeVisible();
    await expect(dialog.getByText("Imported Group", { exact: true })).toBeVisible();
    expect(applied).toBe(0);
    await dialog.getByRole("button", { name: "Import", exact: true }).click();
    await expect(dialog).toHaveCount(0);
    await expect(page.locator(".group-wrapper")).toHaveCount(1);
    await expect(page.locator("input.group-name")).toHaveValue("Imported Group");
    expect(applied).toBe(1);
  });

  test("shows identical transfer actions on all three tabs", async ({ page }) => {
    for (const tab of ["groups", "subscriptions", "settings"]) {
      await page.locator(`[data-tabs-trigger][data-value="${tab}"]`).count();
    }
    await expect(
      page.locator(".group-controls-actions button[aria-label='Экспорт конфига']"),
    ).toHaveCount(1);
    await page.getByRole("tab", { name: /Подписки|Subscriptions/ }).click();
    await expect(
      page.locator(".subscription-controls-actions button[aria-label='Экспорт конфига']"),
    ).toBeVisible();
    await page.getByRole("tab", { name: /Настройки|Settings/ }).click();
    await expect(
      page.locator(".settings-toolbar button[aria-label='Экспорт конфига']"),
    ).toBeVisible();
  });

  for (const source of ["subscriptions", "settings"] as const) {
    test(`defaults to ${source} and preserves other server data and drafts`, async ({ page }) => {
      serverGroups = [
        {
          id: "33445566",
          name: "Existing",
          rules: [],
          color: "#123456",
          enable: true,
          interface: "wg0",
        },
      ];
      await page.reload();
      await page.locator("input.group-name").fill("Unsaved name");
      await page
        .getByRole("tab", {
          name: source === "settings" ? "Settings" : "Subscriptions",
          exact: true,
        })
        .click();
      const chooser = page.waitForEvent("filechooser");
      await page.getByRole("button", { name: "Import Config", exact: true }).click();
      await (
        await chooser
      ).setFiles({
        name: "config.mtrickle",
        mimeType: "application/json",
        buffer: Buffer.from(
          JSON.stringify({
            groups: [
              {
                id: "11223344",
                name: "Do not import",
                rules: [],
                color: "#123456",
                enable: true,
                interface: "wg0",
                profile: "p_missing",
              },
            ],
            subscriptions: [
              {
                id: "55667788",
                name: "Clouds",
                url: "https://example.org/list",
                interface: "wg0",
                enable: true,
                priority: 100,
                interval: 86400,
              },
            ],
            profiles: [
              { id: "vpn", name: "VPN", interfaces: ["wg0"], on_unavailable: "blackhole" },
            ],
            settings: { logLevel: "debug" },
          }),
        ),
      });
      const dialog = page.getByRole("dialog");
      const name = source === "settings" ? "Settings" : "Subscriptions";
      await expect(dialog.getByRole("tab", { name, exact: true })).toHaveAttribute(
        "aria-selected",
        "true",
      );
      await expect(
        dialog.getByRole("checkbox", {
          name: source === "settings" ? "VPN" : "Clouds",
          exact: true,
        }),
      ).toBeChecked();
      await dialog.getByRole("tab", { name: "Groups", exact: true }).click();
      await expect(
        dialog.getByRole("checkbox", { name: "Do not import", exact: true }),
      ).not.toBeChecked();
      await dialog.getByRole("tab", { name, exact: true }).click();
      await expect(dialog.getByRole("button", { name: "Import", exact: true })).toBeEnabled();
      const request = page.waitForRequest((req) => req.url().endsWith("/system/config/apply"));
      await dialog.getByRole("button", { name: "Import", exact: true }).click();
      const payload = (await request).postDataJSON();
      expect(payload.config.groups.map((group: { name: string }) => group.name)).toEqual([
        "Existing",
      ]);
      expect(payload.config.subscriptions).toHaveLength(source === "subscriptions" ? 1 : 0);
      expect(payload.config.app.logLevel).toBe("info");
      expect(payload.config.profiles).toHaveLength(source === "settings" ? 1 : 0);
      await expect(dialog).toHaveCount(0);
      await page.getByRole("tab", { name: "Groups", exact: true }).click();
      await expect(page.locator("input.group-name")).toHaveValue("Unsaved name");
    });
  }
  for (const width of [1280, 390]) {
    test(`selects individual objects from every section at ${width}px`, async ({ page }) => {
      await page.setViewportSize({ width, height: width === 390 ? 844 : 900 });
      serverGroups = [
        {
          id: "33445566",
          name: "Existing",
          rules: [],
          color: "#123456",
          enable: true,
          interface: "wg0",
        },
      ];
      await page.reload();
      const chooser = page.waitForEvent("filechooser");
      await page.locator(".group-controls-actions button").nth(1).click();
      await (
        await chooser
      ).setFiles({
        name: "config.mtrickle",
        mimeType: "application/json",
        buffer: Buffer.from(
          JSON.stringify({
            groups: [
              {
                id: "11223344",
                name: "Video",
                rules: [],
                color: "#123456",
                enable: true,
                interface: "wg0",
                profile: "vpn",
              },
              {
                id: "22334455",
                name: "Skip group",
                rules: [],
                color: "#123456",
                enable: true,
                interface: "wg0",
              },
            ],
            subscriptions: [
              {
                id: "55667788",
                name: "Clouds",
                url: "https://example.org/list",
                interface: "wg0",
                enable: true,
                priority: 100,
                interval: 86400,
              },
            ],
            profiles: [
              { id: "vpn", name: "VPN", interfaces: ["wg0"], on_unavailable: "blackhole" },
              { id: "spare", name: "Spare", interfaces: ["wg0"], on_unavailable: "blackhole" },
            ],
            settings: { logLevel: "debug" },
          }),
        ),
      });
      const dialog = page.getByRole("dialog");
      await expect(dialog.getByText("config.mtrickle", { exact: false })).toHaveCount(0);
      await expect(dialog.getByText("Will import:", { exact: false })).toHaveCount(0);
      await expect(dialog.getByRole("tablist").getByRole("checkbox")).toHaveCount(0);
      const groupsTab = dialog.getByRole("tab", { name: "Groups", exact: true });
      await groupsTab.focus();
      await page.keyboard.press("ArrowRight");
      await expect(dialog.getByRole("tab", { name: "Subscriptions", exact: true })).toHaveAttribute(
        "aria-selected",
        "true",
      );
      await expect(dialog.getByRole("checkbox", { name: "Clouds", exact: true })).not.toBeChecked();
      await page.keyboard.press("ArrowLeft");
      await expect(dialog.getByRole("checkbox", { name: "Video", exact: true })).toBeChecked();
      await expect(dialog.getByText("2 / 2", { exact: true })).toBeVisible();
      await dialog.getByRole("button", { name: "Reset", exact: true }).click();
      await expect(dialog.getByRole("checkbox", { name: "Video", exact: true })).not.toBeChecked();
      await expect(dialog.getByRole("button", { name: "Import", exact: true })).toBeDisabled();
      await dialog.getByRole("button", { name: "All", exact: true }).click();
      await dialog.getByRole("checkbox", { name: "Skip group", exact: true }).uncheck();
      await dialog.getByRole("tab", { name: "Settings", exact: true }).click();
      for (const name of ["VPN", "Spare"]) {
        await expect(dialog.getByRole("checkbox", { name, exact: true })).not.toBeChecked();
      }
      await expect(dialog.getByRole("checkbox", { name: "Log level", exact: true })).toHaveCount(0);
      const subscriptionsTab = dialog.getByRole("tab", { name: "Subscriptions", exact: true });
      await subscriptionsTab.click({ position: { x: 2, y: 2 } });
      await expect(dialog.getByRole("checkbox", { name: "Clouds", exact: true })).not.toBeChecked();
      await dialog.getByText("Clouds", { exact: true }).click();
      await expect(dialog.getByRole("checkbox", { name: "Clouds", exact: true })).toBeChecked();
      await dialog.getByRole("button", { name: "Reset", exact: true }).click();
      await expect(subscriptionsTab).toHaveAttribute("aria-selected", "true");
      await groupsTab.click();
      await expect(dialog.getByRole("checkbox", { name: "Video", exact: true })).toBeChecked();
      await expect(
        dialog.getByRole("checkbox", { name: "Skip group", exact: true }),
      ).not.toBeChecked();
      await subscriptionsTab.click();
      await expect(dialog.getByRole("checkbox", { name: "Clouds", exact: true })).not.toBeChecked();
      await dialog.getByRole("button", { name: "All", exact: true }).click();
      for (const region of [
        dialog,
        dialog.locator(".import-config"),
        dialog.getByRole("tablist"),
      ]) {
        expect(
          await region.evaluate((element) => element.scrollWidth - element.clientWidth),
        ).toBeLessThanOrEqual(1);
      }
      if (width === 390) await dialog.getByRole("switch", { name: "Replace", exact: true }).click();
      // The selected group still brings its required profile, even when unchecked separately.
      const request = page.waitForRequest((req) => req.url().endsWith("/system/config/apply"));
      await dialog.getByRole("button", { name: "Import", exact: true }).click();
      const payload = (await request).postDataJSON();
      expect(payload.config.groups.map((item: { name: string }) => item.name)).toEqual(
        width === 390 ? ["Video"] : ["Existing", "Video"],
      );
      expect(payload.config.subscriptions.map((item: { name: string }) => item.name)).toEqual([
        "Clouds",
      ]);
      expect(payload.config.profiles.map((item: { name: string }) => item.name)).toEqual(["VPN"]);
      expect(payload.config.app.logLevel).toBe("info");
      await expect(dialog).toHaveCount(0);
      expect(applied).toBe(1);
    });
  }
  for (const width of [1280, 390]) {
    test(`imports unavailable interfaces unchanged and allows editing at ${width}px`, async ({
      page,
    }) => {
      await page.setViewportSize({ width, height: width === 390 ? 844 : 900 });
      const chooser = page.waitForEvent("filechooser");
      await page.locator(".group-controls-actions button").nth(1).click();
      await (
        await chooser
      ).setFiles({
        name: "config.mtrickle",
        mimeType: "application/json",
        buffer: Buffer.from(
          JSON.stringify({
            groups: [
              {
                id: "12345678",
                name: "Imported Group",
                rules: [],
                color: "#123456",
                enable: true,
                interface: "mitun0",
              },
            ],
            subscriptions: [
              {
                id: "22334455",
                name: "Imported Subscription",
                url: "https://example.org/list",
                interface: "offline0",
                enable: true,
                priority: 100,
                interval: 86400,
              },
            ],
            profiles: [
              {
                id: "vpn",
                name: "Imported Profile",
                interfaces: ["missing0", "missing1"],
                on_unavailable: "blackhole",
              },
            ],
          }),
        ),
      });
      const dialog = page.getByRole("dialog");
      await dialog.getByRole("tab", { name: "Subscriptions", exact: true }).click();
      await dialog.getByRole("button", { name: "All", exact: true }).click();
      await dialog.getByRole("tab", { name: "Settings", exact: true }).click();
      await dialog.getByRole("button", { name: "All", exact: true }).click();
      await expect(dialog.getByRole("region", { name: "Interface mapping" })).toHaveCount(0);
      await expect(
        dialog.getByText("Interface not found on this device", { exact: false }),
      ).toHaveCount(0);
      await expect(dialog.getByRole("button", { name: "Import", exact: true })).toBeEnabled();
      const request = page.waitForRequest((req) => req.url().endsWith("/system/config/apply"));
      await dialog.getByRole("button", { name: "Import", exact: true }).click();
      const payload = (await request).postDataJSON();
      expect(payload.config.groups[0].interface).toBe("mitun0");
      expect(payload.config.subscriptions[0].interface).toBe("offline0");
      expect(payload.config.profiles[0].interfaces).toEqual(["missing0", "missing1"]);
      await expect(dialog).toHaveCount(0);
      expect(applied).toBe(1);
      const route = page
        .locator(".group-wrapper")
        .getByRole("button", { name: "Route", exact: true });
      await expect(route).toHaveText("mitun0");
      await route.click();
      await page.getByRole("option", { name: "wg0", exact: true }).click();
      await expect(route).toHaveText("wg0");
    });
  }
  for (const width of [1280, 390]) {
    test(`ignores daemon settings from old files and preserves device settings at ${width}px`, async ({
      page,
    }) => {
      await page.setViewportSize({ width, height: width === 390 ? 844 : 900 });
      const chooser = page.waitForEvent("filechooser");
      const snapshot = page.waitForResponse((response) =>
        response.url().endsWith("/system/config/snapshot"),
      );
      await page.locator(".group-controls-actions button").nth(1).click();
      await (
        await chooser
      ).setFiles({
        name: "config.mtrickle",
        mimeType: "application/json",
        buffer: Buffer.from(
          JSON.stringify({
            groups: [
              {
                id: "12345678",
                name: "Imported Group",
                interface: "wg0",
                enable: true,
                rules: [],
                color: "#123456",
              },
            ],
            profiles: [],
            settings: {
              logLevel: "debug",
              showAllInterfaces: true,
              dnsProxy: { timeout: "15s" },
              httpWeb: { host: { address: "0.0.0.0", port: 9999 } },
              netfilter: { disableIPv4: true },
            },
          }),
        ),
      });
      const originalSettings = (await (await snapshot).json()).config.app;
      const dialog = page.getByRole("dialog");
      await dialog.getByRole("tab", { name: "Settings", exact: true }).click();
      await expect(dialog.getByRole("checkbox")).toHaveCount(0);
      await expect(dialog.getByRole("heading", { name: "Profiles", exact: true })).toHaveCount(0);
      await expect(dialog.getByText("0 / 0", { exact: true })).toBeVisible();
      await expect(dialog.getByText("This file has no data for this section")).toBeVisible();
      await expect(dialog.getByRole("button", { name: "All", exact: true })).toBeDisabled();
      const request = page.waitForRequest((req) => req.url().endsWith("/system/config/apply"));
      await dialog.getByRole("button", { name: "Import", exact: true }).click();
      const payload = (await request).postDataJSON();
      expect(payload.config.app).toEqual(originalSettings);
      expect(payload.config.groups[0].name).toBe("Imported Group");
      await expect(dialog).toHaveCount(0);
      expect(applied).toBe(1);
      const downloadPromise = page.waitForEvent("download");
      await page.locator(".group-controls-actions button").nth(2).click();
      const download = await downloadPromise;
      const exported = JSON.parse(fs.readFileSync(await download.path(), "utf8"));
      expect(exported).not.toHaveProperty("settings");
      expect(exported.groups[0].name).toBe("Imported Group");
    });
  }
  for (const width of [1280, 390]) {
    test(`explains a blocked import and keeps controls usable at ${width}px`, async ({ page }) => {
      await page.addInitScript(() =>
        localStorage.setItem("locale", JSON.stringify({ value: "ru" })),
      );
      await page.reload();
      await page.setViewportSize({ width, height: width === 390 ? 844 : 900 });
      const chooser = page.waitForEvent("filechooser");
      await page.locator(".group-controls-actions button").nth(1).click();
      await (
        await chooser
      ).setFiles({
        name: "config.mtrickle",
        mimeType: "application/json",
        buffer: Buffer.from(
          JSON.stringify({
            profiles: [],
            settings: { logLevel: "debug" },
            groups: [
              {
                id: "12345678",
                name: "Imported Group",
                rules: [],
                color: "#123456",
                enable: true,
                interface: "wg0",
                profile: "p_missing",
              },
              {
                id: "22334455",
                name: "Missing interface",
                rules: [],
                color: "#123456",
                enable: true,
                interface: "tun0",
              },
            ],
          }),
        ),
      });
      const dialog = page.getByRole("dialog");
      await expect(dialog.getByRole("checkbox", { name: "Imported Group" })).toBeChecked();
      await expect(dialog.getByRole("alert")).toHaveCount(0);
      await expect(dialog.getByText("p_missing", { exact: false })).toHaveCount(0);
      await expect(
        dialog.getByRole("button", { name: "Импортировать", exact: true }),
      ).toHaveAccessibleDescription(/В файле нет профилей маршрутизации для: Imported Group/);
      await dialog.getByRole("checkbox", { name: "Imported Group" }).uncheck();
      await expect(
        dialog.getByRole("button", { name: "Импортировать", exact: true }),
      ).toBeEnabled();
      await dialog.getByRole("checkbox", { name: "Imported Group" }).check();
      await expect(
        dialog.getByRole("button", { name: "Импортировать", exact: true }),
      ).toBeDisabled();
      await dialog.getByRole("switch", { name: "Заменить" }).click();
      await expect(dialog.getByRole("switch", { name: "Заменить" })).toBeChecked();
      for (const region of [
        dialog,
        dialog.locator(".import-config"),
        dialog.getByRole("tablist"),
      ]) {
        expect(
          await region.evaluate((element) => element.scrollWidth - element.clientWidth),
        ).toBeLessThanOrEqual(1);
      }
      await dialog.getByRole("tab", { name: "Настройки", exact: true }).click();
      await expect(dialog.getByRole("heading", { name: "Профили", exact: true })).toHaveCount(0);
      await expect(dialog.getByRole("checkbox", { name: "Настройки", exact: true })).toHaveCount(0);
      await expect(dialog.getByRole("checkbox")).toHaveCount(0);
      await expect(dialog.getByText("В файле нет данных для этого раздела")).toBeVisible();
      const bounds = await dialog.boundingBox();
      expect(bounds!.x).toBeGreaterThanOrEqual(0);
      expect(bounds!.x + bounds!.width).toBeLessThanOrEqual(width);
      await dialog.getByRole("button", { name: "Отмена" }).click();
      await expect(dialog).toHaveCount(0);
      expect(applied).toBe(0);
    });
  }
});
