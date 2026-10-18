import fs from "fs";
import path from "path";
import { expect, test, type Locator, type Page } from "@playwright/test";

import { GroupsPage } from "./pages/GroupsPage";

const makeGroup = (id: string, name: string) => ({
  id,
  name,
  rules: [],
  color: "#123456",
  enable: true,
  interface: "wg0",
  priority: 100,
});
const makeSubscription = (id: string, name: string) => ({
  id,
  name,
  url: `https://example.org/${id}`,
  interface: "wg0",
  enable: true,
  priority: 100,
  interval: 86400,
  lastUpdate: 1,
  overrides: [],
  rules: [],
});
async function openTransfer(page: Page, payload: unknown) {
  const chooser = page.waitForEvent("filechooser");
  await page.getByRole("button", { name: "Import Config", exact: true }).click();
  await (
    await chooser
  ).setFiles({
    name: "config.mtrickle",
    mimeType: "application/json",
    buffer: Buffer.from(JSON.stringify(payload)),
  });
  const dialog = page.getByRole("dialog");
  await expect(dialog).toBeVisible();
  return dialog;
}
async function expectInputValues(inputs: Locator, values: string[]) {
  await expect(inputs).toHaveCount(values.length);
  for (const [index, value] of values.entries()) await expect(inputs.nth(index)).toHaveValue(value);
}

