<script lang="ts">
  import PlatformSettings from "#platform/settings";
  import { onMount, setContext } from "svelte";

  import PageControls from "../../components/layout/PageControls.svelte";
  import { t } from "../../data/locale.svelte";
  import ProfilesPanel from "./ProfilesPanel.svelte";

  import {
    SETTINGS_SECTIONS,
    type RegisterSection,
    type SettingsSection,
  } from "./section-controls";

  let page: HTMLDivElement;
  let toolbar: HTMLDivElement;
  let sections = $state.raw<SettingsSection[]>([]);
  let active = $state.raw<SettingsSection>();
  let titleIndex = $state(0);
  const visibleSection = $derived(sections[titleIndex]);
  const controls = $derived(active?.controls());
  const visibleControls = $derived(visibleSection?.controls());

  function add() {
    active = visibleSection;
    visibleControls?.onAdd?.();
  }

  setContext<RegisterSection>(SETTINGS_SECTIONS, (section) => {
    sections = [...sections, section];
    active ??= section;
    // A short page may show both editors without enough room to scroll.
    // Pointer/keyboard interaction chooses the editor without saving it.
    const activate = () => {
      active = section;
    };
    section.element.addEventListener("pointerdown", activate);
    section.element.addEventListener("focusin", activate);
    return () => {
      section.element.removeEventListener("pointerdown", activate);
      section.element.removeEventListener("focusin", activate);
      sections = sections.filter((s) => s !== section);
      if (active === section) active = sections[0];
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
        active = sections[next];
      }
    };
    // Geometry is measured only when layout changes, never on scroll.
    // CSS sticky owns the toolbar; the observer only switches its title and actions.
    const observeHeadings = () => {
      if (!page.getClientRects().length) return;
      intersection?.disconnect();
      const inset = toolbar.getBoundingClientRect().height;
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
        (entries) => {
          if (!page.getClientRects().length) return;
          for (const entry of entries) {
            const section = targets.get(entry.target)!;
            if (!entry.isIntersecting && entry.boundingClientRect.bottom <= inset)
              passed.add(section);
            else passed.delete(section);
          }
          selectHeading();
        },
        { rootMargin: `-${inset}px 0px 0px 0px`, threshold: 0 },
      );
      for (const anchor of targets.keys()) intersection.observe(anchor);
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
      canSave={controls?.canSave ?? false}
      onSave={() => controls?.onSave()}
      saveButtonId="save-settings"
      saveLabel={controls?.saveLabel ?? t("Save Changes")}
      addLabel={visibleControls?.addLabel}
      canAdd={visibleControls?.canAdd ?? false}
      onAdd={visibleControls?.onAdd ? add : undefined}
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
    pointer-events: none;
    opacity: 0;
    transition: opacity var(--context-transition);
  }
  .title-layer.current {
    opacity: 1;
  }
  @media (prefers-reduced-motion: reduce) {
    .title-layer,
    .title-layer.current {
      transition: none;
    }
  }
</style>
