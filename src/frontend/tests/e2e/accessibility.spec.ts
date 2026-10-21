import { expect, test } from "@playwright/test";

import { GroupsPage } from "./pages/GroupsPage";

test.describe("Accessibility", () => {
  let groupsPage: GroupsPage;

  test.beforeEach(async ({ page }) => {
    groupsPage = new GroupsPage(page);
    await page.route("**/auth", async (route) => route.fulfill({ json: { enabled: false } }));
    await page.route("**/groups?with_rules=true", async (route) =>
      route.fulfill({ json: { groups: [] } }),
    );
    await page.route("**/interfaces", async (route) => route.fulfill({ json: { interfaces: [] } }));
    await groupsPage.goto();
  });

  test("should focus group name on creation", async ({ page }) => {
    await groupsPage.createGroup();

    // After creation, focus should be on the group name input
    const groupNameInput = page.locator(".group-wrapper").first().locator("input.group-name");
    await expect(groupNameInput).toBeFocused();
  });

  test("should tab through group controls", async ({ page }) => {
    await groupsPage.createGroup();
    const groupNameInput = page.locator(".group-wrapper").first().locator("input.group-name");

    // Focus Name
    await groupNameInput.focus();

    // Tab -> Interface Select (First select in the group)
    await page.keyboard.press("Tab");
    const interfaceTrigger = page
      .locator(".group-wrapper")
      .first()
      .locator("[data-select-trigger]")
      .first();
    await expect(interfaceTrigger).toBeFocused();

    // Tab -> Switch (Enable/Disable)
    await page.keyboard.press("Tab");
    const switchBtn = page.locator(".group-wrapper").first().locator(".enable-group");
    await expect(switchBtn).toBeFocused();

    // Tab -> Delete Group Button
    await page.keyboard.press("Tab");
    // Delete button is inside .group-actions.
    // Order: Select, Switch, Delete, Add, Import, Trigger.
    // Select is separate div. Switch is separate.
    // Then Delete.
    // Selector: .group-actions button that has Delete icon, or just next one.
    // Let's use checking if focused element contains delete icon logic or simpler:
    // Just check if we can reach it.
    // Let's assume we want to verify navigation flow.
    const deleteBtn = page
      .locator(".group-wrapper")
      .first()
      .locator(".group-actions button")
      .nth(2);
    // nth(0) is Select, nth(1) is Switch, nth(2) is Delete.
    await expect(deleteBtn).toBeFocused();
  });
});

for (const width of [390, 1280]) {
  test.describe(`Focus after adding at ${width}px`, () => {
    test.beforeEach(async ({ page }) => {
      await page.setViewportSize({ width, height: 844 });
      await page.route("**/api/v1/**", (route) => {
        const path = new URL(route.request().url()).pathname.split("/").pop();
        if (path === "rules") return route.fulfill({ json: { count: 1, types: { namespace: 1 } } });
        const fixtures: Record<string, unknown> = {
          auth: { enabled: false },
          groups: { groups: [] },
          subscriptions: { subscriptions: [] },
          interfaces: { interfaces: [] },
          profiles: { profiles: [] },
          status: { stage: "idle" },
        };
        return route.fulfill({ json: fixtures[path ?? ""] ?? {} });
      });
      await page.goto("/");
    });
    test("new groups and rules focus the newly added item", async ({ page }) => {
      const groups = new GroupsPage(page);
      await groups.createGroup();
      const group = page.locator(".group-wrapper").first();
      await expect(group.locator("input.group-name")).toBeFocused();
      await page.keyboard.insertText("Required fields");
      await group.locator(".pattern input").fill("initial.example");
      await group.locator('[data-value="Collapse Group"] button').click();
      for (const name of ["First new rule", "Second new rule"]) {
        if (width < 700) {
          await group.locator(".group-actions [data-dropdown-menu-trigger]").click();
          await page.getByRole("button", { name: "Add Rule", exact: true }).click();
        } else {
          await groups.addRuleToGroup(0);
        }
        const input = group.locator('.rule[data-index="0"] .name input');
        await expect(input).toBeFocused();
        await expect(input).toBeInViewport();
        await page.keyboard.insertText(name);
        await expect(input).toHaveValue(name);
      }
      await expect(group.locator('.rule[data-index="2"] .pattern input')).toHaveValue(
        "initial.example",
      );
    });
    test("adding a subscription focuses the required URL", async ({ page }) => {
      if (width < 700) await page.locator(".mobile-dropdown-btn").click();
      await page.getByRole("tab", { name: "Subscriptions", exact: true }).click();
      await page.getByRole("button", { name: "Add Subscription", exact: true }).click();
      const url = page.getByRole("dialog").locator("#sub-url");
      await expect(url).toBeFocused();
      await page.keyboard.insertText("https://example.com/list.txt");
      await expect(url).toHaveValue("https://example.com/list.txt");
    });
  });
}
