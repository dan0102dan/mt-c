<script lang="ts">
  import { untrack } from "svelte";

  import Button from "../../components/ui/Button.svelte";
  import Tooltip from "../../components/ui/Tooltip.svelte";
  import { locales, t } from "../../data/locale.svelte";
  import { applicationLogs as logs } from "./logs.svelte";
  import LogsPanel from "./LogsPanel.svelte";

  import { Pause, Play } from "../../components/ui/icons";
  import { settingsSection } from "../settings/section-controls";
  import translations from "./ru.json";

  let { active = false }: { active?: boolean } = $props();
  Object.assign(locales.ru, translations);
  const section = settingsSection();
  $effect(() => {
    if (active) untrack(() => logs.open());
  });
  const recording = $derived(logs.capture?.enabled ?? false);
  const action = $derived(recording ? t("Stop log recording") : t("Start log recording"));
  const unavailable = $derived(
    !logs.capture || ["idle", "reconnecting", "unavailable"].includes(logs.connection),
  );
  const status = $derived.by(() => {
    const messages: string[] = [];
    if (logs.connection === "reconnecting") messages.push(t("Connection lost"));
    else if (logs.connection === "unavailable") messages.push(t("Update the service to view logs"));
    else if (logs.connection === "connecting") messages.push(t("Loading log history..."));
    if (logs.capture && logs.capture.dropped !== "0")
      messages.push(`${t("Records not saved")}: ${logs.capture.dropped}`);
    return messages.join(" · ");
  });
  const statusLoading = $derived(logs.connection === "connecting");
  async function changeRecording() {
    try {
      await logs.setRecording(!recording);
    } catch (error) {
      console.error("Application log recording change failed", error);
    }
  }
</script>

<section
  id="logs"
  use:section={() => ({ dirty: false, canSave: false, onSave: async () => {} })}
  class="application-logs"
  aria-labelledby="application-logs-heading"
>
  <h2 id="application-logs-heading" class="sr-only">{t("Logs")}</h2>
  <LogsPanel
    entries={logs.entries}
    revision={logs.revision}
    onclear={() => logs.clear()}
    {status}
    {statusLoading}
    labels={{
      filter: t("Search application logs..."),
      export: t("Export application logs"),
      clear: t("Clear local logs"),
      scrollToBottom: t("Scroll to latest log"),
      results: t("Application log entries"),
      empty: t("No matching application logs"),
      session: t("Recording session"),
    }}
  >
    {#snippet controls()}
      <Tooltip
        value={recording
          ? t(
              "Stop recording for all viewers and free server history. Received logs stay in this browser.",
            )
          : t("Recording continues until stopped or the service restarts.")}
      >
        <Button
          type="button"
          onclick={() => void changeRecording()}
          aria-label={action}
          disabled={unavailable || logs.changing}
        >
          {#if recording}<Pause size={22} />{:else}<Play size={22} />{/if}
          <span class="recording-label">{action}</span>
        </Button>
      </Tooltip>
    {/snippet}
  </LogsPanel>
</section>

<style>
  .application-logs {
    min-width: 0;
  }
  .recording-label {
    margin-left: 0.35rem;
  }
</style>
