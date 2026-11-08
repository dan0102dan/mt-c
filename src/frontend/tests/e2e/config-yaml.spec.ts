import { expect, test, type Locator, type Page } from "@playwright/test";

import type { Group, Profile, Subscription } from "../../src/types";

const originalYaml = `# Router configuration — keep this comment.
configVersion: 0.7.0
app:
  logLevel: info
profiles:
  - id: "11223344"
    name: "Primary VPN"
    interfaces:
      - tun0
    on_unavailable: blackhole
groups: []
subscriptions: []
`;

const editedYaml = originalYaml
  .replace(
    "# Router configuration — keep this comment.",
    "# Edited in browser — комментарий сохранён.",
  )
  .replace("  logLevel: info", "  logLevel: 'debug' # temporary diagnostics");

type Failure = "validation" | "server" | "network" | "malformed";
type YamlWrite = { yaml: string; revision: string; allow_out_of_sync?: boolean };

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

/** Model disk revisions independently of the browser. C integration tests cover
 * parsing and atomic disk writes; these tests exercise user-visible recovery. */
async function backend(page: Page) {
  const state = {
    yaml: originalYaml,
    version: 1,
    outOfSync: false,
    restartRequired: false,
    failure: null as Failure | null,
    readFailure: false,
    writes: [] as YamlWrite[],
    profileWrites: 0,
    structuredWrites: [] as string[],
    profilePersistenceFailure: false,
    groups: [] as Group[],
    subscriptions: [] as Subscription[],
    profiles: [
      {
        id: "11223344",
        name: "Primary VPN",
        interfaces: ["tun0"],
        on_unavailable: "blackhole",
        usage: { groups: 0, subscriptions: 0 },
      },
    ] as Profile[],
  };
  const revision = () => state.version.toString(16).padStart(64, "0");
  const snapshot = () => ({
    yaml: state.yaml,
    revision: revision(),
    restart_required: state.restartRequired,
    out_of_sync: state.outOfSync,
  });

  await page.route("**/api/v1/**", async (route) => {
    const request = route.request();
    const path = new URL(request.url()).pathname.replace("/api/v1", "");
    if (request.method() === "OPTIONS") return route.fulfill({ status: 204 });
    if (path === "/auth") return route.fulfill({ json: { enabled: false } });
    if (path === "/system/interfaces")
      return route.fulfill({
        json: { interfaces: ["blackhole", "tun0", "tun1"].map((id) => ({ id, name: "" })) },
      });
    if (path === "/system/config/yaml") {
      if (request.method() === "GET") {
        if (state.readFailure)
          return route.fulfill({ status: 500, json: { error: "Cannot read configuration" } });
        return route.fulfill({ json: snapshot() });
      }
      const body: unknown = request.postDataJSON();
      if (!isRecord(body) || typeof body.yaml !== "string" || typeof body.revision !== "string")
        return route.fulfill({ status: 400, json: { error: "Invalid YAML save request" } });
      const submission: YamlWrite = { yaml: body.yaml, revision: body.revision };
      if (typeof body.allow_out_of_sync === "boolean")
        submission.allow_out_of_sync = body.allow_out_of_sync;
      state.writes.push(submission);
      if (submission.revision !== revision())
        return route.fulfill({
          status: 409,
          json: { code: "CONFIG_CONFLICT", error: "Configuration changed since it was loaded" },
        });
      if (state.outOfSync && !submission.allow_out_of_sync)
        return route.fulfill({
          status: 409,
          json: { code: "CONFIG_OUT_OF_SYNC", error: "Runtime and disk configuration differ" },
        });
      if (state.failure === "network") return route.abort("failed");
      if (state.failure === "validation")
        return route.fulfill({
          status: 400,
          json: {
            code: "INVALID_CONFIG",
            error: "Unknown configuration option",
            line: 4,
            column: 3,
          },
        });
      if (state.failure === "server")
        return route.fulfill({ status: 500, json: { error: "Cannot write configuration file" } });
      state.yaml = submission.yaml;
      state.version++;
      state.outOfSync = false;
      if (state.failure === "malformed")
        return route.fulfill({ json: { yaml: submission.yaml, revision: 123 } });
      return route.fulfill({ json: snapshot() });
    }
    if (request.method() === "PUT") state.structuredWrites.push(path);
    if (path === "/profiles") {
      if (request.method() === "PUT") {
        const body: unknown = request.postDataJSON();
        if (!isRecord(body) || !Array.isArray(body.profiles))
          throw new Error("Invalid profiles request");
        const first: unknown = body.profiles[0];
        if (!isRecord(first) || typeof first.name !== "string")
          throw new Error("Expected an existing profile");
        state.profiles[0].name = first.name;
        state.profileWrites++;
        state.version++;
        if (state.profilePersistenceFailure) {
          state.outOfSync = true;
          return route.fulfill({
            status: 500,
            json: {
              code: "PERSISTENCE_FAILED",
              applied: true,
              error: "Disk is full",
              profiles: state.profiles,
            },
          });
        }
        state.outOfSync = false;
        state.yaml = state.yaml.replace(/    name: ".*"/, `    name: "${first.name}"`);
      }
      return route.fulfill({ json: { profiles: state.profiles } });
    }
    if (path === "/groups") return route.fulfill({ json: { groups: state.groups } });
    if (path === "/subscriptions")
      return route.fulfill({ json: { subscriptions: state.subscriptions } });
    if (path === "/system/update/status") return route.fulfill({ json: { stage: "idle" } });
    if (path === "/keenetic/policy")
      return route.fulfill({
        json: {
          settings: { enabled: false, policy_id: "", mode: "include" },
          runtime: { observed_ip_count: 0, ipset_sync_state: "disabled" },
          selected_policy: null,
          selected_policy_missing: false,
          selected_policy_ambiguous: false,
        },
      });
    if (path === "/keenetic/policies") return route.fulfill({ json: { policies: [] } });
    return route.fulfill({ json: { version: "test" } });
  });
  return { state, revision };
}

