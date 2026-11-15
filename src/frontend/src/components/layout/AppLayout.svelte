<script lang="ts">
  import { Tabs } from "bits-ui";
  import { setContext, untrack } from "svelte";

  import { t } from "../../data/locale.svelte";
  import {
    acceptProfileImport,
    fetchProfiles,
    profiles,
    profilesDirty,
    stageProfileImport,
  } from "../../data/profiles.svelte";
  import ConfigTransferDialog from "../../modules/config-transfer/ConfigTransferDialog.svelte";
  import type { GroupsStore } from "../../modules/groups/groups.svelte";
  import GroupsView from "../../modules/groups/GroupsView.svelte";
  // import LogsPanel from "../../modules/logs/LogsPanel.svelte";
  import SettingsPanel from "../../modules/settings/SettingsPanel.svelte";
  import {
    validateSubscriptionUrl,
    type SubscriptionsStore,
  } from "../../modules/subscriptions/subscriptions.svelte";
  import SubscriptionsView from "../../modules/subscriptions/SubscriptionsView.svelte";
  import { persistedState } from "../../utils/persisted-state.svelte";
  import Overlay from "../feedback/Overlay.svelte";
  import ScrollToTop from "../feedback/ScrollToTop.svelte";
  import SnowField from "../feedback/SnowField.svelte";
  import Toast from "../feedback/Toast.svelte";
  import HeaderActions from "./HeaderActions.svelte";

  import {
    assertImportApplied,
    configForApply,
    decodePortable,
    profilesWithUsage,
    snapshotFromServer,
    subscriptionEditorsForImport,
    type SavedSubscription,
    type ServerSnapshot,
    type TransferPolicy,
  } from "../../modules/config-transfer/adapter";
  import {
    CONFIG_TRANSFER_CONTEXT,
    type ConfigTransferControls,
    type ImportSaveSection,
    type PolicyDraftSource,
  } from "../../modules/config-transfer/context";
  import {
    projectImportDraft,
    rebaseImportDraft,
    stageImportDraft,
    type PendingImportDraft,
  } from "../../modules/config-transfer/draft";
  import {
    copy,
    exportSnapshot,
    fingerprint,
    sameConfiguration,
    type ImportPlan,
    type PortableConfig,
    type Section,
    type Snapshot,
  } from "../../modules/config-transfer/model";
  import { cleanProfiles, profileError } from "../../modules/settings/profiles-data";
  import type { Subscription } from "../../types";
  import { overlay, toast } from "../../utils/events";
  import { fetcher } from "../../utils/fetcher";
  import { HttpError } from "../../utils/http-error";
  import { isValidPriority } from "../../utils/priority";
  import { LayoutList, RSS, Settings } from "../ui/icons";
  import { PAGE_CONTROLS_MOTION, type PageControlsMotion } from "./page-controls-motion";

  let { active = true }: { active?: boolean } = $props();
  setContext<PageControlsMotion>(PAGE_CONTROLS_MOTION, {});

  const lastActiveTab = persistedState("active_tab", "groups");
  let active_tab = $state(lastActiveTab.current);
  let isRenderCompleteGroups = $state(false);
  let isRenderCompleteSubscriptions = $state(false);
  let isRenderComplete = $derived(isRenderCompleteGroups && isRenderCompleteSubscriptions);

  $effect(() => {
    lastActiveTab.current = active_tab;
    // Tab content stays mounted. onMount alone cannot refresh usage after
    // Groups/Subscriptions have saved a different profile assignment.
    if (active && active_tab === "settings") {
      untrack(() => void fetchProfiles({ fresh: true }));
    }
  });

  let groupsStore = $state.raw<GroupsStore>();
  let subscriptionsStore = $state.raw<SubscriptionsStore>();
  let policySource = $state.raw<PolicyDraftSource>();

  let importPreview = $state.raw<{
    file: PortableConfig;
    server: ServerSnapshot;
    current: Snapshot;
  }>();
  let targetPolicies = $state<{ id: string; name: string; available: boolean }[]>([]);
  let transferBusy = $state(false);
  let pendingImport = $state.raw<PendingImportDraft>();
  let importBaseline = $state.raw<ServerSnapshot>();
  let subscriptionRuleBaseline = $state.raw<readonly Subscription[]>();
  // A successful POST with a lost confirmation must be verified before a retry
  // sends another write against an obsolete revision.
  let unconfirmedSave = $state.raw<{ expected: Snapshot; editor: Snapshot }>();

  function currentSnapshot(server: ServerSnapshot): Snapshot {
    const names = new Map(profiles.list.map((profile) => [profile.id, profile.name]));
    const profileDraftChanged =
      profilesDirty() || profiles.draft.some((profile) => names.get(profile.id) !== profile.name);
    return snapshotFromServer(server.config, {
      ...(groupsStore?.dataLoaded && groupsStore.tracker.isDirty
        ? { groups: $state.snapshot(groupsStore.data) }
        : {}),
      ...(subscriptionsStore?.dataLoaded && subscriptionsStore.tracker.isDirty
        ? { subscriptions: $state.snapshot(subscriptionsStore.data) }
        : {}),
      ...(subscriptionRuleBaseline ? { subscriptionRuleBaseline } : {}),
      ...(profiles.loaded && profileDraftChanged
        ? { profiles: $state.snapshot(profiles.draft) }
        : {}),
      ...(policySource?.dirty() && policySource.get()
        ? { policy: policySource.get() as TransferPolicy }
        : {}),
    });
  }

  async function fetchSnapshot(): Promise<ServerSnapshot> {
    const value = await fetcher.get<ServerSnapshot>("/system/config/snapshot", { silent: true });
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
      toast.error(t("Failed to export current configuration"));
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
    let failure = "Invalid config file";
    try {
      const parsed = decodePortable(await file.text());
      failure = "Failed to import configuration";
      const server = await fetchSnapshot();
      if (unconfirmedSave && importBaseline) {
        let confirmed = false;
        try {
          assertImportApplied(unconfirmedSave.expected, server.config);
          confirmed = true;
        } catch {
          // A failed attempt may have left the original saved configuration intact.
        }
        if (confirmed) {
          acceptImportSave(server, unconfirmedSave.expected, unconfirmedSave.editor);
        } else if (server.revision !== importBaseline.revision) {
          failure =
            "Configuration changed on the server. Reload before saving; your draft is unchanged.";
          throw new Error(failure);
        } else {
          unconfirmedSave = undefined;
        }
      }
      const policyResponse =
        parsed.settings?.keeneticPolicy && server.config.app.keeneticPolicy
          ? await fetcher
              .get<{
                policies: { id: string; name: string; available: boolean }[];
              }>("/keenetic/policies", { silent: true, timeoutMs: 5000 })
              .catch(() => ({ policies: [] }))
          : { policies: [] };
      targetPolicies = policyResponse.policies;
      const previewServer = importBaseline ?? server;
      // A preview is one immutable session. Its data outlives the dialog's
      // closing animation, so derived values never observe cleared props.
      importPreview = {
        file: parsed,
        server: copy(previewServer),
        current: currentSnapshot(previewServer),
      };
    } catch (error) {
      console.error("Configuration import failed", error);
      toast.error(t(failure));
    } finally {
      transferBusy = false;
    }
  }

  function closeImportPreview() {
    importPreview = undefined;
  }

  function setImportPending(value: boolean) {
    if (groupsStore) groupsStore.importPending = value;
    if (subscriptionsStore) subscriptionsStore.importPending = value;
  }

  function savedSubscriptions(items: readonly SavedSubscription[]): Subscription[] {
    return items.map(({ last_update, ...item }) => ({
      ...copy(item),
      lastUpdate: last_update,
      rules: copy(item.rules ?? []),
    }));
  }

  function policySettings(snapshot: Snapshot): TransferPolicy | undefined {
    const value = snapshot.settings.keeneticPolicy;
    if (!value || typeof value !== "object" || Array.isArray(value)) return;
    if (
      typeof value.enabled !== "boolean" ||
      typeof value.policyId !== "string" ||
      (value.mode !== "include" && value.mode !== "exclude")
    )
      throw new Error("Invalid routing policy settings");
    return { enabled: value.enabled, policy_id: value.policyId, mode: value.mode };
  }

  function savingInProgress() {
    return Boolean(
      groupsStore?.saving ||
      subscriptionsStore?.saving ||
      profiles.saving ||
      policySource?.saving(),
    );
  }

  function saveSections(section: ImportSaveSection): Section[] {
    return section === "settings" ? ["profiles", "settings"] : [section];
  }

  function canSaveImport(section: ImportSaveSection): boolean {
    if (!pendingImport || transferBusy || savingInProgress()) return false;
    const selected = new Set([...pendingImport.sections, ...saveSections(section)]);
    if (selected.has("groups") && !groupsStore?.canSave) return false;
    if (selected.has("subscriptions") && !subscriptionsStore?.canSave) return false;
    const importedProfiles = new Set(pendingImport.implicit.profiles.map((item) => item.id));
    const definitions = selected.has("profiles")
      ? profiles.draft
      : profiles.draft.filter((profile) => importedProfiles.has(profile.id));
    if (profileError(definitions)) return false;
    if (selected.has("settings") && policySource && !policySource.valid()) return false;
    return true;
  }

  async function applyConfigImport(plan: ImportPlan) {
    if (transferBusy || !importPreview) return;
    const preview = importPreview;
    transferBusy = true;
    let failure = t("Failed to import configuration");
    const yieldUI = () => new Promise<void>((resolve) => setTimeout(resolve, 0));
    try {
      await yieldUI();
      if (savingInProgress()) {
        failure = t("Wait for the current save to finish before importing.");
        throw new Error(failure);
      }
      const before = currentSnapshot(preview.server);
      await yieldUI();
      if (plan.baseline !== fingerprint(before)) {
        failure = t("Configuration changed; refresh the import preview");
        throw new Error(failure);
      }
      if (plan.problems.length) throw new Error(plan.problems.join("\n"));
      const changed = new Set(
        plan.changes
          .filter((change) => change.kind !== "unchanged")
          .map((change) => change.section),
      );
      const baseline = importBaseline ?? preview.server;
      await yieldUI();
      const saved = snapshotFromServer(baseline.config);
      const next = plan.next;
      const staged = stageImportDraft(saved, before, plan, pendingImport, plan.changes);
      if (!changed.size) {
        let alreadySaved = false;
        try {
          assertImportApplied(projectImportDraft(staged, next, []), baseline.config);
          alreadySaved = true;
        } catch {
          // Identical editor values can still depend on an unsaved definition.
        }
        if (alreadySaved) {
          if (!unconfirmedSave) {
            // Even a no-op import may expose newer saved data than the mounted
            // editors. Rebase it read-only, retaining unrelated local drafts.
            importBaseline = baseline;
            acceptImportSave(baseline, saved, before, { persisted: false });
          }
          closeImportPreview();
          toast.info(t("No changes to import"));
          return;
        }
      }
      const policyBaseline = policySettings(saved);
      const policyDraft = policySettings(next);
      const subscriptions = subscriptionEditorsForImport(
        next.subscriptions,
        savedSubscriptions(baseline.config.subscriptions),
        subscriptionsStore ? copy(subscriptionsStore.data) : [],
      );
      // Prepare offscreen without publishing a partially staged transaction.
      const preparedGroups = changed.has("groups")
        ? await groupsStore?.prepareImport(saved.groups, next.groups)
        : undefined;
      const preparedSubscriptions = changed.has("subscriptions")
        ? await subscriptionsStore?.prepareImport(
            savedSubscriptions(baseline.config.subscriptions),
            subscriptions,
          )
        : undefined;
      // A background refresh or other editor mutation must not be overwritten
      // while preparation yields to the browser.
      preparedGroups?.validate();
      preparedSubscriptions?.validate();
      if (!sameConfiguration(currentSnapshot(preview.server), before))
        throw new Error(t("Configuration changed; refresh the import preview"));
      preparedGroups?.commit();
      preparedSubscriptions?.commit();
      if (changed.has("subscriptions")) subscriptionRuleBaseline = copy(subscriptions);
      if (changed.has("profiles"))
        stageProfileImport(
          profilesWithUsage(saved.profiles, saved),
          profilesWithUsage(next.profiles, next),
        );
      if (changed.has("settings") && policyBaseline && policyDraft)
        policySource?.stage(policyBaseline, policyDraft);
      importBaseline = baseline;
      pendingImport = staged;
      setImportPending(true);
      closeImportPreview();
      toast.success(t("Configuration imported"));
    } catch (error) {
      console.error("Configuration import staging failed", error);
      toast.error(failure);
    } finally {
      transferBusy = false;
    }
  }

  function validateImportSave(snapshot: Snapshot) {
    const invalidProfile = profileError(snapshot.profiles);
    if (invalidProfile) throw new Error(invalidProfile);
    const profileIds = new Set(snapshot.profiles.map((profile) => profile.id));
    for (const item of [...snapshot.groups, ...snapshot.subscriptions]) {
      if (!item.name.trim() || !isValidPriority(item.priority))
        throw new Error("Invalid imported configuration");
      if (item.profile && !profileIds.has(item.profile))
        throw new Error("Missing routing profile: " + item.profile);
    }
    if (snapshot.subscriptions.some((item) => !validateSubscriptionUrl(item.url)))
      throw new Error("Invalid URL");
    policySettings(snapshot);
  }

  function acceptImportSave(
    confirmed: ServerSnapshot,
    expected: Snapshot,
    editor: Snapshot,
    { persisted = true }: { persisted?: boolean } = {},
  ) {
    if (!importBaseline) return;
    const current = currentSnapshot(importBaseline);
    const saved = snapshotFromServer(confirmed.config);
    const retained = rebaseImportDraft(expected, current, saved, editor);
    const savedSubs = savedSubscriptions(confirmed.config.subscriptions);
    const retainedSubs = subscriptionEditorsForImport(
      retained.subscriptions,
      savedSubs,
      subscriptionsStore ? copy(subscriptionsStore.data) : [],
    );
    groupsStore?.acceptImport(copy(saved.groups), retained.groups, { persisted });
    subscriptionsStore?.acceptImport(savedSubs, retainedSubs, { persisted });
    const acceptedProfiles = profilesWithUsage(confirmed.config.profiles ?? [], saved);
    acceptProfileImport(acceptedProfiles, retained.profiles, { persisted });
    const policyBaseline = policySettings(saved);
    const policyDraft = policySettings(retained);
    if (policyBaseline && policyDraft) policySource?.accept(policyBaseline, policyDraft);
    pendingImport = undefined;
    importBaseline = undefined;
    unconfirmedSave = undefined;
    subscriptionRuleBaseline = undefined;
    setImportPending(false);
  }

  async function saveConfigImport(section: ImportSaveSection) {
    if (!importBaseline || !pendingImport || !canSaveImport(section)) return;
    transferBusy = true;
    overlay.show(t("saving changes..."));
    let failure = t("Failed to save imported configuration");
    try {
      if (unconfirmedSave) {
        const confirmed = await fetchSnapshot();
        let matches = false;
        try {
          assertImportApplied(unconfirmedSave.expected, confirmed.config);
          matches = true;
        } catch {
          // A rejected/aborted POST can be retried only at the original revision.
        }
        if (matches) {
          acceptImportSave(confirmed, unconfirmedSave.expected, unconfirmedSave.editor);
          toast.success(t("Saved"));
          return;
        }
        if (confirmed.revision !== importBaseline.revision) {
          failure = t(
            "Configuration changed on the server. Reload before saving; your draft is unchanged.",
          );
          throw new Error(failure);
        }
        unconfirmedSave = undefined;
      }
      const editor = currentSnapshot(importBaseline);
      const expected = projectImportDraft(pendingImport, editor, saveSections(section));
      validateImportSave(expected);
      unconfirmedSave = { expected: copy(expected), editor: copy(editor) };
      const response = await fetcher<unknown>("/system/config/apply", {
        method: "POST",
        silent: true,
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          revision: importBaseline.revision,
          config: configForApply(importBaseline.config, {
            ...expected,
            profiles: cleanProfiles(expected.profiles),
          }),
        }),
      });
      if (!response || typeof response !== "object" || !("ok" in response) || response.ok !== true)
        throw new Error("Invalid configuration apply acknowledgement");
      const confirmed = await fetchSnapshot();
      assertImportApplied(expected, confirmed.config);
      acceptImportSave(confirmed, expected, editor);
      toast.success(t("Saved"));
    } catch (error) {
      if (error instanceof HttpError && error.status === 409)
        failure = t(
          "Configuration changed on the server. Reload before saving; your draft is unchanged.",
        );
      console.error("Configuration import save failed", error);
      toast.error(failure);
    } finally {
      transferBusy = false;
      overlay.hide();
    }
  }

  const transferControls: ConfigTransferControls = {
    registerGroups(store) {
      groupsStore = store;
      store.importPending = Boolean(pendingImport);
      store.delegateSave = () => saveConfigImport("groups");
      return () => {
        store.delegateSave = undefined;
        if (groupsStore === store) groupsStore = undefined;
      };
    },
    registerSubscriptions(store) {
      subscriptionsStore = store;
      store.importPending = Boolean(pendingImport);
      store.delegateSave = () => saveConfigImport("subscriptions");
      return () => {
        store.delegateSave = undefined;
        if (subscriptionsStore === store) subscriptionsStore = undefined;
      };
    },
    registerPolicy(source) {
      policySource = source;
      return () => {
        if (policySource === source) policySource = undefined;
      };
    },
    connectionChangeBlocked: () =>
      transferBusy ||
      !!(groupsStore && !groupsStore.dataLoaded) ||
      !!(subscriptionsStore && !subscriptionsStore.dataLoaded) ||
      !!groupsStore?.hasUnsavedChanges ||
      !!subscriptionsStore?.hasUnsavedChanges ||
      !!groupsStore?.saving ||
      !!subscriptionsStore?.saving,
    pending: () => Boolean(pendingImport),
    busy: () => transferBusy,
    canSave: canSaveImport,
    save: saveConfigImport,
    routeProfiles: () => {
      if (!pendingImport) return profiles.list;
      if (pendingImport.sections.includes("profiles")) return profiles.draft;
      const importedIds = new Set(pendingImport.implicit.profiles.map((item) => item.id));
      const available = profiles.list.filter((profile) => !importedIds.has(profile.id));
      return [...available, ...profiles.draft.filter((profile) => importedIds.has(profile.id))];
    },
    exportConfig: exportCurrentConfig,
    importConfig: openConfigImport,
  };
  setContext(CONFIG_TRANSFER_CONTEXT, transferControls);

  function handleSearchShortcut(event: KeyboardEvent) {
    if (
      !active ||
      event.defaultPrevented ||
      event.isComposing ||
      event.altKey ||
      event.shiftKey ||
      !(event.ctrlKey || event.metaKey) ||
      event.code !== "KeyF"
    ) {
      return;
    }

    if (document.querySelector('[data-dialog-content][data-state="open"]')) return;

    const searchInput = Array.from(
      document.querySelectorAll<HTMLInputElement>(
        '[data-tabs-content][data-state="active"] [data-page-search]',
      ),
    ).find((input) => input.getClientRects().length > 0);
    if (!searchInput) return;

    event.preventDefault();
    searchInput.focus();
    searchInput.select();
  }
