<script lang="ts">
  import PlatformSettings from "#platform/settings";

  import ProfilesPanel from "./ProfilesPanel.svelte";
</script>

<!-- Sections are stacked; each section owns its editor and save state. -->
<div class="settings-page">
  <section class="settings-section" aria-labelledby="interface-priority-profiles">
    <ProfilesPanel />
  </section>
  <PlatformSettings />
</div>

<style>
  .settings-page {
    display: flex;
    flex-direction: column;
    gap: 2rem;
  }
</style>