async function showPage(page: Page, name: "Groups" | "Subscriptions" | "Settings") {
  const menu = page.getByRole("button", { name: "Open menu", exact: true });
  if (await menu.isVisible()) await menu.click();
  await page.getByRole("tab", { name, exact: true }).click();
}

function yamlEditor(page: Page): Locator {
  return page.getByRole("textbox", { name: "YAML configuration", exact: true });
}

async function openYaml(page: Page) {
  await showPage(page, "Settings");
  await page.getByRole("tab", { name: "YAML", exact: true }).click();
  const editor = yamlEditor(page);
  await expect(editor).toBeVisible();
  return editor;
}

async function editorText(editor: Locator) {
  return (await editor.locator(".cm-line").allTextContents()).join("\n");
}

async function hasUnloadWarning(page: Page) {
  return page.evaluate(() => {
    const event = new Event("beforeunload", { cancelable: true });
    window.dispatchEvent(event);
    return event.defaultPrevented;
  });
}

test("save and reload preserve YAML comments, quotes, Unicode and final newline", async ({
  page,
}) => {
  const { state, revision } = await backend(page);
  await page.goto("/");
  const editor = await openYaml(page);
  await expect.poll(() => editorText(editor)).toBe(originalYaml);
  const loadedRevision = revision();
  await editor.fill(editedYaml);
  await page.locator("#save-settings").click();
  await expect.poll(() => state.yaml).toBe(editedYaml);
  await expect.poll(() => hasUnloadWarning(page)).toBe(false);
  await expect(page.locator("#save-settings")).toBeDisabled();
  expect(state.writes).toEqual([{ yaml: editedYaml, revision: loadedRevision }]);
  expect(state.yaml).toBe(editedYaml);
  expect(await hasUnloadWarning(page)).toBe(false);
  await page.reload();
  const reloaded = await openYaml(page);
  await expect.poll(() => editorText(reloaded)).toBe(editedYaml);
  expect(await page.evaluate(() => Object.values(localStorage).join("\n"))).not.toContain(
    "temporary diagnostics",
  );
});

test("typing and saving preserve CRLF line endings and the final terminator", async ({ page }) => {
  const { state, revision } = await backend(page);
  const crlfYaml = originalYaml.replaceAll("\n", "\r\n");
  state.yaml = crlfYaml;
  await page.goto("/");
  const editor = await openYaml(page);
  await expect.poll(() => editorText(editor)).toBe(originalYaml);
  const loadedRevision = revision();
  await editor.press("Control+Home");
  await editor.press("End");
  await editor.pressSequentially(" Edited in browser.");
  const savedYaml = crlfYaml.replace("\r\n", " Edited in browser.\r\n");
  await page.locator("#save-settings").click();
  await expect.poll(() => state.yaml).toBe(savedYaml);
  await expect(page.locator("#save-settings")).toBeDisabled();
  expect(state.writes).toEqual([{ yaml: savedYaml, revision: loadedRevision }]);
});

test("the restart notice follows the flag returned when loading and saving YAML", async ({
  page,
}) => {
  const { state } = await backend(page);
  state.restartRequired = true;
  await page.goto("/");
  const editor = await openYaml(page);
  const notice = page
    .getByRole("status")
    .filter({ hasText: "Configuration saved. Restart the service to apply startup settings." });
  await expect(notice).toBeVisible();
  state.restartRequired = false;
  await editor.fill(editedYaml);
  await page.locator("#save-settings").click();
  await expect.poll(() => state.yaml).toBe(editedYaml);
  await expect(notice).toHaveCount(0);
  state.restartRequired = true;
  await editor.fill(originalYaml);
  await page.locator("#save-settings").click();
  await expect.poll(() => state.yaml).toBe(originalYaml);
  await expect(notice).toBeVisible();
  await expect(page.locator("#save-settings")).toBeDisabled();
});

