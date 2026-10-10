<script lang="ts">
  import { t } from "../../data/locale.svelte";
  import { checkUpdates, setUpdateChannel, updates } from "../../data/updates.svelte";
  import Button from "../../components/ui/Button.svelte";
  import Checkbox from "../../components/ui/Checkbox.svelte";
  import { Refresh } from "../../components/ui/icons";
  import { candidate, isActive } from "./releases";

  const choice = $derived(candidate(updates.release, updates.info));
  const busy = $derived(updates.checking || updates.submitting || updates.reconnecting || isActive(updates.status));
</script>

<div class="updates-panel">
  <h2 id="software-updates">{t("Software updates")}</h2>
  {#if updates.info}<p>{t("Installed version")}: <strong>{updates.info.installed_version}</strong></p>{/if}
  <label class="preview">
    <Checkbox checked={updates.preview} disabled={busy} ariaLabel={t("Include pre-releases")}
      on:change={(event) => setUpdateChannel(event.detail.checked)} />
    {t("Include pre-releases")}
  </label>
  <div class="actions">
    <Button type="button" disabled={busy} onclick={() => void checkUpdates(true)}>
      <Refresh size={16} /> {updates.checking ? t("Checking for updates") : t("Check for updates")}
    </Button>
    {#if choice.newer || isActive(updates.status) || updates.status.stage !== "idle"}
      <Button type="button" onclick={() => (updates.dialogOpen = true)}>
        {isActive(updates.status) ? t("Update progress") : choice.newer ? `${t("Update available")}: ${updates.release?.tag_name}` : t("Last update result")}
      </Button>
    {:else if updates.checkedAt && !updates.checking && !updates.error && !choice.reason}
      <span>{updates.release ? t("No newer release") : t("No published releases")}</span>
    {/if}
  </div>
  {#if choice.reason}<p>{t(choice.reason)}</p>{/if}
  {#if updates.error}<p role="status">{t(updates.error)}</p>{/if}
  {#if updates.checkedAt}<p class="checked">{t("Last checked")}: {new Date(updates.checkedAt).toLocaleString()}</p>{/if}
</div>

<style>
  h2 { margin: 0 0 1rem; font: 600 1.3rem var(--font); }
  .preview, .actions { display: flex; align-items: center; gap: 0.75rem; flex-wrap: wrap; }
  .actions { margin-top: 1rem; }
  p { margin: 0.75rem 0; }
  .checked { color: var(--text-2); font-size: 0.9rem; }
</style>
