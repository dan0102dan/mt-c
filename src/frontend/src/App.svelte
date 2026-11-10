<script lang="ts">
  import "./app.css";

  import { onMount } from "svelte";

  import AuthPage from "./components/AuthPage.svelte";
  import AppLayout from "./components/layout/AppLayout.svelte";
  import { authState, token } from "./data/auth.svelte";
  import { fetchInterfaces } from "./data/interfaces.svelte";
  import { t } from "./data/locale.svelte";
  import { fetchProfiles } from "./data/profiles.svelte";
  import TooltipLayer from "./lib/tooltip/TooltipLayer.svelte";

  import { fetcher } from "./utils/fetcher";

  let opened = $state(false);
  const needsAuthentication = $derived(authState.enabled && !token.current);

  // Keep owners of in-memory drafts alive while a changed access setting
  // asks for a new login. The modal also makes portalled controls inert.
  $effect(() => {
    if (authState.checked && !needsAuthentication) opened = true;
  });

  function showAuthentication(dialog: HTMLDialogElement) {
    dialog.showModal();
    return { destroy: () => dialog.close() };
  }

  onMount(async () => {
    try {
      const { enabled } = await fetcher.get<{ enabled: boolean }>("/auth");
      authState.enabled = enabled;
    } catch (e) {
      console.error("Failed to check auth status", e);
    } finally {
      authState.checked = true;
    }
  });

  $effect(() => {
    if (token.current || (authState.checked && !authState.enabled)) {
      fetchInterfaces();
      void fetchProfiles();
    }
  });
</script>

{#if opened}
  <div class="authenticated-content" hidden={needsAuthentication} inert={needsAuthentication}>
    <AppLayout active={!needsAuthentication} />
  </div>
{/if}

{#if authState.checked && needsAuthentication}
  {#if opened}
    <dialog
      use:showAuthentication
      class="authentication-dialog"
      aria-label={t("Sign In")}
      oncancel={(event) => event.preventDefault()}
    >
      <AuthPage />
    </dialog>
  {:else}
    <AuthPage />
  {/if}
{/if}

<TooltipLayer />

<style>
  .authenticated-content:not([hidden]) {
    display: contents;
  }

  .authentication-dialog {
    position: fixed;
    inset: 0;
    width: 100%;
    height: 100%;
    max-width: none;
    max-height: none;
    margin: 0;
    padding: 0;
    border: 0;
    background: var(--bg-dark);
    color: var(--text);
    overflow: auto;
  }

  .authentication-dialog::backdrop {
    background: var(--bg-dark);
  }
</style>
