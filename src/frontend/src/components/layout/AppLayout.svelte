<script lang="ts">
  import { Tabs } from "bits-ui";
  import { setContext, untrack } from "svelte";

  import { t } from "../../data/locale.svelte";
  import { fetchProfiles, profiles, profilesDirty } from "../../data/profiles.svelte";
  import ConfigTransferDialog from "../../modules/config-transfer/ConfigTransferDialog.svelte";
  import type { GroupsStore } from "../../modules/groups/groups.svelte";
  import GroupsView from "../../modules/groups/GroupsView.svelte";
  // import LogsPanel from "../../modules/logs/LogsPanel.svelte";
  import SettingsPanel from "../../modules/settings/SettingsPanel.svelte";
  import type { SubscriptionsStore } from "../../modules/subscriptions/subscriptions.svelte";
  import SubscriptionsView from "../../modules/subscriptions/SubscriptionsView.svelte";
  import { persistedState } from "../../utils/persisted-state.svelte";
  import Overlay from "../feedback/Overlay.svelte";
  import ScrollToTop from "../feedback/ScrollToTop.svelte";
  import SnowField from "../feedback/SnowField.svelte";
  import Toast from "../feedback/Toast.svelte";
  import HeaderSettings from "./HeaderSettings.svelte";

  import {
    configForApply,
    decodePortable,
    snapshotFromServer,
    type ServerSnapshot,
    type TransferPolicy,
  } from "../../modules/config-transfer/adapter";
  import {
    CONFIG_TRANSFER_CONTEXT,
    type ConfigTransferControls,
    type PolicyDraftSource,
  } from "../../modules/config-transfer/context";
  import {
    assertPlanCurrent,
    exportSnapshot,
    type ImportPlan,
    type PortableConfig,
    type Snapshot,
  } from "../../modules/config-transfer/model";
  import { toast } from "../../utils/events";
  import { fetcher } from "../../utils/fetcher";
  import { LayoutList, Menu, RSS, Settings } from "../ui/icons";

  const lastActiveTab = persistedState("active_tab", "groups");
  let active_tab = $state(lastActiveTab.current);
  let isMenuOpen = $state(false);
  let isRenderCompleteGroups = $state(false);
  let isRenderCompleteSubscriptions = $state(false);
  let isRenderComplete = $derived(isRenderCompleteGroups && isRenderCompleteSubscriptions);

  $effect(() => {
    lastActiveTab.current = active_tab;
    // Tab content stays mounted. onMount alone cannot refresh usage after
    // Groups/Subscriptions have saved a different profile assignment.
    if (active_tab === "settings") {
      untrack(() => void fetchProfiles({ fresh: true }));
    }
  });

  let groupsStore: GroupsStore | undefined;
  let subscriptionsStore: SubscriptionsStore | undefined;
  let policySource: PolicyDraftSource | undefined;

  let imported = $state<PortableConfig | undefined>();
  let previewServer = $state<ServerSnapshot | undefined>();
  let previewCurrent = $state<Snapshot | undefined>();
  let targetPolicies = $state<{ id: string; name: string; available: boolean }[]>([]);
  let transferBusy = $state(false);

  function currentSnapshot(server: ServerSnapshot): Snapshot {
    return snapshotFromServer(server.config, {
      ...(groupsStore?.dataLoaded ? { groups: $state.snapshot(groupsStore.data) } : {}),
      ...(subscriptionsStore?.dataLoaded
        ? { subscriptions: $state.snapshot(subscriptionsStore.data) }
        : {}),
      ...(profiles.loaded ? { profiles: $state.snapshot(profiles.draft) } : {}),
      ...(policySource?.get() ? { policy: policySource.get() as TransferPolicy } : {}),
    });
  }

  async function fetchSnapshot(): Promise<ServerSnapshot> {
    const value = await fetcher.get<ServerSnapshot>("/system/config/snapshot");
    if (
      !value?.revision ||
      !value.config?.app ||
      !value.config.groups ||
      !value.config.subscriptions
    )
      throw new Error("Invalid configuration snapshot response");
    return value;
  }

  async function exportCurrentConfig() {
    if (transferBusy) return;
    transferBusy = true;
    try {
      const server = await fetchSnapshot();
      const snapshot = currentSnapshot(server);
      const portable = exportSnapshot(snapshot, {
        application: "mt-c",
        version: server.config.configVersion,
        platform: Object.hasOwn(server.config.app, "keeneticPolicy") ? "keenetic" : "generic",
      });
      const blob = new Blob([JSON.stringify(portable, null, 2)], { type: "application/json" });
      const url = URL.createObjectURL(blob);
      const link = document.createElement("a");
      link.href = url;
      link.download = "config.mtrickle";
      document.body.appendChild(link);
      link.click();
      link.remove();
      setTimeout(() => URL.revokeObjectURL(url), 5000);
    } catch (error) {
      console.error("Configuration export failed", error);
      toast.error("Не удалось экспортировать текущую конфигурацию");
    } finally {
      transferBusy = false;
    }
  }

  async function openConfigImport(event: Event) {
    const input = event.currentTarget as HTMLInputElement;
    const file = input.files?.[0];
    input.value = "";
    if (!file || transferBusy) return;
    transferBusy = true;
    try {
      if (file.size > 20 * 1024 * 1024) throw new Error("Файл слишком большой");
      const parsed = decodePortable(await file.text());
      const server = await fetchSnapshot();
      const policyResponse =
        parsed.settings?.keeneticPolicy && server.config.app.keeneticPolicy
          ? await fetcher
              .get<{ policies: { id: string; name: string; available: boolean }[] }>(
                "/keenetic/policies",
                { silent: true, timeoutMs: 5000 },
              )
              .catch(() => ({ policies: [] }))
          : { policies: [] };
      targetPolicies = policyResponse.policies;
      previewServer = server;
      // Import only selected file sections. Unrelated editor drafts must not
      // enter the full server payload or block it with unfinished references.
      previewCurrent = snapshotFromServer(server.config);
      imported = parsed;
    } catch (error) {
      console.error("Configuration import failed", error);
      toast.error(error instanceof Error ? error.message : "Некорректный файл конфигурации");
    } finally {
      transferBusy = false;
    }
  }

  async function applyConfigImport(plan: ImportPlan) {
    if (transferBusy || !previewServer) return;
    transferBusy = true;
    try {
      assertPlanCurrent(plan, snapshotFromServer(previewServer.config));
      const nextConfig = configForApply(previewServer.config, plan.next);
      await fetcher.post<{ ok: boolean }>("/system/config/apply", {
        revision: previewServer.revision,
        config: nextConfig,
      });
      imported = undefined;
      previewServer = undefined;
      previewCurrent = undefined;
      // Keep drafts in untouched sections; refresh only confirmed changes,
      // including consumers affected by an imported routing profile.
      const changed = new Set(
        plan.changes
          .filter((change) => change.kind !== "unchanged")
          .map((change) => change.section),
      );
      if (changed.has("profiles")) {
        profiles.loaded = false;
        profiles.draft = [];
        profiles.baseline = "[]";
      }
      await Promise.all([
        changed.has("groups") ? groupsStore?.mount() : undefined,
        changed.has("subscriptions") ? subscriptionsStore?.mount() : undefined,
        changed.has("profiles") ? fetchProfiles({ fresh: true }) : undefined,
        changed.has("settings") ? policySource?.refresh() : undefined,
      ]);
      toast.success("Конфигурация импортирована");
    } catch (error) {
      console.error("Configuration import apply failed", error);
      toast.error(error instanceof Error ? error.message : "Не удалось импортировать конфигурацию");
    } finally {
      transferBusy = false;
    }
  }

  const transferControls: ConfigTransferControls = {
    registerGroups(store) {
      groupsStore = store;
      return () => {
        if (groupsStore === store) groupsStore = undefined;
      };
    },
    registerSubscriptions(store) {
      subscriptionsStore = store;
      return () => {
        if (subscriptionsStore === store) subscriptionsStore = undefined;
      };
    },
    registerPolicy(source) {
      policySource = source;
      return () => {
        if (policySource === source) policySource = undefined;
      };
    },
    exportConfig: exportCurrentConfig,
    importConfig: openConfigImport,
  };
  setContext(CONFIG_TRANSFER_CONTEXT, transferControls);

  const toggleMenu = () => (isMenuOpen = !isMenuOpen);
  const closeMenu = () => (isMenuOpen = false);

  function handleSearchShortcut(event: KeyboardEvent) {
    if (
      event.isComposing ||
      event.altKey ||
      event.shiftKey ||
      !(event.ctrlKey || event.metaKey) ||
      event.code !== "KeyF"
    ) {
      return;
    }

    if (document.querySelector('[data-dialog-content][data-state="open"]')) return;

    const searchInput = document.querySelector<HTMLInputElement>(
      '[data-tabs-content][data-state="active"] [data-page-search]',
    );
    if (!searchInput) return;

    event.preventDefault();
    searchInput.focus();
    searchInput.select();
  }
