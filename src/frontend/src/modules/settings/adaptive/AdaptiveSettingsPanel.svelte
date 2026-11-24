<script lang="ts">
  import { onDestroy, tick, untrack } from "svelte";

  import Pagination from "../../../components/Pagination.svelte";
  import Button from "../../../components/ui/Button.svelte";
  import FormField from "../../../components/ui/FormField.svelte";
  import FormSection from "../../../components/ui/FormSection.svelte";
  import PanelSurface from "../../../components/ui/PanelSurface.svelte";
  import Placeholder from "../../../components/ui/Placeholder.svelte";
  import SectionDescription from "../../../components/ui/SectionDescription.svelte";
  import Select from "../../../components/ui/Select.svelte";
  import SwitchField from "../../../components/ui/SwitchField.svelte";
  import { t } from "../../../data/locale.svelte";
  import SettingsSection from "../SettingsSection.svelte";
  import { AdaptiveSettingsState } from "./state.svelte";

  import { Clear, Delete, Refresh } from "../../../components/ui/icons";
  import { toast } from "../../../utils/events";
  import type { AdaptiveField } from "./data";

  type Props = { active: boolean; pageSaving?: boolean };
  let { active, pageSaving = false }: Props = $props();
  const config = new AdaptiveSettingsState();
  let candidate = $state("");
  let page = $state(1);
  const pageSize = 50;
  const blocked = $derived(config.busy || pageSaving);
  const settings = $derived(config.built);
  const options = $derived(
    (config.snapshot?.interfaces ?? []).map((item) => ({
      value: item.id,
      label: item.id,
      description: item.name,
    })),
  );
  const ingressOptions = $derived(
    options
      .filter(
        (item) =>
          item.value !== config.draft.interface &&
          !config.draft.ingressInterfaces.includes(item.value),
      )
      .sort(
        (a, b) =>
          Number(["br-lan", "br0"].includes(b.value)) - Number(["br-lan", "br0"].includes(a.value)),
      ),
  );
  const entries = $derived(config.snapshot?.entries ?? []);
  const currentPage = $derived(Math.min(page, Math.max(1, Math.ceil(entries.length / pageSize))));
  const visibleEntries = $derived(
    entries.slice((currentPage - 1) * pageSize, currentPage * pageSize),
  );
  const runtimeLabel = $derived(
    config.failed
      ? t("Status unavailable")
      : config.snapshot?.active
        ? t("Active")
        : config.snapshot?.settings.enabled
          ? t("Activation unavailable")
          : t("Disabled"),
  );

  $effect(() => {
    if (active) untrack(() => void config.load());
  });
  onDestroy(() => config.destroy());

  async function addIngress(id: string) {
    if (
      !blocked &&
      config.draft.ingressInterfaces.length < 16 &&
      ingressOptions.some((item) => item.value === id)
    )
      config.draft.ingressInterfaces = [...config.draft.ingressInterfaces, id];
    await tick();
    candidate = "";
  }

  async function save() {
    try {
      await config.save();
    } catch (error) {
      if (config.persistencePending)
        toast.warning(t("Changes are applied but not saved to disk. Retry saving."));
      throw error;
    }
  }

  async function retryActivation() {
    try {
      await config.retryActivation();
    } catch {
      if (config.persistencePending)
        toast.warning(t("Changes are applied but not saved to disk. Retry saving."));
    }
  }

  function errorFor(field: AdaptiveField) {
    const error = settings.errors[field];
    return error ? t(error) : undefined;
  }

  function beforeUnload(event: BeforeUnloadEvent) {
    if (!config.dirty && !config.saving) return;
    event.preventDefault();
    event.returnValue = "";
  }
</script>

<svelte:window onbeforeunload={beforeUnload} />

