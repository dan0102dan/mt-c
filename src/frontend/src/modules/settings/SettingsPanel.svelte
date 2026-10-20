<script lang="ts">
  import PlatformSettings from "#platform/settings";
  import { getContext, onMount, setContext } from "svelte";

  import PageControls from "../../components/layout/PageControls.svelte";
  import { t } from "../../data/locale.svelte";
  import DnsQueryLog from "./DnsQueryLog.svelte";
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
  let page: HTMLDivElement;
  let toolbar: HTMLDivElement;
  let sections = $state.raw<SettingsSection[]>([]);
  let saving = $state(false);
  let titleIndex = $state(0);
  const visibleSection = $derived(sections[titleIndex]);
  const visibleControls = $derived(visibleSection?.controls());
  const dirty = $derived(sections.some((section) => section.controls().dirty));
  const invalid = $derived(
    sections.find((section) => section.controls().dirty && !section.controls().canSave)?.controls(),
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
      titleIndex = Math.min(titleIndex, Math.max(0, sections.length - 1));
    };
  });

  onMount(() => {
    let intersection: IntersectionObserver | undefined;
    const passed = new Set<SettingsSection>();
    const selectHeading = () => {
      let next = 0;
      sections.forEach((section, index) => {
        if (passed.has(section)) next = index;
      });
      if (next !== titleIndex) {
        titleIndex = next;
      }
    };
    // CSS sticky owns the toolbar. Measure only on resize/section crossings,
    // not on every scroll event; header changes must not replace action nodes.
    const observeHeadings = () => {
      if (!page.getClientRects().length) return;
      intersection?.disconnect();
      const title = toolbar.querySelector<HTMLElement>(".settings-title")!;
      // Use a stable switching line even when the active title makes the
      // toolbar shorter. Otherwise scroll anchoring can flip it back and forth.
      const tallestTitle = Math.max(
        ...Array.from(
          toolbar.querySelectorAll<HTMLElement>(".title-layer"),
          (layer) => layer.scrollHeight,
        ),
      );
      const inset = Math.ceil(
        toolbar.getBoundingClientRect().height +
          Math.max(0, tallestTitle - title.getBoundingClientRect().height),
      );
      passed.clear();
      const targets = new Map<Element, SettingsSection>();
      for (const section of sections) {
        const anchor = section.element.querySelector(":scope > h2");
        if (!anchor) continue;
        targets.set(anchor, section);
        if (anchor.getBoundingClientRect().bottom <= inset) passed.add(section);
      }
      selectHeading();
      intersection = new IntersectionObserver(
        () => {
          if (!page.getClientRects().length) return;
          for (const [anchor, section] of targets) {
            if (anchor.getBoundingClientRect().bottom <= inset) passed.add(section);
            else passed.delete(section);
          }
          selectHeading();
        },
        { rootMargin: `-${inset}px 0px 0px 0px`, threshold: 0 },
      );
      for (const [anchor, section] of targets) {
        intersection.observe(anchor);
        // A jump can skip a heading from below to above the viewport without
        // changing its intersection state. The section still enters/leaves.
        intersection.observe(section.element);
      }
    };
    const resize = new ResizeObserver(observeHeadings);
    resize.observe(page);
    resize.observe(toolbar);
    observeHeadings();
    return () => {
      resize.disconnect();
      intersection?.disconnect();
    };
  });
</script>

<div class="settings-page" bind:this={page}>
  <div class="settings-toolbar" bind:this={toolbar}>
    <PageControls
      {canSave}
      onSave={() => void saveAll()}
      saveButtonId="save-settings"
      saveLabel={t("Save Changes")}
      exportLabel={t("Export Config")}
      importLabel={t("Import Config")}
      onExport={() => void transfer.exportConfig()}
      onImport={(event) => void transfer.importConfig(event)}
      addLabel={visibleControls?.addLabel}
      canAdd={visibleControls?.canAdd ?? false}
      onAdd={visibleControls?.onAdd}
      contextual={sections.length > 1}
    >
      {#snippet search()}
        <h2 class="settings-title" aria-label={visibleControls?.title}>
          <span class="settings-title-viewport" aria-hidden="true">
            {#each sections as section, index (section)}
              <span class="title-layer" class:current={index === titleIndex}>
                {section.controls().title}
              </span>
            {/each}
          </span>
        </h2>
      {/snippet}
    </PageControls>
  </div>
  <div class="settings-sections">
    <ProfilesPanel />
    <PlatformSettings />
    <DnsQueryLog {active} />
  </div>
</div>

<style>
  .settings-page {
    min-width: 0;
  }
  .settings-toolbar {
    position: sticky;
    top: 0;
    z-index: 5;
    --page-controls-background: var(--bg-dark);
    --context-transition: 220ms cubic-bezier(0.2, 0, 0.2, 1);
  }
  .settings-sections {
    display: flex;
    flex-direction: column;
    gap: 2rem;
  }
  .settings-title {
    display: grid;
    align-items: center;
    margin: 0;
    min-height: var(--row-h);
    color: var(--text-2);
    font: 600 1.3rem / 1.4 var(--font);
  }
  .settings-title-viewport {
    display: grid;
    position: relative;
    overflow: hidden;
  }
  .title-layer {
    grid-area: 1 / 1;
    /* Preserve title width and fading without reserving hidden titles' height. */
    height: 0;
    align-self: start;
    pointer-events: none;
    opacity: 0;
    transition: opacity var(--context-transition);
  }
  .title-layer.current {
    height: auto;
    opacity: 1;
  }
  @media (prefers-reduced-motion: reduce) {
    .title-layer,
    .title-layer.current {
      transition: none;
    }
  }
</style>
