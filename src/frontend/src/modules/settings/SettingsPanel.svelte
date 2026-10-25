<script lang="ts">
  import PlatformSettings from "#platform/settings";
  import { Tabs } from "bits-ui";
  import { getContext, setContext } from "svelte";

  import PageControls from "../../components/layout/PageControls.svelte";
  import { t } from "../../data/locale.svelte";
  import DnsQueryLog from "./dns/DnsQueryLog.svelte";
  import ProfilesPanel from "./ProfilesPanel.svelte";

  import { toast } from "../../utils/events";
  import { CONFIG_TRANSFER_CONTEXT, type ConfigTransferControls } from "../config-transfer/context";
  import {
    SETTINGS_SECTIONS,
    type RegisterSection,
    type SettingsSection,
  } from "./section-controls";

  let { active = true }: { active?: boolean } = $props();

  const transfer = getContext<ConfigTransferControls>(CONFIG_TRANSFER_CONTEXT);
  let sections = $state.raw<SettingsSection[]>([]);
  let saving = $state(false);
  let selected = $state("profiles");
  const visibleSection = $derived(sections.find((section) => section.element.id === selected));
  const tabLabels = $derived<Record<string, string>>({
    profiles: t("Profiles"),
    keenetic: "Keenetic",
    dns: t("DNS queries"),
  });
  const visibleControls = $derived(visibleSection?.controls());
  const dirty = $derived(sections.some((section) => section.controls().dirty));
  const invalid = $derived(
    sections.find((section) => section.controls().dirty && !section.controls().canSave)?.controls(),
  );
  const validationSections = $derived(
    sections.filter((section) => {
      const controls = section.controls();
      return controls.dirty && controls.validationMessage;
    }),
  );

  const canSave = $derived(dirty && !saving && !invalid);

  async function saveAll() {
    if (!canSave) return;
    // Both section APIs persist the same configuration file: serialize
    // writes. The failed section remains dirty; other edits still save.
    const pending = sections.filter((section) => section.controls().dirty);
    saving = true;
    let succeeded = 0;
    let failed = 0;
    try {
      for (const section of pending) {
        try {
          await section.controls().onSave();
          succeeded++;
        } catch {
          failed++;
        }
      }
      if (!failed) toast.success(t("Settings saved"));
      else if (succeeded) toast.warning(t("Some settings could not be saved"));
    } finally {
      saving = false;
    }
  }

  setContext<RegisterSection>(SETTINGS_SECTIONS, (section) => {
    sections = [...sections, section];
    return () => {
      sections = sections.filter((s) => s !== section);
    };
  });
</script>

<Tabs.Root bind:value={selected} class="settings-page">
  <div class="settings-toolbar">
    <PageControls
      {canSave}
      controlsClass="settings-controls"
      {active}
      onSave={() => void saveAll()}
      saveButtonId="save-settings"
      saveLabel={t("Save Changes")}
      saveDescriptionId={validationSections.length
        ? validationSections.map((section) => `settings-validation-${section.element.id}`).join(" ")
        : undefined}
      exportLabel={t("Export Config")}
      importLabel={t("Import Config")}
      onExport={() => void transfer.exportConfig()}
      onImport={(event) => void transfer.importConfig(event)}
      addLabel={visibleControls?.addLabel}
      canAdd={visibleControls?.canAdd ?? false}
      onAdd={visibleControls?.onAdd}
    >
      {#snippet search()}
        <Tabs.List class="settings-tabs" aria-label={t("Settings")}>
          {#each sections as section (section.element.id)}
            <Tabs.Trigger value={section.element.id}>{tabLabels[section.element.id]}</Tabs.Trigger>
          {/each}
        </Tabs.List>
      {/snippet}
    </PageControls>
    {#each validationSections as section (section.element.id)}
      <span
        id={`settings-validation-${section.element.id}`}
        class="validation-description"
        role="status"
      >
        {tabLabels[section.element.id]}: {section.controls().validationMessage}
      </span>
    {/each}
  </div>
  <Tabs.Content value="profiles" class="settings-content"><ProfilesPanel /></Tabs.Content>
  <Tabs.Content value="keenetic" class="settings-content"><PlatformSettings /></Tabs.Content>
  <Tabs.Content value="dns" class="settings-content"
    ><DnsQueryLog active={active && selected === "dns"} /></Tabs.Content
  >
</Tabs.Root>

<style>
  :global(.settings-page) {
    min-width: 0;
  }
  .settings-toolbar {
    position: sticky;
    top: 0;
    z-index: 5;
    background: var(--bg-dark);
    border-bottom: 1px solid var(--bg-light-extra);
    padding-bottom: 0.45rem;
    --page-controls-background: var(--bg-dark);
  }
  .settings-toolbar :global(.settings-controls) {
    display: flex;
    flex-wrap: wrap;
    margin-bottom: 0;
    padding: 0.3rem 0;
  }
  .settings-toolbar :global(.page-controls-search) {
    padding-right: 0;
    transition: none;
  }
  .settings-toolbar :global(.page-controls-actions) {
    position: static;
    height: auto;
    flex-shrink: 0;
    margin-left: auto;
  }
  :global(.settings-content[data-state="inactive"]) {
    display: none;
  }
  :global(.settings-content) {
    min-width: 0;
    padding-top: 1rem;
  }
  .validation-description,
  :global(.settings-content > section > .sr-only) {
    position: absolute;
    width: 1px;
    height: 1px;
    padding: 0;
    margin: -1px;
    overflow: hidden;
    clip-path: inset(50%);
    white-space: nowrap;
    border: 0;
  }
  :global(.settings-content .settings-description) {
    margin: 0 0 1.25rem;
    color: var(--text-2);
    font: 400 0.95rem / 1.5 var(--font);
  }
  :global(.settings-tabs) {
    display: flex;
    flex-wrap: wrap;
    gap: 0.5rem;
  }
  :global(.settings-tabs [data-tabs-trigger]) {
    border: 0;
    border-bottom: 2px solid transparent;
    border-radius: 0;
    padding: 0.5rem;
    background: transparent;
    color: var(--text-2);
    font: 400 1.1rem / 1.4 var(--font);
    cursor: pointer;
  }
  :global(.settings-tabs [data-tabs-trigger][data-state="active"]) {
    color: var(--blue-light-extra);
    border-bottom-color: var(--blue-light-extra);
  }
  :global(.settings-tabs [data-tabs-trigger]:hover) {
    color: var(--text);
  }
  :global(.settings-tabs [data-tabs-trigger]:focus-visible) {
    outline: 2px solid var(--accent);
    outline-offset: 2px;
  }
  @media (max-width: 700px) {
    .settings-toolbar :global(.page-controls-search) {
      width: 100%;
    }
  }
</style>