test("draft survives Settings tabs and page switches and warns before leaving", async ({
  page,
}) => {
  const { state } = await backend(page);
  await page.goto("/");
  const editor = await openYaml(page);
  await editor.fill(editedYaml);
  expect(await hasUnloadWarning(page)).toBe(true);
  await page.getByRole("tab", { name: "Profiles", exact: true }).click();
  await page.getByRole("tab", { name: "YAML", exact: true }).click();
  await expect.poll(() => editorText(editor)).toBe(editedYaml);
  await showPage(page, "Groups");
  await openYaml(page);
  await expect.poll(() => editorText(editor)).toBe(editedYaml);
  expect(state.writes).toHaveLength(0);
});

for (const failure of ["validation", "server", "network"] as const) {
  test(`${failure} failure retains the YAML draft and allows an explicit retry`, async ({
    page,
  }) => {
    const { state, revision } = await backend(page);
    await page.goto("/");
    const editor = await openYaml(page);
    await editor.fill(editedYaml);
    const loadedRevision = revision();
    state.failure = failure;
    await page.locator("#save-settings").click();
    await expect.poll(() => state.writes.length).toBe(1);
    await expect(page.locator("#save-settings")).toBeEnabled();
    await expect.poll(() => editorText(editor)).toBe(editedYaml);
    expect(state.yaml).toBe(originalYaml);
    expect(await hasUnloadWarning(page)).toBe(true);
    if (failure === "validation")
      await expect(
        page.getByText("Unknown configuration option", { exact: false }).first(),
      ).toBeVisible();
    if (failure === "server")
      await expect(
        page.getByText("Cannot write configuration file", { exact: false }).first(),
      ).toBeVisible();
    state.failure = null;
    await page.locator("#save-settings").click();
    await expect.poll(() => state.yaml).toBe(editedYaml);
    await expect.poll(() => hasUnloadWarning(page)).toBe(false);
    await expect(page.locator("#save-settings")).toBeDisabled();
    expect(state.writes).toEqual([
      { yaml: editedYaml, revision: loadedRevision },
      { yaml: editedYaml, revision: loadedRevision },
    ]);
    expect(state.yaml).toBe(editedYaml);
  });
}

test("a malformed save response does not acknowledge or discard the draft", async ({ page }) => {
  const { state } = await backend(page);
  await page.goto("/");
  const editor = await openYaml(page);
  await editor.fill(editedYaml);
  state.failure = "malformed";
  await page.locator("#save-settings").click();
  await expect.poll(() => state.writes.length).toBe(1);
  await expect(page.locator("#save-settings")).toBeEnabled();
  await expect.poll(() => editorText(editor)).toBe(editedYaml);
  expect(await hasUnloadWarning(page)).toBe(true);
});

test("a stale revision cannot overwrite a newer configuration and retains the draft", async ({
  page,
}) => {
  const { state, revision } = await backend(page);
  await page.goto("/");
  const editor = await openYaml(page);
  const loadedRevision = revision();
  await editor.fill(editedYaml);
  state.yaml = originalYaml.replace("logLevel: info", "logLevel: warn");
  state.version++;
  await page.locator("#save-settings").click();
  await expect.poll(() => state.writes.length).toBe(1);
  await expect(
    page.getByText("Configuration changed since it was loaded", { exact: false }).first(),
  ).toBeVisible();
  await expect.poll(() => editorText(editor)).toBe(editedYaml);
  expect(state.writes).toEqual([{ yaml: editedYaml, revision: loadedRevision }]);
  expect(state.yaml).toContain("logLevel: warn");
  expect(await hasUnloadWarning(page)).toBe(true);
});

for (const shortcut of [
  { platform: "Linux x86_64", key: "Control+s" },
  { platform: "MacIntel", key: "Meta+s" },
]) {
  test(`${shortcut.key} does not save the YAML draft`, async ({ page }) => {
    await page.addInitScript((platform) => {
      Object.defineProperty(navigator, "platform", { get: () => platform });
    }, shortcut.platform);
    const { state } = await backend(page);
    await page.goto("/");
    const editor = await openYaml(page);
    await editor.fill(editedYaml);
    await editor.press(shortcut.key);
    expect(state.yaml).toBe(originalYaml);
    expect(await hasUnloadWarning(page)).toBe(true);
    expect(state.writes).toHaveLength(0);
  });
}

