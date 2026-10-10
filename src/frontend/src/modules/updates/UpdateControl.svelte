<script lang="ts">
  import { onMount } from "svelte";

  import { t } from "../../data/locale.svelte";
  import { installUpdate, mountUpdates, updates } from "../../data/updates.svelte";
  import Button from "../../components/ui/Button.svelte";
  import GenericDialog from "../../components/ui/GenericDialog.svelte";
  import Tooltip from "../../components/ui/Tooltip.svelte";
  import { Export, Refresh } from "../../components/ui/icons";
  import { candidate, isActive, RELEASE_WEB } from "./releases";

  onMount(mountUpdates);
  const choice = $derived(candidate(updates.release, updates.info));
  const busy = $derived(isActive(updates.status) || updates.submitting || updates.reconnecting);
  const statusText = $derived({
    idle: "Ready to update", queued: "Preparing update", checking: "Verifying GitHub release",
    downloading: "Downloading package", verifying: "Verifying package checksum", backing_up: "Backing up settings",
    installing: "Installing package", restarting: "Waiting for service restart", succeeded: "Update installed",
    failed: "Update failed", interrupted: "Update interrupted",
  }[updates.status.stage]);
</script>

{#if choice.newer || busy || ["failed", "interrupted"].includes(updates.status.stage)}
  <Tooltip value={busy ? t("Updating") : t("Software update")}>
    <Button small type="button" onclick={() => (updates.dialogOpen = true)}
      aria-label={busy ? t("Updating") : `${t("Update available")}: ${updates.release?.tag_name ?? ""}`}>
      <span class="update-button">
        {#if busy}<Refresh size={16} />{:else}<Export size={16} />{/if}
        <span class="update-label">{busy ? t("Updating") : t("Update available")}</span>
      </span>
    </Button>
  </Tooltip>
{/if}

<GenericDialog open={updates.dialogOpen} title={t("Software update")} maxWidth={560}
  on:close={() => (updates.dialogOpen = false)}>
  <div slot="body" class="update-body">
    {#if updates.info}
      <p>{t("Installed version")}: <strong>{updates.info.installed_version}</strong></p>
    {/if}
    {#if updates.release}
      <p>{t("Available release")}: <strong>{updates.release.tag_name}</strong>
        {#if updates.release.prerelease}<span> · {t("Pre-release")}</span>{/if}
      </p>
      <a href={`${RELEASE_WEB}/tag/${encodeURIComponent(updates.release.tag_name)}`} target="_blank" rel="noopener noreferrer">{t("Release notes on GitHub")}</a>
      {#if updates.release.body}<pre>{updates.release.body}</pre>{/if}
    {/if}
    {#if updates.status.stage !== "idle" || updates.reconnecting}
      <p role="status" aria-live="polite">{updates.reconnecting ? t("Reconnecting to router; update status is not yet known") : t(statusText)}</p>
    {/if}
    {#if updates.status.error}<p role="alert">{t(updates.status.error)}</p>{/if}
    {#if updates.error}<p role="status">{t(updates.error)}</p>{/if}
    {#if updates.status.backup_path}<p class="backup">{t("Settings backup")}: <code>{updates.status.backup_path}</code></p>{/if}
    {#if !busy && (updates.status.stage !== "succeeded" || choice.newer)}
      {#if choice.reason}<p>{t(choice.reason)}</p>{/if}
      <p>{t("Save your edits first. The service will restart; unsaved browser edits are not included in the backup.")}</p>
      <p>{t("Do not power off the router during installation.")}</p>
    {/if}
    {#if busy}<p>{t("You may close this dialog. The update continues on the router.")}</p>{/if}
  </div>
  <div slot="actions" class="footer">
    <Button type="button" onclick={() => (updates.dialogOpen = false)}>{t("Close")}</Button>
    {#if updates.status.stage === "succeeded" && !choice.newer}
      <Button type="button" general onclick={() => window.location.reload()}>{t("Reload interface")}</Button>
    {:else}
      <Button type="button" general disabled={busy || !choice.asset || !!choice.reason}
        onclick={() => void installUpdate()}>{t("Install update")}</Button>
    {/if}
  </div>
</GenericDialog>

<style>
  .update-button { display: inline-flex; align-items: center; gap: 0.35rem; }
  .update-body { min-width: 0; }
  p { margin: 0.75rem 0; }
  pre { max-height: 25vh; overflow: auto; white-space: pre-wrap; overflow-wrap: anywhere; font: inherit; color: var(--text-2); }
  .backup { overflow-wrap: anywhere; }
  a { color: var(--blue-light-extra); }
  @media (max-width: 700px) { .update-label { display: none; } }
</style>
