import { expect, type Page } from "@playwright/test";

export async function openInterface(page: Page) {
  await page.getByRole("tab", { name: /^(Settings|Настройки)$/ }).click();
  await page.getByRole("tab", { name: /^(Interface|Интерфейс)$/ }).click();
  await expect(page.locator("#interface")).toBeVisible();
}

export async function selectLanguage(page: Page, language: "English" | "Русский") {
  await page.getByLabel(/^(Language|Язык)$/).click();
  await page.getByRole("option", { name: language, exact: true }).click();
}