test("Tab leaves the YAML editor without changing indentation", async ({ page }) => {
  await backend(page);
  await page.goto("/");
  const editor = await openYaml(page);
  await editor.fill("app:\nlogLevel: info");
  await editor.press("Control+End");
  await editor.press("Home");
  await editor.press("Tab");
  await expect.poll(() => editorText(editor)).toBe("app:\nlogLevel: info");
  await expect(editor).not.toBeFocused();
});

test("browser find shortcuts are not intercepted by the YAML editor", async ({ page }) => {
  await backend(page);
  await page.goto("/");
  const editor = await openYaml(page);
  for (const modifier of ["ctrlKey", "metaKey"]) {
    const intercepted = await editor.evaluate((element, modifier) => {
      const event = new KeyboardEvent("keydown", {
        key: "f",
        code: "KeyF",
        [modifier]: true,
        bubbles: true,
        cancelable: true,
      });
      element.dispatchEvent(event);
      return event.defaultPrevented;
    }, modifier);
    expect(intercepted).toBe(false);
  }
  await expect(page.locator(".cm-search")).toHaveCount(0);
});

for (const width of [1280, 390]) {
  test(`YAML uses the page width and scroll at ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 900 });
    const { state } = await backend(page);
    state.yaml = Array.from({ length: 150 }, (_, index) => `# Configuration line ${index}`).join(
      "\n",
    );
    await page.goto("/");
    const editor = await openYaml(page);
    await editor.click();
    const bounds = await page.locator(".yaml-editor").boundingBox();
    const pageBounds = await page.locator(".app-tabs").boundingBox();
    expect(bounds!.width).toBeCloseTo(pageBounds!.width, 0);
    expect(bounds!.x).toBeCloseTo(pageBounds!.x, 0);
    expect(bounds!.height).toBeGreaterThan(900);
    await page.mouse.wheel(0, 700);
    await expect.poll(() => page.evaluate(() => window.scrollY)).toBeGreaterThan(0);
    expect(await page.locator(".cm-scroller").evaluate((element) => element.scrollTop)).toBe(0);
    await expect(page.locator("#save-settings")).toBeInViewport();
  });
}

test("YAML tab and Save remain reachable on a narrow screen with a long YAML line", async ({
  page,
}) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await backend(page);
  await page.goto("/");
  const editor = await openYaml(page);
  await editor.fill(`# ${"long comment ".repeat(100)}\n${editedYaml}`);
  const save = page.getByRole("button", { name: "Save Changes", exact: true });
  const tab = page.getByRole("tab", { name: "YAML", exact: true });
  await expect(save).toBeEnabled();
  for (const control of [save, tab]) {
    await expect(control).toBeInViewport();
    const box = await control.boundingBox();
    expect(box).not.toBeNull();
    expect(box!.x).toBeGreaterThanOrEqual(0);
    expect(box!.x + box!.width).toBeLessThanOrEqual(390);
  }
  expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBeLessThanOrEqual(390);
});

const initialGroup: Group = {
  id: "aabbccdd",
  name: "Routing group",
  interface: "tun0",
  enable: true,
  priority: 300,
  rules: [],
};

const initialSubscription: Subscription = {
  id: "bbccddee",
  name: "Domain subscription",
  interface: "tun0",
  enable: true,
  priority: 100,
  url: "https://example.com/domains.txt",
  interval: 86400,
  rules: [],
};

for (const kind of ["groups", "subscriptions"] as const) {
  test(`unsaved ${kind} block YAML replacement and remain intact`, async ({ page }) => {
    const { state } = await backend(page);
    if (kind === "groups") state.groups = [structuredClone(initialGroup)];
    else state.subscriptions = [structuredClone(initialSubscription)];
    await page.goto("/");
    const editor = await openYaml(page);
    await editor.fill(editedYaml);
    await showPage(page, kind === "groups" ? "Groups" : "Subscriptions");
    const name = page.locator(kind === "groups" ? ".group-name" : ".subscription-name").first();
    await name.fill("Unsaved routing changes");
    await openYaml(page);
    await expect(page.locator("#save-settings")).toBeDisabled();
    await expect.poll(() => editorText(editor)).toBe(editedYaml);
    await editor.press("Control+s");
    expect(state.writes).toHaveLength(0);
    await showPage(page, kind === "groups" ? "Groups" : "Subscriptions");
    await expect(name).toHaveValue("Unsaved routing changes");
    await expect(
      page.locator(kind === "groups" ? "#save-changes" : "#save-subscriptions"),
    ).toBeEnabled();
    expect(state.structuredWrites).toEqual([]);
    expect(await hasUnloadWarning(page)).toBe(true);
  });
}

