import { persistedState } from "../utils/persisted-state.svelte";

export const token = persistedState<string | undefined>("auth", undefined);

export const authState = $state({
  enabled: true,
  checked: false,
});

/** A running service can enable authentication without reloading this page. */
export function requireAuthentication() {
  authState.enabled = true;
  token.reset();
}