test.describe("Import/Export", () => {
  let groupsPage: GroupsPage;
  let serverGroups: any[];
  let serverSubs: any[];
  let serverProfiles: any[];
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
    serverSubs = [];
    serverProfiles = [];
    await page.route("**/auth", async (route) => route.fulfill({ json: { enabled: false } }));
    await page.route("**/groups?with_rules=true", async (route) =>
      route.fulfill({ json: { groups: serverGroups } }),
    );
    await page.route("**/subscriptions", async (route) =>
      route.fulfill({ json: { subscriptions: serverSubs } }),
    );
    await page.route("**/profiles", async (route) => {
      if (route.request().method() === "PUT")
        serverProfiles = route.request().postDataJSON().profiles;
      await route.fulfill({ json: { profiles: serverProfiles } });
    });
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
        rules:
          s.rules ?? serverSubs.find((old) => old.id === s.id && old.url === s.url)?.rules ?? [],
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

  test("append preserves group and nested rule drafts and saves against the new baseline", async ({
    page,
  }) => {
    serverGroups = [
      {
        ...makeGroup("11223344", "Existing"),
        rules: [
          {
            id: "22334455",
            name: "Saved rule",
            rule: "example.org",
            type: "namespace",
            enable: true,
          },
        ],
      },
    ];
    await page.reload();
    await groupsPage.setGroupName(0, "Unsaved name");
    await page.locator(".group-header [data-collapsible-trigger]").click();
    await groupsPage.setRuleName(0, 0, "Unsaved rule");
    const dialog = await openTransfer(page, { groups: [makeGroup("55667788", "Imported")] });
    await dialog.getByRole("button", { name: "Import", exact: true }).click();
    await expect(dialog).toHaveCount(0);
    await expect(page.locator(".group-list")).toBeVisible();
    await expectInputValues(page.locator("input.group-name"), ["Unsaved name", "Imported"]);
    await expect(page.locator(".rule .name input")).toHaveValue("Unsaved rule");
    expect(serverGroups[0].name).toBe("Existing");
    expect(serverGroups[0].rules[0].name).toBe("Saved rule");
    await expect(groupsPage.saveButton).toBeEnabled();

    let saved: any;
    await page.route("**/groups?save=true", async (route) => {
      saved = route.request().postDataJSON();
      serverGroups = [
        {
          ...serverGroups[0],
          name: "Unsaved name",
          rules: [{ ...serverGroups[0].rules[0], name: "Unsaved rule" }],
        },
        serverGroups[1],
      ];
      await route.fulfill({ json: { groups: serverGroups } });
    });
    await groupsPage.save();
    await expect(groupsPage.saveButton).toBeDisabled();
    expect(saved.groups.map((group: any) => group.name)).toEqual(["Unsaved name", "Imported"]);
    expect(saved.groups[0].ruleChanges).toEqual([
      expect.objectContaining({
        id: "22334455",
        name: "Unsaved rule",
        previous: expect.objectContaining({ name: "Saved rule" }),
      }),
    ]);
    expect(saved.groups[1].ruleChanges).toEqual([]);
    expect(saved.groups[0]).not.toHaveProperty("rules");
    await page.reload();
    await expectInputValues(page.locator("input.group-name"), ["Unsaved name", "Imported"]);
  });

  test("overlapping import leaves the dirty group and server untouched", async ({ page }) => {
    serverGroups = [makeGroup("11223344", "Existing")];
    await page.reload();
    await groupsPage.setGroupName(0, "Unsaved name");
    const dialog = await openTransfer(page, {
      groups: [makeGroup("11223344", "Imported replacement")],
    });
    await dialog.getByRole("switch", { name: "Replace", exact: true }).click();
    await dialog.getByRole("button", { name: "Import", exact: true }).click();
    await expect(
      page.getByText("Save or discard local changes before importing: Imported replacement", {
        exact: true,
      }),
    ).toBeVisible();
    expect(applied).toBe(0);
    expect(serverGroups[0].name).toBe("Existing");
    await expect(dialog).toBeVisible();
    await dialog.getByRole("button", { name: "Cancel", exact: true }).click();
    await expect(page.locator("input.group-name")).toHaveValue("Unsaved name");
    await expect(groupsPage.saveButton).toBeEnabled();
  });

  test("copied groups keep independent rule drafts and dirty indicators", async ({ page }) => {
    serverGroups = [
      {
        ...makeGroup("11223344", "Existing"),
        rules: [
          {
            id: "22334455",
            name: "Saved rule",
            rule: "example.org",
            type: "namespace",
            enable: true,
          },
        ],
      },
    ];
    await page.reload();
    const dialog = await openTransfer(page, {
      groups: [{ ...serverGroups[0], name: "Imported copy" }],
    });
    await dialog
      .getByRole("button", { name: "Resolve conflict for Imported copy", exact: true })
      .click();
    await page.getByRole("option", { name: "Create a copy", exact: true }).click();
    await dialog.getByRole("button", { name: "Import", exact: true }).click();
    await expect(dialog).toHaveCount(0);
    await expect(page.locator(".group-list")).toBeVisible();
    expect(serverGroups).toHaveLength(2);
    expect(serverGroups[0].rules[0].id).not.toBe(serverGroups[1].rules[0].id);
    await page.locator(".group-header [data-collapsible-trigger]").nth(0).click();
    await page.locator(".group-header [data-collapsible-trigger]").nth(1).click();
    await groupsPage.setRuleName(0, 0, "Original draft");
    await groupsPage.setRuleName(1, 0, "Copied draft");
    await groupsPage.setRuleName(1, 0, "Saved rule");
    await expect(groupsPage.saveButton).toBeEnabled();
    await expect((await groupsPage.getRule(0, 0)).locator(".name input")).toHaveValue(
      "Original draft",
    );
  });

  test("an imported copy cannot alias an unsaved new rule in another group", async ({ page }) => {
    serverGroups = [makeGroup("11223344", "Existing")];
    await page.reload();
    await groupsPage.addRuleToGroup(0);
    await groupsPage.setRuleName(0, 0, "Local rule");
    await groupsPage.setRulePattern(0, 0, "local.example");
    const downloadPromise = page.waitForEvent("download");
    await page.locator(".group-controls-actions button[aria-label='Экспорт конфига']").click();
    const download = await downloadPromise;
    const exported = JSON.parse(fs.readFileSync(await download.path(), "utf8"));
    const dialog = await openTransfer(page, exported);
    await dialog
      .getByRole("button", { name: "Resolve conflict for Existing", exact: true })
      .click();
    await page.getByRole("option", { name: "Create a copy", exact: true }).click();
    await dialog.getByRole("button", { name: "Import", exact: true }).click();
    await expect(
      page.getByText("Save or discard local changes before importing: Existing", { exact: true }),
    ).toBeVisible();
    expect(applied).toBe(0);
    expect(serverGroups).toHaveLength(1);
    expect(serverGroups[0].rules).toEqual([]);
    await dialog.getByRole("button", { name: "Cancel", exact: true }).click();
    await expect((await groupsPage.getRule(0, 0)).locator(".name input")).toHaveValue("Local rule");
    await expect(groupsPage.saveButton).toBeEnabled();
  });

  test("append preserves subscription rule drafts without saving them in the import request", async ({
    page,
  }) => {
    serverSubs = [
      {
        ...makeSubscription("11223344", "Existing subscription"),
        rules: [{ id: "22334455", rule: "example.org", type: "namespace", enable: true }],
      },
    ];
    await page.reload();
    await page.getByRole("tab", { name: "Subscriptions", exact: true }).click();
    await page.locator(".subscription-name").fill("Unsaved subscription");
    await page.locator(".subscription-panel [data-collapsible-trigger]").click();
    await page.locator(".subscription-rule").getByRole("switch").click();
    const dialog = await openTransfer(page, {
      subscriptions: [makeSubscription("55667788", "Imported subscription")],
    });
    await dialog.getByRole("button", { name: "Import", exact: true }).click();
    await expect(dialog).toHaveCount(0);
    await expect(page.locator(".subscription-list")).toBeVisible();
    await expectInputValues(page.locator(".subscription-name"), [
      "Unsaved subscription",
      "Imported subscription",
    ]);
    await expect(page.locator(".subscription-rule").getByRole("switch")).not.toBeChecked();
    expect(serverSubs[0].name).toBe("Existing subscription");
    expect(serverSubs[0].rules[0].enable).toBe(true);
    const saved = page.waitForRequest(
      (request) => request.method() === "PUT" && request.url().endsWith("/subscriptions"),
    );
    await page.locator("#save-subscriptions").click();
    const payload = (await saved).postDataJSON();
    expect(payload.subscriptions.map((sub: any) => sub.name)).toEqual([
      "Unsaved subscription",
      "Imported subscription",
    ]);
    expect(payload.subscriptions[0].ruleChanges).toEqual([
      {
        id: "22334455",
        rule: "example.org",
        previousType: "namespace",
        previousEnable: true,
        type: "namespace",
        enable: false,
      },
    ]);
    expect(payload.subscriptions[0]).not.toHaveProperty("rules");
    expect(payload.subscriptions[1].ruleChanges).toEqual([]);
    await expect(page.locator("#save-subscriptions")).toBeDisabled();
    const downloadPromise = page.waitForEvent("download");
    await page
      .locator(".subscription-controls-actions button[aria-label='Экспорт конфига']")
      .click();
    const download = await downloadPromise;
    const exported = JSON.parse(fs.readFileSync(await download.path(), "utf8"));
    expect(exported.subscriptions[0].overrides).toEqual([{ rule: "example.org", enable: false }]);
  });

  test("append preserves an unrelated profile draft and its save baseline", async ({ page }) => {
    serverProfiles = [
      {
        id: "existing",
        name: "Existing profile",
        interfaces: ["wg0"],
        on_unavailable: "blackhole",
      },
    ];
    await page.reload();
    await page.getByRole("tab", { name: "Settings", exact: true }).click();
    await page.getByLabel("Profile name", { exact: true }).fill("Unsaved profile");
    const dialog = await openTransfer(page, {
      profiles: [
        {
          id: "imported",
          name: "Imported profile",
          interfaces: ["wg0"],
          on_unavailable: "blackhole",
        },
      ],
    });
    await dialog.getByRole("button", { name: "Import", exact: true }).click();
    await expect(dialog).toHaveCount(0);
    await expectInputValues(page.getByLabel("Profile name", { exact: true }), [
      "Unsaved profile",
      "Imported profile",
    ]);
    expect(serverProfiles.map((profile) => profile.name)).toEqual([
      "Existing profile",
      "Imported profile",
    ]);
    await expect(page.locator("#save-settings")).toBeEnabled();
    await page.locator("#save-settings").click();
    await expect(page.locator("#save-settings")).toBeDisabled();
    expect(serverProfiles.map((profile) => profile.name)).toEqual([
      "Unsaved profile",
      "Imported profile",
    ]);
  });

  test("export with an unfinished profile opens and lets the user import its valid groups", async ({
    page,
  }) => {
    serverGroups = [makeGroup("11223344", "Portable group")];
    await page.reload();
    await page.getByRole("tab", { name: "Settings", exact: true }).click();
    await page.getByRole("button", { name: "Add profile", exact: true }).click();
    const downloadPromise = page.waitForEvent("download");
    await page.locator(".settings-toolbar button[aria-label='Экспорт конфига']").click();
    const download = await downloadPromise;
    const payload = JSON.parse(fs.readFileSync(await download.path(), "utf8"));
    expect(payload.profiles).toEqual([expect.objectContaining({ name: "", interfaces: [] })]);

    serverGroups = [];
    await page.reload();
    await page.getByRole("tab", { name: "Groups", exact: true }).click();
    const dialog = await openTransfer(page, payload);
    await expect(dialog.getByRole("button", { name: "Import", exact: true })).toBeEnabled();
    await dialog.getByRole("tab", { name: "Settings", exact: true }).click();
    const unfinished = dialog.getByRole("checkbox", { name: "Profiles 1", exact: true });
    await expect(unfinished).not.toBeChecked();
    await unfinished.check();
    await expect(dialog.getByRole("button", { name: "Import", exact: true })).toBeDisabled();
    await expect(
      dialog.getByRole("button", { name: "Import", exact: true }),
    ).toHaveAccessibleDescription(/Enter a profile name/);
    await unfinished.uncheck();
    await dialog.getByRole("button", { name: "Import", exact: true }).click();
    await expect(dialog).toHaveCount(0);
    expect(serverGroups[0].name).toBe("Portable group");
    expect(serverProfiles).toEqual([]);
  });

  test("an unfinished profile required by a selected group blocks apply", async ({ page }) => {
    const dialog = await openTransfer(page, {
      groups: [{ ...makeGroup("11223344", "Dependent group"), profile: "unfinished" }],
      profiles: [{ id: "unfinished", name: "", interfaces: [], on_unavailable: "blackhole" }],
    });
    await expect(dialog.getByRole("button", { name: "Import", exact: true })).toBeDisabled();
    await expect(
      dialog.getByRole("button", { name: "Import", exact: true }),
    ).toHaveAccessibleDescription(/Profile unfinished: Enter a profile name/);
    await dialog.getByRole("tab", { name: "Settings", exact: true }).click();
    await expect(
      dialog.getByRole("checkbox", { name: "Profiles 1", exact: true }),
    ).not.toBeChecked();
    expect(applied).toBe(0);
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