</script>

<svelte:window
  onkeydown={handleSearchShortcut}
  onbeforeunload={(event) => {
    if (profilesDirty()) {
      event.preventDefault();
      event.returnValue = "";
    }
  }}
/>

{#if imported && previewServer && previewCurrent}
  <ConfigTransferDialog
    open={true}
    initialSection={active_tab === "subscriptions"
      ? "subscriptions"
      : active_tab === "settings"
        ? "settings"
        : "groups"}
    {imported}
    current={previewCurrent}
    policies={targetPolicies}
    policySupported={Object.hasOwn(previewServer.config.app, "keeneticPolicy")}
    busy={transferBusy}
    onclose={() => {
      if (!transferBusy) {
        imported = undefined;
        previewServer = undefined;
        previewCurrent = undefined;
      }
    }}
    onapply={applyConfigImport}
  />
{/if}

<Toast />
<Overlay />
<ScrollToTop />
{#if [11, 0, 1].includes(new Date().getMonth())}
  <SnowField visible={isRenderComplete} />
{/if}

<main>
  <Tabs.Root bind:value={active_tab} class="app-tabs">
    <nav>
      <div class="nav-left">
        <button
          type="button"
          class="mobile-dropdown-btn"
          aria-label={isMenuOpen ? "Close menu" : "Open menu"}
          aria-expanded={isMenuOpen}
          onclick={toggleMenu}
        >
          <span class="menu-morph" class:open={isMenuOpen} aria-hidden="true">
            <Menu size={24} />
          </span>
        </button>

        <!-- Tabs container -->
        <div class="tabs-panel" class:open={isMenuOpen}>
          <Tabs.List>
            <Tabs.Trigger value="groups" onclick={closeMenu}>
              <span class="tab-icon"><LayoutList size={24} /></span>
              {t("Groups")}
            </Tabs.Trigger>

            <Tabs.Trigger value="subscriptions" onclick={closeMenu}>
              <span class="tab-icon"><RSS size={24} strokeWidth={3} /></span>
              {t("Subscriptions")}
            </Tabs.Trigger>

            <Tabs.Trigger value="settings" onclick={closeMenu}>
              <span class="tab-icon"><Settings size={24} /></span>
              {t("Settings")}
            </Tabs.Trigger>
          </Tabs.List>
        </div>
      </div>

      <div class="header-settings">
        <HeaderSettings />
      </div>
    </nav>

    <article>
      <Tabs.Content value="groups" class="app-tab-content">
        <GroupsView onRenderComplete={() => (isRenderCompleteGroups = true)} />
      </Tabs.Content>
      <Tabs.Content value="subscriptions" class="app-tab-content">
        <SubscriptionsView onRenderComplete={() => (isRenderCompleteSubscriptions = true)} />
      </Tabs.Content>
      <Tabs.Content value="settings" class="app-tab-content"><SettingsPanel /></Tabs.Content>
    </article>
  </Tabs.Root>
</main>

<style>
  main {
    display: flex;
    flex-direction: column;
    align-items: center;
    margin-bottom: 2rem;
    padding: 0.3rem;
  }

  :global(.app-tabs) {
    width: 100%;
    max-width: 1000px;
    display: flex;
    flex-direction: column;
  }

  article {
    width: 100%;
  }

  nav {
    display: flex;
    align-items: center;
    justify-content: space-between;
    width: 100%;
    max-width: 1000px;
    position: relative;
  }

  .nav-left {
    display: flex;
    align-items: center;
  }

  .mobile-dropdown-btn {
    display: none;
  }

  .tabs-panel {
    display: block;
    z-index: 10;
  }

  .tabs-panel :global([data-tabs-list]) {
    display: flex;
    flex-direction: row;
    gap: 1rem;
    background: transparent;
  }

  .tabs-panel :global([data-tabs-trigger]) {
    padding: 0.5rem 0.5rem;
    border: none;
    border-bottom: 2px solid transparent;
    font-size: 1.5rem;
    font-family: var(--font);
    background-color: transparent;
    color: var(--text-2);
    display: flex;
    align-items: center;
    gap: 0.5rem;
    cursor: pointer;
    white-space: nowrap;
    transition:
      color 0.2s,
      border-color 0.2s;
  }

  .tab-icon {
    display: flex;
  }

  .tabs-panel :global([data-tabs-trigger][data-state="active"]) {
    color: var(--blue-light-extra);
    border-color: var(--blue-light-extra);
  }

  .tabs-panel :global([data-tabs-trigger]:hover) {
    color: var(--text);
  }

  :global(.app-tab-content) {
    padding-top: 1rem;
    outline: none;
  }

  @media (max-width: 700px) {
    .mobile-dropdown-btn {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      padding: 0.5rem;
      border: 0;
      background: transparent;
      color: var(--text);
      cursor: pointer;
      user-select: none;
      -webkit-tap-highlight-color: transparent;
    }

    .tabs-panel {
      position: absolute;
      top: 100%;
      left: 0;
      right: 0;
      background: var(--bg-dark);
      border-top: 1px solid var(--border-light);
      border-bottom: 1px solid var(--border-light);
      padding: 1rem 0.7rem;
      display: flex;
      flex-direction: column;
      opacity: 0;
      visibility: hidden;
      pointer-events: none;
      transition: none;
    }

    .tabs-panel.open {
      opacity: 1;
      visibility: visible;
      pointer-events: auto;
      transform: translateY(0);
      transition:
        opacity 160ms ease,
        transform 160ms ease,
        visibility 0s linear 0s;
    }

    @supports selector(:has(*)) {
      @starting-style {
        .tabs-panel.open {
          transform: translateY(-10px);
        }
      }
    }

    .tabs-panel :global([data-tabs-list]) {
      flex-direction: column;
      gap: 1.2rem;
      align-items: flex-start;
    }

    .tabs-panel :global([data-tabs-trigger]) {
      font-size: 1.5rem;
      font-weight: 500;
      border-bottom: none;
      padding: 0;
      width: 100%;
      justify-content: flex-start;
      gap: 0.8rem;
      line-height: 1;
    }
  }

  .menu-morph {
    --icon: 24px;
    --dur-move: 170ms;
    --dur-fade: 110ms;
    --ease: cubic-bezier(0.2, 0.8, 0.2, 1);
    --offset: calc(var(--icon) * 7 / 24);
    --xscale: 1.08;

    width: var(--icon);
    height: var(--icon);
    display: inline-block;
  }

  .menu-morph :global(svg) {
    width: var(--icon);
    height: var(--icon);
    display: block;
  }

  .menu-morph :global(path) {
    vector-effect: non-scaling-stroke;
    transform-box: fill-box;
    transform-origin: center;
    will-change: transform, opacity;

    transition-property: transform, opacity;
    transition-duration: var(--dur-move), var(--dur-fade);
    transition-timing-function: var(--ease), ease-out;
  }

  .menu-morph:not(.open) :global(path:nth-child(2)) {
    transition-delay: 60ms, 0ms;
  }

  .menu-morph.open :global(path:nth-child(1)) {
    transform: translateY(var(--offset)) rotate(45deg) scale(var(--xscale));
  }

  .menu-morph.open :global(path:nth-child(3)) {
    transform: translateY(calc(var(--offset) * -1)) rotate(-45deg) scale(var(--xscale));
  }

  .menu-morph.open :global(path:nth-child(2)) {
    opacity: 0;
    transform: scaleX(0);
    transition-delay: 0ms, 0ms;
  }

  @media (prefers-reduced-motion: reduce) {
    .menu-morph :global(path),
    .tabs-panel.open {
      transition: none !important;
    }
  }
</style>