{#snippet feedback()}
  {#if config.persistencePending}<p role="alert">
      {t("Changes are applied but not saved to disk. Retry saving.")}
    </p>{/if}
  {#if config.saveError}<p role="alert">{t(config.saveError)}</p>{/if}
  {#if config.snapshot?.error}<p role="alert">{config.snapshot.error}</p>{/if}
  {#if config.failed && config.snapshot}<p role="status">
      {t("Could not confirm the current adaptive routing status. Refresh it before saving.")}
    </p>{/if}
{/snippet}

<SettingsSection
  id="adaptive"
  title={t("Adaptive routing")}
  aria-busy={config.busy}
  data-active={config.failed ? "unknown" : (config.snapshot?.active ?? false)}
  showFeedback={Boolean(
    config.persistencePending ||
    config.saveError ||
    config.snapshot?.error ||
    (config.failed && config.snapshot),
  )}
  toolbarFeedback={feedback}
  controls={() => ({
    dirty: config.dirty,
    canSave: config.canSave,
    onSave: save,
    validationMessage:
      config.failed && config.snapshot
        ? t("Refresh adaptive routing before saving.")
        : !settings.settings && config.snapshot
          ? t("Check the highlighted settings.")
          : undefined,
  })}
>
  <SectionDescription>
    {t(
      "Try the default route first and learn an alternate route when a web connection fails. Applies to TCP ports 80 and 443; QUIC and other UDP traffic are unchanged.",
    )}
  </SectionDescription>
  {#if !config.snapshot}
    <Placeholder variant={config.failed ? "error" : "loading"} minHeight="auto">
      {config.failed ? t("Failed to load adaptive routing settings") : t("Loading...")}
      {#snippet actions()}
        {#if config.failed}<Button disabled={config.loading} onclick={() => void config.load(true)}
            >{t("Retry")}</Button
          >{/if}
      {/snippet}
    </Placeholder>
  {:else}
    <fieldset disabled={blocked}>
      <FormSection id="adaptive-routing-settings" title={t("Automatic connection routing")}>
        <div class="enable-row">
          <SwitchField
            id="adaptive-enabled"
            label={t("Enable adaptive routing")}
            bind:checked={config.draft.enabled}
            disabled={blocked}
          />
          <div class="runtime-controls">
            <span class="runtime-status" role="status">{runtimeLabel}</span>
            {#if config.snapshot.settings.enabled && !config.snapshot.active}
              <Button
                small
                disabled={blocked || !config.canRetryActivation}
                onclick={() => void retryActivation()}
              >
                {t("Retry activation")}
              </Button>
            {/if}
          </div>
        </div>
        <div class="fields">
          <FormField label={t("Alternate interface")} error={errorFor("interface")}>
            <Select
              {options}
              bounded
              triggerClass={`form-control${settings.errors.interface ? " invalid" : ""}`}
              bind:selected={config.draft.interface}
              ariaLabel={t("Alternate interface")}
              missingLabel={config.draft.interface || t("Select interface")}
              disabled={blocked}
            />
          </FormField>
          <FormField
            label={t("Client LAN interfaces")}
            error={errorFor("ingressInterfaces")}
            hint={t("Select the local networks whose clients should use automatic routing.")}
          >
            <div class="ingress-list">
              {#each config.draft.ingressInterfaces as id (id)}
                <div class="ingress-row">
                  <span>{id}</span>
                  <Button
                    small
                    disabled={blocked}
                    aria-label={`${t("Remove client interface")} ${id}`}
                    onclick={() => {
                      if (!blocked)
                        config.draft.ingressInterfaces = config.draft.ingressInterfaces.filter(
                          (item) => item !== id,
                        );
                    }}><Delete size={16} /></Button
                  >
                </div>
              {/each}
              <Select
                options={ingressOptions}
                bounded
                compact
                triggerClass={`form-control${settings.errors.ingressInterfaces ? " invalid" : ""}`}
                bind:selected={candidate}
                onValueChange={(value) => void addIngress(value)}
                ariaLabel={t("Add client LAN interface")}
                missingLabel={t("Add client LAN interface")}
                disabled={blocked ||
                  !ingressOptions.length ||
                  config.draft.ingressInterfaces.length >= 16}
              />
            </div>
          </FormField>
        </div>
        <div class="fields timing-fields">
          {#each [{ key: "connectTimeoutMs", label: "Connection timeout, ms" }, { key: "tlsTimeoutMs", label: "TLS response timeout, ms" }, { key: "ttlSeconds", label: "Learning lifetime, seconds" }] as field (field.key)}
            {@const key = field.key as "connectTimeoutMs" | "tlsTimeoutMs" | "ttlSeconds"}
            <FormField id={`adaptive-${key}`} label={t(field.label)} error={errorFor(key)}>
              <input
                id={`adaptive-${key}`}
                class="form-control"
                type="text"
                inputmode="numeric"
                autocomplete="off"
                bind:value={config.draft[key]}
                aria-invalid={Boolean(settings.errors[key])}
                aria-describedby={settings.errors[key] ? `adaptive-${key}-error` : undefined}
              />
            </FormField>
          {/each}
        </div>
      </FormSection>
    </fieldset>

    <div class="learned-routes">
      <PanelSurface>
        {#snippet header()}
          <div class="table-header">
            <h3>{t("Learned routes")} <span>({entries.length})</span></h3>
            <div class="table-actions">
              <Button
                small
                disabled={blocked}
                title={t("Refresh adaptive routing")}
                aria-label={t("Refresh adaptive routing")}
                onclick={() => void config.load(true)}><Refresh size={18} /></Button
              >
              <Button
                small
                disabled={blocked || !entries.length}
                title={t("Clear learned routes")}
                aria-label={t("Clear learned routes")}
                onclick={() => void config.clear()}><Clear size={18} /></Button
              >
            </div>
          </div>
        {/snippet}
        {#if entries.length}
          <div class="table-scroll">
            <table>
              <caption>{t("Remaining lifetime is shown at the last refresh.")}</caption>
              <thead
                ><tr
                  ><th scope="col">{t("Address")}</th><th scope="col">{t("Route")}</th><th
                    scope="col">{t("Expires in")}</th
                  ></tr
                ></thead
              >
              <tbody>
                {#each visibleEntries as entry (`${entry.family}/${entry.address}/${entry.port}`)}
                  <tr>
                    <td>{entry.family === 6 ? `[${entry.address}]` : entry.address}:{entry.port}</td
                    >
                    <td
                      >{entry.alternate
                        ? config.snapshot.settings.interface
                        : t("Default route")}</td
                    >
                    <td>{entry.expiresInSeconds} {t("sec")}</td>
                  </tr>
                {/each}
              </tbody>
            </table>
          </div>
          <Pagination
            totalItems={entries.length}
            {pageSize}
            {currentPage}
            onPageChange={(value) => (page = value)}
          />
        {:else}
          <Placeholder embedded variant="empty" minHeight="auto"
            >{t("No learned routes yet")}</Placeholder
          >
        {/if}
        {#snippet footer()}
          {#if config.snapshot}
            <span>{t("Active connections")}: {config.snapshot.stats.active}</span>
            <span> · {t("Alternate connections")}: {config.snapshot.stats.alternateSuccess}</span>
            {#if config.snapshot.stats.failures}<span>
                · {t("Failed connections")}: {config.snapshot.stats.failures}</span
              >{/if}
          {/if}
        {/snippet}
      </PanelSurface>
    </div>
  {/if}
</SettingsSection>

<style>
  fieldset {
    margin: 0;
    padding: 0;
    min-width: 0;
    border: 0;
  }
  .enable-row,
  .runtime-controls,
  .table-header,
  .ingress-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: var(--space-3);
    min-width: 0;
  }
  .enable-row {
    margin-bottom: var(--space-4);
    flex-wrap: wrap;
  }
  .runtime-controls {
    flex-wrap: wrap;
    justify-content: flex-end;
  }
  .runtime-status {
    color: var(--text-2);
    font-size: var(--text-small);
  }
  .fields {
    display: grid;
    grid-template-columns: repeat(2, minmax(0, 1fr));
    gap: var(--space-5);
    align-items: start;
  }
  .timing-fields {
    grid-template-columns: repeat(3, minmax(0, 1fr));
    margin-top: var(--space-5);
  }
  .fields :global(.select-wrap),
  .fields :global([data-select-trigger]) {
    width: 100%;
    max-width: 100%;
    min-width: 0;
  }
  .ingress-list {
    display: grid;
    gap: var(--space-2);
    min-width: 0;
  }
  .ingress-row {
    padding-left: var(--space-2);
    background: var(--bg-light);
    border-radius: var(--radius-panel);
  }
  .ingress-row span {
    overflow-wrap: anywhere;
    min-width: 0;
  }
  .learned-routes {
    margin-top: var(--space-6);
  }
  h3 {
    margin: 0;
    font: 600 var(--text-body) / var(--line-height-body) var(--font);
  }
  h3 span {
    font-weight: 400;
    color: var(--text-2);
  }
  .table-actions {
    display: flex;
    gap: var(--space-1);
    flex-shrink: 0;
  }
  .table-scroll {
    overflow-x: auto;
  }
  table {
    border-collapse: collapse;
    width: 100%;
    text-align: left;
    font: 400 var(--text-small) / var(--line-height-body) var(--font);
  }
  caption {
    text-align: left;
    color: var(--text-2);
    padding: var(--panel-inset);
    font-size: var(--text-caption);
  }
  th,
  td {
    padding: var(--space-2) var(--panel-inset);
    overflow-wrap: anywhere;
  }
  th {
    color: var(--text-2);
    font-weight: 400;
  }
  td:first-child {
    font-variant-numeric: tabular-nums;
  }
  td:last-child {
    white-space: nowrap;
  }
  tbody tr:nth-child(odd) {
    background: var(--bg-light);
  }
  @media (max-width: 700px) {
    .fields {
      grid-template-columns: minmax(0, 1fr);
    }
    .timing-fields {
      gap: var(--space-4);
    }
  }
</style>