test("a profile draft can be saved while YAML is dirty without silently rebasing the YAML revision", async ({
  page,
}) => {
  const { state, revision } = await backend(page);
  await page.goto("/");
  const editor = await openYaml(page);
  const loadedRevision = revision();
  await editor.fill(editedYaml);
  await page.getByRole("tab", { name: "Profiles", exact: true }).click();
  const profileName = page.getByRole("textbox", { name: "Profile name", exact: true }).first();
  await profileName.fill("Changed VPN");
  await page.getByRole("tab", { name: "YAML", exact: true }).click();
  await expect(page.locator("#save-settings")).toBeDisabled();
  await page.getByRole("tab", { name: "Profiles", exact: true }).click();
  await expect(page.locator("#save-settings")).toBeEnabled();
  await page.locator("#save-settings").click();
  await expect.poll(() => state.profileWrites).toBe(1);
  await expect(page.locator("#save-settings")).toBeDisabled();
  expect(state.yaml).toContain('name: "Changed VPN"');
  await page.getByRole("tab", { name: "YAML", exact: true }).click();
  await expect.poll(() => editorText(editor)).toBe(editedYaml);
  await expect(page.locator("#save-settings")).toBeEnabled();
  await page.locator("#save-settings").click();
  await expect.poll(() => state.writes.length).toBe(1);
  await expect(
    page.getByText("Configuration changed since it was loaded", { exact: false }).first(),
  ).toBeVisible();
  expect(state.writes[0].revision).toBe(loadedRevision);
  expect(state.yaml).toContain('name: "Changed VPN"');
  await expect.poll(() => editorText(editor)).toBe(editedYaml);
});

test("a failed structured save retains drafts in groups, subscriptions, profiles and YAML", async ({
  page,
}) => {
  const { state } = await backend(page);
  state.groups = [structuredClone(initialGroup)];
  state.subscriptions = [structuredClone(initialSubscription)];
  state.profilePersistenceFailure = true;
  await page.goto("/");
  const editor = await openYaml(page);
  await editor.fill(editedYaml);
  await showPage(page, "Groups");
  await page.locator(".group-name").first().fill("Group draft");
  await showPage(page, "Subscriptions");
  await page.locator(".subscription-name").first().fill("Subscription draft");
  await showPage(page, "Settings");
  await page.getByRole("tab", { name: "Profiles", exact: true }).click();
  const profileName = page.getByRole("textbox", { name: "Profile name", exact: true }).first();
  await profileName.fill("Profile draft");
  await page.locator("#save-settings").click();
  await expect.poll(() => state.profileWrites).toBe(1);
  await expect(page.locator("#save-settings")).toBeEnabled();
  await expect(profileName).toHaveValue("Profile draft");
  await page.getByRole("tab", { name: "YAML", exact: true }).click();
  await expect(page.locator("#save-settings")).toBeDisabled();
  await expect.poll(() => editorText(editor)).toBe(editedYaml);
  await showPage(page, "Groups");
  await expect(page.locator(".group-name").first()).toHaveValue("Group draft");
  await showPage(page, "Subscriptions");
  await expect(page.locator(".subscription-name").first()).toHaveValue("Subscription draft");
  expect(state.yaml).toBe(originalYaml);
  expect(state.writes).toHaveLength(0);
  expect(await hasUnloadWarning(page)).toBe(true);
});

test("an invalid file can be repaired only after confirming replacement of the running configuration", async ({
  page,
}) => {
  const { state, revision } = await backend(page);
  state.yaml = "app: [broken yaml\n";
  state.outOfSync = true;
  await page.goto("/");
  const editor = await openYaml(page);
  await expect.poll(() => editorText(editor)).toBe(state.yaml);
  const loadedRevision = revision();
  await editor.fill(editedYaml);
  await expect(
    page.getByText(
      "The file differs from the running configuration. Saving it will require confirmation.",
      { exact: true },
    ),
  ).toBeVisible();
  const cancel = page.waitForEvent("dialog").then(async (dialog) => {
    expect(dialog.type()).toBe("confirm");
    expect(dialog.message()).toContain("replace the running configuration");
    await dialog.dismiss();
  });
  await page.locator("#save-settings").click();
  await cancel;
  await expect(page.locator("#save-settings")).toBeEnabled();
  await expect.poll(() => editorText(editor)).toBe(editedYaml);
  expect(state.writes).toHaveLength(0);
  expect(state.yaml).toBe("app: [broken yaml\n");
  expect(await hasUnloadWarning(page)).toBe(true);
  const approve = page.waitForEvent("dialog").then((dialog) => dialog.accept());
  await page.locator("#save-settings").click();
  await approve;
  await expect.poll(() => state.yaml).toBe(editedYaml);
  await expect(page.locator("#save-settings")).toBeDisabled();
  expect(state.writes).toEqual([
    { yaml: editedYaml, revision: loadedRevision, allow_out_of_sync: true },
  ]);
  await expect.poll(() => hasUnloadWarning(page)).toBe(false);
});