</script>

<svelte:window
  onkeydown={handleSearchShortcut}
  onbeforeunload={(event) => {
    if (pendingImport || profilesDirty()) {
      event.preventDefault();
      event.returnValue = "";
    }
  }}
/>

{#each importPreview ? [importPreview] : [] as preview (preview)}
  <ConfigTransferDialog
    open={true}
    initialSection={active_tab === "subscriptions"
      ? "subscriptions"
      : active_tab === "settings"
        ? "settings"
        : "groups"}
    imported={preview.file}
    current={preview.current}
    policies={targetPolicies}
    policySupported={Object.hasOwn(preview.server.config.app, "keeneticPolicy")}
    busy={transferBusy}
    onclose={() => {
      if (!transferBusy) closeImportPreview();
    }}
    onapply={applyConfigImport}
  />
{/each}

<Toast />
<Overlay />
<ScrollToTop />
{#if [11, 0, 1].includes(new Date().getMonth())}
  <SnowField visible={isRenderComplete} />
{/if}

<main>
  <Tabs.Root bind:value={active_tab} class="app-tabs">
    <nav aria-label={t("Main navigation")}>
      <div class="nav-left">
        <div class="tabs-panel">
          <Tabs.List aria-label={t("Main navigation")}>
            <Tabs.Trigger value="groups">
              <span class="tab-icon" aria-hidden="true"><LayoutList size={24} /></span>
              {t("Groups")}
            </Tabs.Trigger>

            <Tabs.Trigger value="subscriptions">
              <span class="tab-icon" aria-hidden="true"><RSS size={24} strokeWidth={3} /></span>
              {t("Subscriptions")}
            </Tabs.Trigger>

            <Tabs.Trigger value="settings">
              <span class="tab-icon" aria-hidden="true"><Settings size={24} /></span>
              {t("Settings")}
            </Tabs.Trigger>
          </Tabs.List>
        </div>
      </div>

      <HeaderActions />
    </nav>

    <article>
      <Tabs.Content value="groups" class="app-tab-content">
        <GroupsView
          active={active && active_tab === "groups"}
          onRenderComplete={() => (isRenderCompleteGroups = true)}
        />
      </Tabs.Content>
      <Tabs.Content value="subscriptions" class="app-tab-content">
        <SubscriptionsView
          active={active && active_tab === "subscriptions"}
          onRenderComplete={() => (isRenderCompleteSubscriptions = true)}
        />
      </Tabs.Content>
      <Tabs.Content value="settings" class="app-tab-content"
        ><SettingsPanel active={active && active_tab === "settings"} /></Tabs.Content
      >
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
    gap: var(--space-3);
    width: 100%;
    max-width: 1000px;
    position: relative;
  }

  .nav-left {
    display: flex;
    align-items: center;
    min-width: 0;
    flex: 1;
  }

  .tabs-panel {
    display: block;
    min-width: 0;
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

  .tabs-panel :global([data-tabs-trigger]:focus-visible) {
    outline: 2px solid var(--accent);
    outline-offset: 2px;
  }

  :global(.app-tab-content) {
    padding-top: 1rem;
    outline: none;
  }

  @media (max-width: 700px) {
    .tabs-panel {
      width: 100%;
    }

    .tabs-panel :global([data-tabs-list]) {
      width: 100%;
      gap: var(--space-1);
    }

    .tabs-panel :global([data-tabs-trigger]) {
      flex: 1 1 0;
      min-width: 0;
      flex-direction: column;
      justify-content: center;
      gap: 0.3rem;
      padding: 0.5rem 0.1rem;
      font-size: 0.85rem;
      line-height: 1.3;
    }
    .tab-icon :global(svg) {
      width: 20px;
      height: 20px;
    }
  }
</style>
