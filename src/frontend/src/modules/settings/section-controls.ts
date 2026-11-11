import { getContext, type Snippet } from "svelte";

import type { ContextAction } from "../../components/layout/PageControls.svelte";

export type SectionControls = {
  canSave: boolean;
  dirty: boolean;
  /** Confirm before any section is written; runtime changes run last. */
  prepareSave?: () => Promise<boolean>;
  saveLast?: boolean;
  validationMessage?: string;
  toolbarFeedback?: Snippet;
  dialog?: Snippet;
  onSave: () => Promise<void>;
  addLabel?: string;
  canAdd?: boolean;
  onAdd?: () => void;
  contextAction?: ContextAction;
  hideImport?: boolean;
  exportAction?: { label: string; disabled: boolean; onclick: () => void };
};
export type SettingsSection = {
  element: HTMLElement;
  controls: () => SectionControls;
};
export const SETTINGS_SECTIONS = Symbol("settings-sections");
export type RegisterSection = (section: SettingsSection) => () => void;

// Each section owns its draft and API write. The page coordinates one Save
// across all dirty sections; generic builds never import Keenetic code.
export function settingsSection() {
  const register = getContext<RegisterSection>(SETTINGS_SECTIONS);
  return (element: HTMLElement, controls: () => SectionControls) => ({
    destroy: register({ element, controls }),
  });
}
