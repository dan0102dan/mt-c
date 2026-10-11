import { getContext } from "svelte";

export type SectionControls = {
  title: string;
  canSave: boolean;
  dirty?: boolean;
  saveLabel: string;
  onSave: () => void;
  addLabel?: string;
  canAdd?: boolean;
  onAdd?: () => void;
};
export type SettingsSection = {
  element: HTMLElement;
  controls: () => SectionControls;
};
export const SETTINGS_SECTIONS = Symbol("settings-sections");
export type RegisterSection = (section: SettingsSection) => () => void;

// Editors own their draft/save logic. Only the active toolbar is shared;
// generic builds never have to import the Keenetic editor or its state.
export function settingsSection() {
  const register = getContext<RegisterSection>(SETTINGS_SECTIONS);
  return (element: HTMLElement, controls: () => SectionControls) => ({
    destroy: register({ element, controls }),
  });
}
