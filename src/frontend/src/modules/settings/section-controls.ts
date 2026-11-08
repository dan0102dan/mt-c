import { getContext, type Snippet } from "svelte";

import type { ContextAction } from "../../components/layout/PageControls.svelte";

export type SectionControls = {
  canSave: boolean;
  dirty: boolean;
  /** Whole-file writers save separately from structured settings sections. */
  exclusive?: boolean;
  validationMessage?: string;
  toolbarFeedback?: Snippet;
  onSave: () => Promise<void>;
  addLabel?: string;
  canAdd?: boolean;
  onAdd?: () => void;
  contextAction?: ContextAction;
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
