<script lang="ts">
  import { untrack } from "svelte";
  import Button from "../../components/ui/Button.svelte";
  import { Pause, Play } from "../../components/ui/icons";
  import Tooltip from "../../components/ui/Tooltip.svelte";
  import { locales, t } from "../../data/locale.svelte";
  import { settingsSection } from "../settings/section-controls";
  import LogsPanel from "./LogsPanel.svelte";
  import { applicationLogs as logs } from "./logs.svelte";
  import translations from "./ru.json";

  let { active = false }: { active?: boolean } = $props();
  Object.assign(locales.ru, translations);
  const section = settingsSection();
  $effect(() => { if (active) untrack(() => logs.open()); });
  const recording = $derived(logs.capture?.enabled ?? false);
  const action = $derived(recording ? t("Stop log recording") : t("Start log recording"));
  const unavailable = $derived(
    !logs.capture || ["idle", "reconnecting", "unavailable"].includes(logs.connection),
  );
  const status = $derived(
    logs.connection === "reconnecting"
      ? t("Connection lost. Received logs remain available.")
      : logs.connection === "unavailable"
        ? t("Application logs are unavailable in this version.")
        : logs.connection === "connecting"
          ? t("Loading log history...")
          : recording
            ? t("Recording application logs")
            : t("Application log recording is off"),
  );
  async function changeRecording() {
    try { await logs.setRecording(!recording); }
    catch (error) { console.error("Application log recording change failed", error); }
  }
</script>

<section id="logs" use:section={() => ({ dirty: false, canSave: false, onSave: async () => {} })}
  class="application-logs" aria-labelledby="application-logs-heading">
  <h2 id="application-logs-heading" class="sr-only">{t("Logs")}</h2>
  <p class="settings-description">
    {t("Recording starts only on request and uses RAM. Messages from before Start are not collected.")}
  </p>
  <LogsPanel
    entries={logs.entries}
    revision={logs.revision}
    onclear={() => logs.clear()}
    status={`${status} · ${t("Received records")}: ${logs.count}${logs.capture && logs.capture.dropped !== "0" ? ` · ${t("Records not saved")}: ${logs.capture.dropped}` : ""}`}
    labels={{
      filter: t("Search application logs..."), level: t("Minimum log level"),
      save: t("Save application logs"), clear: t("Clear local logs"),
      scrollToBottom: t("Scroll to latest log"), results: t("Application log entries"),
      empty: t("No matching application logs"), session: t("Recording session"),
    }}
  >
    {#snippet controls()}
      <Tooltip value={recording
        ? t("Stop recording for all viewers and free server history. Received logs stay in this browser.")
        : t("Recording continues until stopped or the service restarts.")}>
        <Button type="button" onclick={() => void changeRecording()}
          aria-label={action} disabled={unavailable || logs.changing}>
          {#if recording}<Pause size={22} />{:else}<Play size={22} />{/if}
          <span class="recording-label">{action}</span>
        </Button>
      </Tooltip>
    {/snippet}
  </LogsPanel>
</section>

<style>
  .application-logs { min-width: 0; }
  .recording-label { margin-left: 0.35rem; }
</style>