test("confirming replacement never permits overwriting a newer configuration revision", async ({
  page,
}) => {
  const { state, revision } = await backend(page);
  state.outOfSync = true;
  await page.goto("/");
  const editor = await openYaml(page);
  const loadedRevision = revision();
  await editor.fill(editedYaml);
  state.yaml = originalYaml.replace("logLevel: info", "logLevel: warn");
  state.version++;
  const approve = page.waitForEvent("dialog").then((dialog) => dialog.accept());
  await page.locator("#save-settings").click();
  await approve;
  await expect(
    page.getByRole("alert").filter({ hasText: "Configuration changed since it was loaded" }),
  ).toBeVisible();
  await expect(page.locator("#save-settings")).toBeEnabled();
  await expect.poll(() => editorText(editor)).toBe(editedYaml);
  expect(state.yaml).toContain("logLevel: warn");
  expect(state.writes).toEqual([
    { yaml: editedYaml, revision: loadedRevision, allow_out_of_sync: true },
  ]);
  expect(await hasUnloadWarning(page)).toBe(true);
});

test("page reload reads the current configuration after confirming draft loss", async ({
  page,
}) => {
  const { state } = await backend(page);
  await page.goto("/");
  const editor = await openYaml(page);
  await expect(page.getByRole("button", { name: "Reload configuration", exact: true })).toHaveCount(
    0,
  );
  await editor.fill(editedYaml);
  state.yaml = originalYaml.replace("logLevel: info", "logLevel: warn");
  state.version++;
  page.once("dialog", async (dialog) => {
    expect(dialog.type()).toBe("beforeunload");
    await dialog.accept();
  });
  await page.reload();
  const reloaded = await openYaml(page);
  await expect.poll(() => editorText(reloaded)).toBe(state.yaml);
  await expect(page.locator("#save-settings")).toBeDisabled();
  expect(state.writes).toHaveLength(0);
  expect(await hasUnloadWarning(page)).toBe(false);
});

for (const invalid of ["app: [", "app: {}\napp: {}", "", "- app", "app: {}\n---\ngroups: []"]) {
  test(`invalid YAML blocks saving and can be corrected: ${JSON.stringify(invalid)}`, async ({
    page,
  }) => {
    const { state } = await backend(page);
    await page.goto("/");
    const editor = await openYaml(page);
    await editor.fill(invalid);
    await expect(page.locator("#save-settings")).toBeDisabled();
    await expect(page.locator(".cm-yaml-error-gutter")).toBeVisible();
    expect(state.writes).toHaveLength(0);
    expect(state.yaml).toBe(originalYaml);
    expect(await hasUnloadWarning(page)).toBe(true);
    await editor.fill(editedYaml);
    await expect(page.locator(".cm-yaml-error-gutter")).toHaveCount(0);
    await expect(page.locator("#save-settings")).toBeEnabled();
    await page.locator("#save-settings").click();
    await expect.poll(() => state.yaml).toBe(editedYaml);
    expect(state.writes).toHaveLength(1);
  });
}

