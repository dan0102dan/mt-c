<script lang="ts">
  import { getContext, onDestroy, untrack } from "svelte";

  import Button from "../../../components/ui/Button.svelte";
  import GenericDialog from "../../../components/ui/GenericDialog.svelte";
  import Placeholder from "../../../components/ui/Placeholder.svelte";
  import Select from "../../../components/ui/Select.svelte";
  import Switch from "../../../components/ui/Switch.svelte";
  import { fetchInterfaces, interfaces } from "../../../data/interfaces.svelte";
  import { t } from "../../../data/locale.svelte";
  import { GeneralSettingsState } from "./settings-state.svelte";

  import {
    CONFIG_TRANSFER_CONTEXT,
    type ConfigTransferControls,
  } from "../../config-transfer/context";
  import { settingsSection } from "../section-controls";
  import {
    changesWebAccess,
    fieldForPath,
    logLevels,
    UINT32_MAX,
    webTarget,
    webTargetUrl,
    type DraftField,
    type SwitchField,
    type TextField,
  } from "./config";

  type Props = { active: boolean; pageSaving?: boolean };
  let { active, pageSaving = false }: Props = $props();
  const section = settingsSection();
  const transfer = getContext<ConfigTransferControls | undefined>(CONFIG_TRANSFER_CONTEXT);
  const config = new GeneralSettingsState();
  let confirmation = $state<"apply" | "reload" | null>(null);
  let advancedOpen = $state(false);
  let resolveConfirmation: ((confirmed: boolean) => void) | undefined;
  const proposed = $derived(config.built.settings);
  const connectionChange = $derived(
    Boolean(config.snapshot && proposed && changesWebAccess(config.snapshot.settings, proposed)),
  );
  const blocked = $derived(connectionChange && Boolean(transfer?.connectionChangeBlocked()));
  const externalApplication = $derived(
    !config.pending && config.snapshot?.operation?.state === "applying",
  );
  const formDisabled = $derived(pageSaving || config.busy || externalApplication);
  const invalid = $derived(!proposed);
  const logLabels: Record<(typeof logLevels)[number], string> = {
    trace: "Trace",
    debug: "Debug",
    info: "Info",
    warn: "Warnings",
    error: "Errors",
    fatal: "Fatal errors",
    panic: "Panic",
    nolevel: "No level",
    disabled: "Disabled",
  };
  const logOptions = $derived(logLevels.map((value) => ({ value, label: t(logLabels[value]) })));
  const suggestedInterfaces = $derived([
    ...new Set([
      ...interfaces.list.map((item) => item.id),
      ...(config.snapshot?.settings.link ?? []),
    ]),
  ]);
  const proposedUrl = $derived(
    proposed ? webTargetUrl(webTarget(proposed), window.location.href) : undefined,
  );
  const nextUrl = $derived(
    config.nextWeb ? webTargetUrl(config.nextWeb, window.location.href) : undefined,
  );
  const addressChanges = $derived(
    Boolean(
      config.snapshot &&
      proposed &&
      (config.snapshot.settings.httpWeb.host.address !== proposed.httpWeb.host.address ||
        config.snapshot.settings.httpWeb.host.port !== proposed.httpWeb.host.port),
    ),
  );
  const authChanges = $derived(
    Boolean(
      config.snapshot &&
      proposed &&
      config.snapshot.settings.httpWeb.auth.enabled !== proposed.httpWeb.auth.enabled,
    ),
  );

  $effect(() => {
    if (active) untrack(() => void config.load());
  });
  $effect(() => {
    if (
      config.snapshot &&
      (config.built.errors.timeoutMs ||
        config.built.errors.maxConcurrent ||
        config.built.errors.maxIdleConns)
    )
      advancedOpen = true;
  });

  function answerConfirmation(confirmed: boolean) {
    const resolve = resolveConfirmation;
    resolveConfirmation = undefined;
    confirmation = null;
    resolve?.(confirmed);
  }

  async function confirmSave(): Promise<boolean> {
    if (!config.canSave || blocked) return false;
    confirmation = "apply";
    return new Promise((resolve) => {
      resolveConfirmation = resolve;
    });
  }

  async function save() {
    if (blocked)
      throw new Error("Save changes in Groups and Subscriptions before changing web access.");
    const previous = config.snapshot?.settings.showAllInterfaces;
    await config.save();
    if (previous !== config.snapshot?.settings.showAllInterfaces) void fetchInterfaces();
  }

  async function checkApplication() {
    const previous = config.snapshot?.settings.showAllInterfaces;
    await config.checkApplication();
    if (config.outcome === "succeeded" && previous !== config.snapshot?.settings.showAllInterfaces)
      void fetchInterfaces();
  }

  async function reload() {
    if (config.busy || pageSaving) return;
    if (config.dirty || config.pending) {
      confirmation = "reload";
      if (
        !(await new Promise<boolean>((resolve) => {
          resolveConfirmation = resolve;
        }))
      )
        return;
    }
    await config.load(true);
  }

  function fieldError(key: DraftField): string | undefined {
    return (
      config.built.errors[key] ??
      (config.error?.field && fieldForPath(config.error.field) === key
        ? config.error.message
        : undefined)
    );
  }

  function beforeUnload(event: BeforeUnloadEvent) {
    if (!config.dirty && !config.pending) return;
    event.preventDefault();
    event.returnValue = "";
  }

  onDestroy(() => {
    answerConfirmation(false);
    config.destroy();
  });
