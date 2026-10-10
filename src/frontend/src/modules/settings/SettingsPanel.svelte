<script lang="ts">
  import UpdatesPanel from "../updates/UpdatesPanel.svelte";
  import ProfilesPanel from "./ProfilesPanel.svelte";
</script>

<!-- Sections are stacked; each section owns its editor and save state. -->
<div class="settings-page">
  <section class="settings-section" aria-labelledby="interface-priority-profiles">
    <ProfilesPanel />
  </section>
  <section class="settings-section" aria-labelledby="software-updates">
    <UpdatesPanel />
  </section>
</div>

<style>
  .settings-page {
    display: flex;
    flex-direction: column;
    gap: 2rem;
  }
</style>
