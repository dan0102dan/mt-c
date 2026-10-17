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
      httpWeb: { enabled: true, auth: { enabled: false }, host: { address: "127.0.0.1", port: 6969 }, skin: "" },
      dnsProxy: { host: { address: "0.0.0.0", port: 53 }, upstream: { address: "8.8.8.8", port: 53 },
        disableRemap53: false, disableFakePTR: false, disableDropAAAA: false,
        maxIdleConns: 128, maxConcurrent: 128, timeout: "10s" },
      netfilter: { iptables: { chainPrefix: "MT_" }, ipset: { tablePrefix: "mt_", additionalTTL: "30s" },
        disableIPv4: false, disableIPv6: false, startMarkTableIndex: 150 },
      link: [], showAllInterfaces: false, logLevel: "info",
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
          config: { configVersion: "0.8.5", app, groups: serverGroups,
            profiles: serverProfiles, subscriptions: serverSubs },
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
        ...s, lastUpdate: s.last_update ?? 0,
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
    expect(payload).toHaveProperty("settings");
    expect(payload.subscriptions).toEqual([]);
    expect(payload.profiles).toEqual([]);
    expect(applied).toBe(0);
    expect(page.getByRole("dialog")).toHaveCount(0);
  });

  test("previews and applies legacy group-only import without erasing other sections", async ({ page }) => {
    const configData = {
      groups: [
        {
          id: "12345678", name: "Imported Group", rules: [], color: "#123456",
          enable: true, interface: "wg0",
        },
      ],
    };
    const filePath = path.join("tests", "e2e", "fixtures", "config.mtrickle");
    const fileChooserPromise = page.waitForEvent("filechooser");
    await page.locator(".group-controls-actions button").nth(1).click();
    const fileChooser = await fileChooserPromise;
    await fileChooser.setFiles({ name: path.basename(filePath),
      mimeType: "application/json", buffer: Buffer.from(JSON.stringify(configData)) });

    const dialog = page.getByRole("dialog");
    await expect(dialog).toBeVisible();
    await expect(dialog.getByText("Импорт конфига")).toBeVisible();
    await expect(dialog.getByText("Группы", { exact: true })).toBeVisible();
    await dialog.getByText("Предпросмотр изменений").click();
    await expect(dialog.getByText("Imported Group")).toBeVisible();
    expect(applied).toBe(0);
    await dialog.getByRole("button", { name: "Применить изменения" }).click();
    await expect(dialog).toHaveCount(0);
    await expect(page.locator(".group-wrapper")).toHaveCount(1);
    await expect(page.locator("input.group-name")).toHaveValue("Imported Group");
    expect(applied).toBe(1);
  });

  test("shows identical transfer actions on all three tabs", async ({ page }) => {
    for (const tab of ["groups", "subscriptions", "settings"]) {
      await page.locator(`[data-tabs-trigger][data-value="${tab}"]`).count();
    }
    await expect(page.locator(".group-controls-actions button[aria-label='Экспорт конфига']")).toHaveCount(1);
    await page.getByRole("tab", { name: /Подписки|Subscriptions/ }).click();
    await expect(page.locator(".subscription-controls-actions button[aria-label='Экспорт конфига']")).toBeVisible();
    await page.getByRole("tab", { name: /Настройки|Settings/ }).click();
    await expect(page.locator(".settings-toolbar button[aria-label='Экспорт конфига']")).toBeVisible();
  });
});