</script>

<svelte:window onbeforeunload={beforeUnload} />

{#snippet confirmationDialog()}
  <GenericDialog
    open={confirmation !== null}
    title={confirmation === "reload" ? t("Reload current settings?") : t("Apply general settings?")}
    maxWidth={480}
    on:close={() => answerConfirmation(false)}
    on:submit={() => answerConfirmation(true)}
    onOpenAutoFocus={() => document.getElementById("general-confirm-cancel")?.focus()}
  >
    <div slot="body" class="confirmation-body">
      {#if confirmation === "reload"}
        <p>
          {t(
            "Discard this draft and load the current settings? This will not cancel an application already accepted by the server.",
          )}
        </p>
      {:else}
        <p>{t("DNS routing may be temporarily unavailable while settings are being applied.")}</p>
        {#if proposed && !proposed.httpWeb.enabled}
          <p>
            {t(
              "The web interface will be disabled. To enable it again, you will need to change the settings on the device.",
            )}
          </p>
        {:else if addressChanges}
          <p>
            {t("The web address will change. Use the new address to reconnect after applying.")}
          </p>
          {#if proposedUrl}<p class="web-url">{proposedUrl}</p>{/if}
        {/if}
        {#if authChanges && proposed?.httpWeb.auth.enabled}
          <p>{t("You may need to sign in again. Your open drafts will be kept.")}</p>
        {:else if authChanges}
          <p>{t("Sign-in will no longer be required for the web interface.")}</p>
        {/if}
      {/if}
    </div>
    <div slot="actions" class="confirmation-actions">
      <Button id="general-confirm-cancel" type="button" onclick={() => answerConfirmation(false)}
        >{t("Cancel")}</Button
      >
      <Button type="button" class="accent" onclick={() => answerConfirmation(true)}>
        {confirmation === "reload" ? t("Reload settings") : t("Apply")}
      </Button>
    </div>
  </GenericDialog>
{/snippet}

{#snippet field(
  key: TextField,
  label: string,
  type: "text" | "number" = "text",
  min?: number,
  max?: number,
  hint?: string,
)}
  <div class="setting-field">
    <label for={`general-${key}`}>{t(label)}</label>
    <input
      id={`general-${key}`}
      {type}
      value={config.draft[key]}
      oninput={(event) => {
        config.draft[key] = event.currentTarget.value;
      }}
      {min}
      {max}
      step={type === "number" ? 1 : undefined}
      inputmode={type === "number" ? "numeric" : undefined}
      autocomplete="off"
      spellcheck="false"
      aria-invalid={Boolean(fieldError(key))}
      aria-describedby={fieldError(key)
        ? `general-${key}-error`
        : hint
          ? `general-${key}-hint`
          : undefined}
    />
    {#if fieldError(key)}
      <p id={`general-${key}-error`} class="field-error">{t(fieldError(key) ?? "")}</p>
    {:else if hint}
      <p id={`general-${key}-hint`} class="field-hint">{t(hint)}</p>
    {/if}
  </div>
{/snippet}

{#snippet toggle(key: SwitchField, label: string)}
  <div class="switch-row">
    <Switch
      id={`general-${key}`}
      aria-label={t(label)}
      bind:checked={
        () => config.draft[key],
        (value) => {
          config.draft[key] = value;
        }
      }
    />
    <label for={`general-${key}`}>{t(label)}</label>
  </div>
{/snippet}

{#snippet feedback()}
  {#if blocked}
    <p class="notice" role="status">
      {t("Save changes in Groups and Subscriptions before changing web access.")}
    </p>
  {/if}
  {#if config.error}
    <div class="application-error" role="alert">
      {#if config.error.code === "SETTINGS_CONFLICT"}
        <p>
          {t(
            "General settings changed elsewhere. Your draft is preserved. Reload current settings before saving again.",
          )}
        </p>
      {:else if config.error.code === "SETTINGS_BUSY"}
        <p>{t("Another settings application is in progress. Your draft is preserved.")}</p>
      {/if}
      {#if config.outcome === "rollback_failed"}
        <p>
          {t(
            "The service could not restore the previous settings. Check the device before trying again.",
          )}
        </p>
      {/if}
      <p>{t(config.error.message)}</p>
    </div>
  {/if}
  {#if (config.busy && !config.loading) || externalApplication}
    <p class="application-status" role="status">{t("Applying settings…")}</p>
  {:else if config.outcome === "succeeded" && !config.dirty}
    <p class="application-status" role="status">{t("Settings applied")}</p>
  {:else if config.dirty && !config.pending}
    <p class="application-status" role="status">{t("Unsaved changes")}</p>
  {/if}
  {#if config.nextWeb}
    <div class="connection-notice" role="status">
      {#if nextUrl}
        <p>{t("If the web address has changed, open the new address to check the result.")}</p>
        <a href={nextUrl} target="_blank" rel="noopener noreferrer"
          >{t("Open new web address")}: {nextUrl}</a
        >
      {:else}
        <p>
          {t(
            "If web access is disabled, this page cannot confirm the result. Your draft is preserved.",
          )}
        </p>
      {/if}
    </div>
  {/if}
  {#if config.pending || externalApplication}
    <Button small disabled={config.busy || pageSaving} onclick={() => void checkApplication()}
      >{t("Check application")}</Button
    >
  {/if}
{/snippet}

<section
  id="general"
  aria-labelledby="general-heading"
  aria-busy={config.busy}
  use:section={() => ({
    dirty: config.dirty,
    canSave: config.canSave && !blocked,
    saveLast: true,
    prepareSave: confirmSave,
    dialog: confirmationDialog,
    toolbarFeedback:
      active || config.pending || config.error || config.nextWeb || config.saving || config.checking
        ? feedback
        : undefined,
    validationMessage: blocked
      ? t("Save changes in Groups and Subscriptions before changing web access.")
      : config.pending
        ? t("Check the pending settings application before saving again.")
        : invalid && config.snapshot
          ? t("Check the highlighted settings.")
          : undefined,
    onSave: save,
  })}
>
  <h2 id="general-heading" class="sr-only">{t("General settings")}</h2>
  <div class="description-row">
    <p class="settings-description">
      {t("Configure DNS, network interfaces and access to the web interface.")}
    </p>
    {#if config.snapshot}<Button
        small
        disabled={config.busy || pageSaving}
        onclick={() => void reload()}>{t("Reload settings")}</Button
      >{/if}
  </div>
  {#if !config.snapshot}
    <Placeholder variant={config.error ? "error" : "loading"} minHeight="auto">
      {config.error ? t("Failed to load general settings") : t("Loading settings…")}
      {#snippet actions()}
        {#if config.error}<Button disabled={config.loading} onclick={() => void config.load()}
            >{t("Retry")}</Button
          >{/if}
      {/snippet}
    </Placeholder>
  {:else}
    <fieldset class="general-form" disabled={formDisabled}>
      <legend class="sr-only">{t("General settings")}</legend>
      <div class="settings-group">
        <h3>{t("DNS")}</h3>
        <div class="field-grid endpoint-grid">
          {@render field("upstreamAddress", "Upstream DNS address")}
          {@render field("upstreamPort", "Upstream DNS port", "number", 1, 65535)}
          {@render field("dnsAddress", "DNS listen address")}
          {@render field("dnsPort", "DNS listen port", "number", 1, 65535)}
        </div>
        <div class="switches">
          {@render toggle("interceptDns", "Intercept DNS on port 53")}
          {@render toggle("blockPtr", "Block reverse DNS queries (PTR)")}
          {@render toggle("filterAaaa", "Remove IPv6 addresses from DNS answers (AAAA)")}
        </div>
        <details class="advanced" bind:open={advancedOpen}>
          <summary>{t("DNS limits and timeout")}</summary>
          <div class="field-grid">
            {@render field("timeoutMs", "DNS response timeout (ms)", "number", 1, UINT32_MAX)}
            {@render field("maxConcurrent", "Concurrent DNS queries", "number", 1, UINT32_MAX)}
            {@render field("maxIdleConns", "Idle DNS connections", "number", 0, UINT32_MAX)}
          </div>
        </details>
      </div>

      <div class="settings-group">
        <h3>{t("Network interfaces")}</h3>
        <div class="setting-field interface-field">
          <label for="general-links">{t("Interfaces for DNS interception")}</label>
          <input
            id="general-links"
            type="text"
            bind:value={config.draft.links}
            list="general-interface-suggestions"
            autocomplete="off"
            spellcheck="false"
            placeholder={t("Example: br0, br-lan")}
            aria-invalid={Boolean(fieldError("links"))}
            aria-describedby="general-links-help"
          />
          <datalist id="general-interface-suggestions">
            {#each suggestedInterfaces as name}<option value={name}></option>{/each}
          </datalist>
          <p
            id="general-links-help"
            class:field-error={Boolean(fieldError("links"))}
            class:field-hint={!fieldError("links")}
          >
            {t(
              fieldError("links") ??
                "Enter interface names separated by commas. Names do not have to be in the suggestions.",
            )}
          </p>
        </div>
        <div class="switches">
          {@render toggle("ipv4", "Route IPv4 traffic")}
          {@render toggle("ipv6", "Route IPv6 traffic")}
          {@render toggle("showAllInterfaces", "Show all network interfaces")}
        </div>
        <div class="field-grid">
          {@render field(
            "additionalTTLSeconds",
            "Additional IP retention (seconds)",
            "number",
            0,
            UINT32_MAX,
            "Added to the DNS answer TTL when keeping IP addresses in routing rules.",
          )}
        </div>
      </div>

      <div class="settings-group">
        <h3>{t("Web access")}</h3>
        {@render toggle("webEnabled", "Enable web interface")}
        <fieldset class="web-fields" disabled={formDisabled || !config.draft.webEnabled}>
          <legend class="sr-only">{t("Web access options")}</legend>
          {@render toggle("authEnabled", "Require sign-in")}
          <div class="field-grid endpoint-grid">
            {@render field("webAddress", "Web listen address")}
            {@render field("webPort", "Web port", "number", 1, 65535)}
          </div>
        </fieldset>
      </div>

      <div class="settings-group">
        <h3>{t("Logging")}</h3>
        <div class="setting-field log-field">
          <span id="general-log-label">{t("Log level")}</span>
          <Select
            options={logOptions}
            bind:selected={config.draft.logLevel}
            ariaLabel={t("Log level")}
            disabled={formDisabled}
            compact
            bounded
          />
          {#if fieldError("logLevel")}<p class="field-error">
              {t(fieldError("logLevel") ?? "")}
            </p>{/if}
        </div>
      </div>
    </fieldset>
  {/if}
</section>

<style>
  fieldset {
    margin: 0;
    padding: 0;
    min-width: 0;
    border: 0;
  }
  .description-row {
    display: flex;
    align-items: start;
    gap: 1rem;
    justify-content: space-between;
  }
  .description-row .settings-description {
    flex: 1;
  }
  .general-form {
    display: grid;
    gap: 1.5rem;
  }
  .settings-group {
    min-width: 0;
    padding-bottom: 1.5rem;
    border-bottom: 1px solid var(--bg-light-extra);
  }
  .settings-group:last-child {
    border-bottom: 0;
    padding-bottom: 0;
  }
  h3 {
    margin: 0 0 0.85rem;
    font-size: 1rem;
    font-weight: 600;
  }
  .field-grid {
    display: grid;
    grid-template-columns: repeat(2, minmax(0, 1fr));
    gap: 1rem 1.5rem;
  }
  .endpoint-grid {
    grid-template-columns: minmax(0, 3fr) minmax(8rem, 1fr);
  }
  .setting-field {
    min-width: 0;
    display: grid;
    align-content: start;
    gap: 0.4rem;
  }
  .setting-field > label,
  .setting-field > span {
    color: var(--text-2);
    font-size: 0.9rem;
    line-height: 1.5;
  }
  input {
    box-sizing: border-box;
    width: 100%;
    min-width: 0;
    padding: 0.5rem 0.65rem;
    border: 1px solid var(--bg-light-extra);
    border-radius: 0.35rem;
    background: var(--bg-light);
    color: var(--text);
    font: 400 0.95rem/1.4 var(--font);
  }
  input[aria-invalid="true"] {
    border-color: var(--red);
  }
  input:disabled {
    opacity: 0.55;
  }
  input:focus-visible,
  summary:focus-visible,
  :global(#general [data-switch-root]:focus-visible) {
    outline: 2px solid var(--accent);
    outline-offset: 2px;
  }
  .switches {
    margin-top: 0.75rem;
  }
  .switch-row {
    display: flex;
    align-items: center;
    gap: 0.65rem;
    min-width: 0;
    padding: 0.45rem 0;
    line-height: 1.5;
  }
  .switch-row label {
    cursor: pointer;
  }
  .advanced {
    margin-top: 0.85rem;
  }
  summary {
    width: fit-content;
    color: var(--text-2);
    cursor: pointer;
    font-size: 0.9rem;
    line-height: 1.5;
  }
  .advanced .field-grid {
    margin-top: 1rem;
  }
  .interface-field {
    max-width: 100%;
  }
  .switches + .field-grid,
  .web-fields .field-grid {
    margin-top: 0.75rem;
  }
  .log-field {
    max-width: 22rem;
  }
  .log-field :global(.select-wrap),
  .log-field :global([data-select-trigger]) {
    width: 100%;
  }
  .log-field :global([data-select-trigger]) {
    box-sizing: border-box;
    padding: 0.5rem 0.65rem;
    background: var(--bg-light);
    border: 1px solid var(--bg-light-extra);
    border-radius: 0.35rem;
  }
  .field-error,
  .field-hint {
    margin: 0;
    font-size: 0.8rem;
    line-height: 1.5;
    overflow-wrap: anywhere;
  }
  .field-error,
  .application-error {
    color: var(--red);
  }
  .field-hint,
  .application-status {
    color: var(--text-2);
  }
  .application-status,
  .notice {
    margin: 0.5rem 0;
    font-size: 0.9rem;
    line-height: 1.5;
  }
  .application-error,
  .connection-notice {
    margin: 0.5rem 0;
    font-size: 0.875rem;
    line-height: 1.5;
    overflow-wrap: anywhere;
  }
  .application-error p,
  .connection-notice p {
    margin: 0.25rem 0;
  }
  .connection-notice a {
    color: var(--blue-light-extra);
  }
  .confirmation-body {
    line-height: 1.5;
    overflow-wrap: anywhere;
  }
  .confirmation-body p {
    margin: 0 0 0.75rem;
  }
  .confirmation-actions {
    display: flex;
    justify-content: flex-end;
    flex-wrap: wrap;
    gap: 0.5rem;
  }
  .web-url {
    color: var(--blue-light-extra);
  }
  .sr-only {
    position: absolute;
    width: 1px;
    height: 1px;
    padding: 0;
    overflow: hidden;
    clip-path: inset(50%);
    white-space: nowrap;
    border: 0;
  }
  @media (max-width: 700px) {
    .field-grid,
    .endpoint-grid {
      grid-template-columns: minmax(0, 1fr);
      gap: 0.85rem;
    }
    .description-row {
      flex-direction: column;
      gap: 0;
      margin-bottom: 1rem;
    }
    .description-row .settings-description {
      margin-bottom: 0.75rem;
    }
  }
</style>