for (const width of [1280, 390]) {
  test(`YAML error stays visible while editing the bottom at ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 844 });
    const { state } = await backend(page);
    state.yaml = `${originalYaml}\n${"# More configuration\n".repeat(200)}`;
    await page.goto("/");
    const editor = await openYaml(page);
    await page.evaluate(() => window.scrollTo(0, document.documentElement.scrollHeight));
    await editor.locator(".cm-line").last().click();
    await page.keyboard.press("End");
    await page.keyboard.type("invalid: [");
    const error = page.locator(".cm-yaml-error-gutter");
    await expect(error).toBeInViewport();
    await expect(page.locator(".cm-yaml-error-line")).toBeInViewport();
    await expect(editor).toHaveAttribute("aria-invalid", "true");
    await expect(page.locator("#save-settings")).toBeInViewport();
    await expect(page.locator("#save-settings")).toBeDisabled();
    expect(await page.evaluate(() => window.scrollY)).toBeGreaterThan(1000);
    await page.keyboard.type("]");
    await expect(error).toHaveCount(0);
    await expect(page.locator(".cm-yaml-error-line")).toHaveCount(0);
    await expect(editor).not.toHaveAttribute("aria-invalid", "true");
    await expect(page.locator("#save-settings")).toBeEnabled();
    await expect(page.getByText("Unsaved changes", { exact: true })).toHaveCount(0);
    await page.getByRole("tab", { name: "Profiles", exact: true }).click();
    await expect(page.getByText("Unsaved changes", { exact: true })).not.toBeVisible();
  });
}

for (const width of [1280, 390]) {
  test(`YAML preserves the offending token highlight after scrolling at ${width}px`, async ({
    page,
  }) => {
    await page.setViewportSize({ width, height: 844 });
    const { state } = await backend(page);
    state.yaml = `app:\r\n  logLevel: "info"oops\r\n${"# More configuration\r\n".repeat(200)}`;
    await page.goto("/");
    await openYaml(page);
    await expect(page.locator(".cm-yaml-error-mark")).toHaveText("oops");
    await expect(page.locator(".cm-yaml-error-gutter")).toHaveCount(1);
    await page.evaluate(() => window.scrollTo(0, document.documentElement.scrollHeight));
    await page.evaluate(() => window.scrollTo(0, 0));
    await expect(page.locator(".cm-yaml-error-mark")).toBeInViewport();
    expect(state.writes).toHaveLength(0);
  });
}

for (const width of [1280, 390]) {
  test(`YAML validation does not shift content at ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 844 });
    const { state } = await backend(page);
    state.yaml = `${originalYaml}\n${"# More configuration\n".repeat(200)}value: "ok"`;
    await page.goto("/");
    const editor = await openYaml(page);
    await page.evaluate(() => window.scrollTo(0, document.documentElement.scrollHeight));
    const line = editor.locator(".cm-line").last();
    await line.click();
    await page.keyboard.press("End");
    const measure = () =>
      page.evaluate(() => ({
        toolbar: document.querySelector(".settings-toolbar")!.getBoundingClientRect().height,
        scroll: window.scrollY,
        editor: document.querySelector(".cm-content")!.getBoundingClientRect().x,
        height: document.documentElement.scrollHeight,
        line: [...document.querySelectorAll(".cm-line")].at(-1)!.getBoundingClientRect().y,
      }));
    const before = await measure();
    await page.keyboard.type("x");
    await expect(page.locator(".cm-yaml-error-gutter")).toBeVisible();
    await expect(page.locator(".cm-yaml-error-mark")).toHaveText("x");
    expect(await measure()).toEqual(before);
    await page.keyboard.press("Backspace");
    await expect(page.locator(".cm-yaml-error-mark")).toHaveCount(0);
    expect(await measure()).toEqual(before);
  });
}

for (const sample of [
  { yaml: "app:\n  httpWeb:\n    enabled: 123", token: "123" },
  { yaml: 'app:\n  httpWeb:\n    auth:\n      enabled: "true"', token: '"true"' },
  { yaml: "app:\n  dnsProxy:\n    disableFakePTR: wrong", token: "wrong" },
  { yaml: 'app:\n  dnsProxy:\n    host:\n      port: "53"', token: '"53"' },
  { yaml: "app:\n  dnsProxy:\n    timeout: false", token: "false" },
  { yaml: "app:\n  netfilter:\n    disableIPv6: 0", token: "0" },
  { yaml: "app:\n  keeneticPolicy:\n    enabled: 1", token: "1" },
  { yaml: "app:\n  link: wrong", token: "wrong" },
  { yaml: "groups:\n  - enable: 5", token: "5" },
  { yaml: "groups:\n  - rules:\n      - enable: 4", token: "4" },
  { yaml: "subscriptions:\n  - overrides:\n      - enable: 2", token: "2" },
  { yaml: "subscriptions:\n  - interval: -1", token: "-1" },
  { yaml: "profiles:\n  - interfaces: false", token: "false" },
  { yaml: "app: false", token: "false" },
  { yaml: "groups:\n  - priority: null", token: "null" },
  { yaml: "app:\n  showAllInterfaces: &bad 3\n  httpWeb:\n    enabled: *bad", token: "3" },
]) {
  test(`YAML field types highlight the incorrect value: ${JSON.stringify(sample.yaml)}`, async ({
    page,
  }) => {
    const { state } = await backend(page);
    await page.goto("/");
    const editor = await openYaml(page);
    await editor.fill(sample.yaml);
    await expect(page.locator(".cm-yaml-error-mark").first()).toHaveText(sample.token);
    await expect(page.locator("#save-settings")).toBeDisabled();
    await expect(editor).toHaveAttribute("aria-invalid", "true");
    expect(state.writes).toHaveLength(0);
    await editor.fill(editedYaml);
    await expect(page.locator(".cm-yaml-error-mark")).toHaveCount(0);
    await expect(page.locator("#save-settings")).toBeEnabled();
    await page.locator("#save-settings").click();
    await expect.poll(() => state.yaml).toBe(editedYaml);
  });
}

