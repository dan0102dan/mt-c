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
  const controls = $derived(active?.controls());

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
    };
  });

  onMount(() => {
    let frame = 0;
    const update = () => {
      frame = 0;
      if (!page.getClientRects().length || !sections.length) return;
      if (window.scrollY <= 0) {
        active = sections[0];
        return;
      }
      const line = toolbar.getBoundingClientRect().bottom + 12;
      let next = sections[0];
      for (const section of sections) {
        if (section.element.getBoundingClientRect().top <= line) next = section;
      }
      // The final editor may be shorter than a screen. It still becomes
      // active at the bottom even when it cannot reach the sticky header.
      if (page.getBoundingClientRect().bottom <= window.innerHeight + 2) {
        next = sections.at(-1)!;
      }
      active = next;
    };
    const schedule = () => {
      if (!frame) frame = requestAnimationFrame(update);
    };
    window.addEventListener("scroll", schedule, { passive: true });
    window.addEventListener("resize", schedule);
    if (window.scrollY > 0) schedule();
    return () => {
      cancelAnimationFrame(frame);
      window.removeEventListener("scroll", schedule);
      window.removeEventListener("resize", schedule);
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
      addLabel={controls?.addLabel}
      canAdd={controls?.canAdd ?? false}
      onAdd={controls?.onAdd}
      contextual={sections.length > 1}
    >
      {#snippet search()}
        <h2 class="settings-title">
          {#each sections as section (section)}
            <span class:current={section === active} aria-hidden={section !== active}>
              {section.controls().title}
            </span>
          {/each}
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
    font: 600 1.3rem var(--font);
  }
  .settings-title span {
    grid-area: 1 / 1;
    opacity: 0;
    transform: translateY(-6px);
    transition:
      opacity 180ms ease,
      transform 220ms cubic-bezier(0.2, 0, 0.2, 1);
    pointer-events: none;
  }
  .settings-title .current {
    opacity: 1;
    transform: translateY(0);
  }
  @media (prefers-reduced-motion: reduce) {
    .settings-title span {
      transition: none;
      transform: none;
    }
  }
</style>
