import { persistedState } from "../utils/persisted-state.svelte";

function priorityVisibility(key: string) {
  const stored = localStorage.getItem(key);
  if (stored !== null) {
    let valid = false;
    try {
      const parsed: unknown = JSON.parse(stored);
      valid =
        typeof parsed === "object" &&
        parsed !== null &&
        "value" in parsed &&
        typeof parsed.value === "boolean";
    } catch {
      // Invalid preferences fall back to showing the existing controls.
    }
    if (!valid) localStorage.removeItem(key);
  }
  return persistedState<boolean>(key, true);
}

export const showGroupPriority = priorityVisibility("show_group_priority");
export const showSubscriptionPriority = priorityVisibility("show_subscription_priority");