test("YAML type checks preserve legacy booleans, numeric scalars, null overlays and aliases", async ({
  page,
}) => {
  await backend(page);
  await page.goto("/");
  const editor = await openYaml(page);
  await editor.fill(`app:
  httpWeb:
    enabled: YES
    auth:
      enabled: off
    host:
      port: 53.0
  showAllInterfaces: &flag true
  netfilter:
    disableIPv4: *flag
    disableIPv6: null
  dnsProxy:
    timeout: 1000
    maxIdleConns: 18446744073709551615
  link: [br0]
groups:
  - id: 12345678
    priority: 100
    enable: false
subscriptions: []
profiles: []
`);
  await expect(page.locator(".cm-yaml-error-mark")).toHaveCount(0);
  await expect(page.locator("#save-settings")).toBeEnabled();
});

test("YAML error tooltip belongs to the line number without a toolbar indicator", async ({
  page,
}) => {
  await backend(page);
  await page.goto("/");
  const editor = await openYaml(page);
  await editor.fill("app:\n  httpWeb:\n    enabled: 123");
  await expect(page.getByRole("button", { name: "Go to YAML error", exact: true })).toHaveCount(0);
  await page.getByRole("tab", { name: "YAML", exact: true }).hover();
  await expect(page.locator("#global-tooltip")).toHaveCount(0);
  const marker = page.locator(".cm-yaml-error-gutter");
  await expect(marker).toHaveText("3");
  await marker.hover();
  await expect(page.locator("#global-tooltip")).toHaveText(
    "Expected a boolean value (true or false)",
  );
  await page.getByRole("tab", { name: "YAML", exact: true }).hover();
  await expect(page.locator("#global-tooltip")).toHaveCount(0);
  await marker.hover();
  await editor.fill(editedYaml);
  await expect(marker).toHaveCount(0);
  await expect(page.locator("#global-tooltip")).toHaveCount(0);
});

for (const width of [1280, 390]) {
  test(`YAML shows all type errors and clears only the corrected one at ${width}px`, async ({
    page,
  }) => {
    await page.setViewportSize({ width, height: 844 });
    const { state } = await backend(page);
    await page.goto("/");
    const editor = await openYaml(page);
    const invalid =
      'app:\r\n  httpWeb:\r\n    enabled: 123\r\n    host:\r\n      port: "53"\r\n  link: false\r\ngroups:\r\n  - enable: 456\r\n';
    await editor.fill(invalid);
    await expect(page.locator(".cm-yaml-error-mark")).toHaveText(["123", '"53"', "false", "456"]);
    await expect(page.locator(".cm-yaml-error-gutter")).toHaveText(["3", "5", "6", "8"]);
    await page.locator(".cm-yaml-error-gutter").nth(0).hover();
    await expect(page.locator("#global-tooltip")).toHaveText(
      "Expected a boolean value (true or false)",
    );
    await page.locator(".cm-yaml-error-gutter").nth(1).hover();
    await expect(page.locator("#global-tooltip")).toHaveText("Expected a nonnegative integer");
    await page.locator(".cm-yaml-error-gutter").nth(2).hover();
    await expect(page.locator("#global-tooltip")).toHaveText("Expected a YAML sequence");
    await page.mouse.move(0, 0);
    await editor.fill(invalid.replace("enabled: 123", "enabled: true"));
    await expect(page.locator(".cm-yaml-error-mark")).toHaveText(['"53"', "false", "456"]);
    await expect(page.locator(".cm-yaml-error-gutter")).toHaveText(["5", "6", "8"]);
    await expect(page.locator("#save-settings")).toBeDisabled();
    expect(state.writes).toHaveLength(0);
    await editor.fill(editedYaml);
    await expect(page.locator(".cm-yaml-error-mark")).toHaveCount(0);
    await expect(page.locator("#save-settings")).toBeEnabled();
  });
}

test("YAML shows multiple syntax errors alongside type errors", async ({ page }) => {
  await backend(page);
  await page.goto("/");
  const editor = await openYaml(page);
  await editor.fill(
    'app:\n  showAllInterfaces: true\n  showAllInterfaces: false\n  logLevel: info\n  logLevel: debug\n  httpWeb: {enabled: 12, host: {port: "53"}}',
  );
  await expect(page.locator(".cm-yaml-error-mark")).toHaveText(["s", "l", "12", '"53"']);
  await expect(page.locator(".cm-yaml-error-gutter")).toHaveText(["3", "5", "6"]);
  await page.locator(".cm-yaml-error-gutter").last().hover();
  await expect(page.locator("#global-tooltip")).toContainText("Expected a boolean value");
  await expect(page.locator("#global-tooltip")).toContainText("Expected a nonnegative integer");
  await expect(page.locator("#save-settings")).toBeDisabled();
});
