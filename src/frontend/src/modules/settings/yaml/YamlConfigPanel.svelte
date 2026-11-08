<script lang="ts">
  import { getContext, untrack } from "svelte";

  import Placeholder from "../../../components/ui/Placeholder.svelte";
  import { t } from "../../../data/locale.svelte";
  import { YamlConfigState } from "./yaml-config.svelte";

  import {
    CONFIG_TRANSFER_CONTEXT,
    type ConfigTransferControls,
  } from "../../config-transfer/context";
  import { settingsSection } from "../section-controls";

  let { active }: { active: boolean } = $props();
  const section = settingsSection();
  const transfer = getContext<ConfigTransferControls>(CONFIG_TRANSFER_CONTEXT);
  const config = new YamlConfigState();
  const blocked = $derived(transfer.rawConfigBlocked());
  let editorModule = $state.raw<Promise<typeof import("./YamlEditor.svelte")>>();
  let refreshWarning = $state(false);

  $effect(() => {
    if (!active) return;
    // Keep the module and editor mounted after first activation, including
    // its undo history. Raw configuration never goes into browser storage.
    untrack(() => {
      editorModule ??= import("./YamlEditor.svelte");
      void config.load();
    });
  });

  async function save() {
    if (blocked || config.loading || config.saving) return;
    const allowOutOfSync = config.snapshot?.out_of_sync ?? false;
    if (
      allowOutOfSync &&
      !window.confirm(
        t(
          "The file differs from the running configuration. Saving this YAML will replace the running configuration and may discard changes that have not been written to the file. Continue?",
        ),
      )
    ) {
      throw new Error("YAML save cancelled");
    }
    await config.save(allowOutOfSync);
    refreshWarning = false;
    try {
      await transfer.refreshAfterYaml();
    } catch {
      refreshWarning = true;
    }
  }

  function beforeUnload(event: BeforeUnloadEvent) {
    if (!config.dirty) return;
    event.preventDefault();
    event.returnValue = "";
  }
</script>

<svelte:window onbeforeunload={beforeUnload} />

{#snippet feedback()}
  {#if config.error}
    <div class="yaml-error" role="alert">
      {#if config.error.code === "CONFIG_CONFLICT"}
        <p>
          {t(
            "Configuration changed since it was loaded. Your draft is preserved. Reload the page to get the current configuration.",
          )}
        </p>
      {:else if config.error.code === "CONFIG_OUT_OF_SYNC"}
        <p>
          {t(
            "The file differs from the running configuration. Reload the page before trying to save again.",
          )}
        </p>
      {/if}
      <p>{t(config.error.message)}</p>
      {#if config.error.line}
        <p>
          {t("Line")}: {config.error.line}{#if config.error.column}, {t("Column")}: {config.error
              .column}{/if}
        </p>
      {/if}
    </div>
  {/if}
{/snippet}

<section
  id="yaml"
  aria-labelledby="yaml-heading"
  use:section={() => ({
    exclusive: true,
    toolbarFeedback: feedback,
    dirty: config.dirty,
    canSave:
      config.dirty &&
      !config.loading &&
      !config.saving &&
      !blocked &&
      !config.validationErrors.length,
    validationMessage: blocked
      ? t("Save changes in other sections before saving the YAML configuration.")
      : config.validationErrors.length
        ? t("Fix YAML errors before saving.")
        : undefined,
    onSave: save,
  })}
>
  <h2 id="yaml-heading" class="sr-only">{t("YAML configuration")}</h2>
  {#if blocked}
    <p class="yaml-notice" role="status">
      {t("Save changes in other sections before saving the YAML configuration.")}
    </p>
  {/if}
  {#if config.snapshot?.out_of_sync}
    <p class="yaml-notice" role="status">
      {t("The file differs from the running configuration. Saving it will require confirmation.")}
    </p>
  {/if}
  {#if config.snapshot?.restart_required}
    <p class="yaml-notice" role="status">
      {t("Configuration saved. Restart the service to apply startup settings.")}
    </p>
  {/if}
  {#if refreshWarning}
    <p class="yaml-notice" role="status">
      {t(
        "YAML was saved, but some other sections could not be refreshed. Reload the page to update them.",
      )}
    </p>
  {/if}
  {#if config.snapshot && editorModule}
    {#await editorModule}
      <Placeholder loading>{t("Loading editor…")}</Placeholder>
    {:then module}
      <module.default
        bind:value={config.draft}
        errors={config.validationErrors}
        disabled={config.loading || config.saving}
      />
    {:catch}
      <Placeholder variant="error"
        >{t("Could not load the editor. Reload the page to try again.")}</Placeholder
      >
    {/await}
  {:else if config.loading}
    <Placeholder loading>{t("Loading configuration…")}</Placeholder>
  {/if}
</section>

<style>
  .yaml-notice {
    padding: 0.75rem;
    border: 1px solid var(--bg-light-extra);
    border-radius: 0.5rem;
    font-size: 0.9rem;
    line-height: 1.5;
    overflow-wrap: anywhere;
  }
  .yaml-notice {
    background: var(--bg-light);
    color: var(--text);
  }
  .yaml-error {
    color: var(--red);
    font-size: 0.875rem;
    line-height: 1.5;
    overflow-wrap: anywhere;
  }
  .yaml-error p {
    margin: 0;
  }
  .yaml-error p + p {
    margin-top: 0.15rem;
  }
</style>
