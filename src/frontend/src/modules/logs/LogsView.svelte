<script lang="ts">
  import { onDestroy, untrack } from "svelte";

  import { locales, t } from "../../data/locale.svelte";
  import { applicationLogs as logs } from "./logs.svelte";
  import LogsPanel from "./LogsPanel.svelte";

  import { settingsSection } from "../settings/section-controls";
  import { logExportParts } from "./log-view";
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
  let export_url: string | undefined;
  let export_timer: ReturnType<typeof setTimeout> | undefined;
  function releaseExport() {
    clearTimeout(export_timer);
    if (export_url) URL.revokeObjectURL(export_url);
    export_url = undefined;
  }
  function saveLogs() {
    if (!logs.count) return;
    releaseExport();
    // All received records, not just the filter; works with a dead backend.
    export_url = URL.createObjectURL(
      new Blob(logExportParts(logs.entries), { type: "text/plain" }),
    );
    const link = document.createElement("a");
    link.href = export_url;
    link.download = `${new Date().toISOString().replace(/[:.]/g, "-")}-mt-c.log`;
    document.body.appendChild(link);
    try {
      link.click();
    } finally {
      link.remove();
    }
    export_timer = setTimeout(releaseExport, 30000);
  }
  onDestroy(releaseExport);
  async function changeRecording() {
    try {
      await logs.setRecording(!recording);
    } catch {
      // The request helper reports failures; the store reconciles capture state.
    }
  }
</script>

<section
  id="logs"
  use:section={() => ({
    dirty: false,
    canSave: false,
    onSave: async () => {},
    hideImport: true,
    exportAction: {
      label: t("Export application logs"),
      disabled: !logs.count,
      onclick: saveLogs,
    },
    contextAction: {
      label: action,
      icon: recording ? "stop" : "record",
      pressed: recording,
      disabled: unavailable || logs.changing,
      onclick: () => void changeRecording(),
    },
  })}
  class="application-logs"
  aria-labelledby="application-logs-heading"
>
  <h2 id="application-logs-heading" class="sr-only">{t("Logs")}</h2>
  <LogsPanel
    entries={logs.entries}
    revision={logs.revision}
    onclear={() => logs.clear()}
    labels={{
      filter: t("Search application logs..."),
      clear: t("Clear local logs"),
      scrollToBottom: t("Scroll to latest log"),
      results: t("Application log entries"),
      empty: t("No matching application logs"),
      session: t("Recording session"),
    }}
  />
</section>

<style>
  .application-logs {
    min-width: 0;
  }
</style>
