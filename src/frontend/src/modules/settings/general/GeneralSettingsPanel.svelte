<script lang="ts">
  import { Collapsible } from "bits-ui";
  import { getContext, onDestroy, tick, untrack } from "svelte";

  import Button from "../../../components/ui/Button.svelte";
  import GenericDialog from "../../../components/ui/GenericDialog.svelte";
  import Placeholder from "../../../components/ui/Placeholder.svelte";
  import Select from "../../../components/ui/Select.svelte";
  import Switch from "../../../components/ui/Switch.svelte";
  import { fetchInterfaces, interfaces } from "../../../data/interfaces.svelte";
  import { t } from "../../../data/locale.svelte";
  import { GeneralSettingsState } from "./settings-state.svelte";

  import { Add, GroupCollapse, GroupExpand } from "../../../components/ui/icons";
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
    type GeneralDraft,
    type SwitchField,
    type TextField,
  } from "./config";

  type Props = { active: boolean; pageSaving?: boolean };
  let { active, pageSaving = false }: Props = $props();
  const section = settingsSection();
  const transfer = getContext<ConfigTransferControls | undefined>(CONFIG_TRANSFER_CONTEXT);
  const config = new GeneralSettingsState();
  let confirmation = $state(false);
  let advancedOpen = $state(false);
  let interfaceCandidate = $state("");
  let submittedDraft = $state.raw<GeneralDraft>();
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
  const enteredInterfaces = $derived(
    config.draft.links
      .trim()
      .split(/[\s,]+/)
      .filter(Boolean),
  );
  const interfaceOptions = $derived(
    [
      ...new Set([
        ...interfaces.list.map((item) => item.id),
        ...(config.snapshot?.settings.link ?? []),
      ]),
    ]
      .filter((id) => id !== "blackhole" && !enteredInterfaces.includes(id))
      .map((id) => ({
        value: id,
        label: id,
        description: interfaces.list.find((item) => item.id === id)?.name,
      })),
  );

  async function addInterface(id: string) {
    if (!formDisabled && interfaceOptions.some((item) => item.value === id)) {
      config.draft.links = [...enteredInterfaces, id].join(", ");
    }
    await tick();
    interfaceCandidate = "";
  }
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
      (fieldError("timeoutMs") || fieldError("maxConcurrent") || fieldError("maxIdleConns"))
    )
      advancedOpen = true;
  });

  function answerConfirmation(confirmed: boolean) {
    const resolve = resolveConfirmation;
    resolveConfirmation = undefined;
    confirmation = false;
    resolve?.(confirmed);
  }

  async function confirmSave(): Promise<boolean> {
    if (!config.canSave || blocked) return false;
    confirmation = true;
    return new Promise((resolve) => {
      resolveConfirmation = resolve;
    });
  }

  async function save() {
    if (blocked)
      throw new Error("Save changes in Groups and Subscriptions before changing web access.");
    const previous = config.snapshot?.settings.showAllInterfaces;
    submittedDraft = { ...config.draft };
    await config.save();
    if (previous !== config.snapshot?.settings.showAllInterfaces) void fetchInterfaces();
  }

  async function checkApplication() {
    const previous = config.snapshot?.settings.showAllInterfaces;
    await config.checkApplication();
    if (config.outcome === "succeeded" && previous !== config.snapshot?.settings.showAllInterfaces)
      void fetchInterfaces();
  }

  function fieldError(key: DraftField): string | undefined {
    return (
      config.built.errors[key] ??
      (config.error?.field &&
      fieldForPath(config.error.field) === key &&
      submittedDraft?.[key] === config.draft[key]
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
    open={confirmation}
    title={t("Apply general settings?")}
    maxWidth={480}
    on:close={() => answerConfirmation(false)}
    on:submit={() => answerConfirmation(true)}
    onOpenAutoFocus={() => document.getElementById("general-confirm-cancel")?.focus()}
  >
    <div slot="body" class="confirmation-body">
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
    </div>
    <div slot="actions" class="confirmation-actions">
      <Button id="general-confirm-cancel" type="button" onclick={() => answerConfirmation(false)}
        >{t("Cancel")}</Button
      >
      <Button type="button" class="accent" onclick={() => answerConfirmation(true)}>
        {t("Apply")}
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
)}
  <div class="setting-field">
    <label for={`general-${key}`}>{t(label)}</label>
    <input
      id={`general-${key}`}
      {type}
      aria-label={key === "webAddress"
        ? t("Web listen address")
        : key === "webPort"
          ? t("Web port")
          : undefined}
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
      aria-describedby={fieldError(key) ? `general-${key}-error` : undefined}
    />
    {#if fieldError(key)}
      <p id={`general-${key}-error`} class="field-error">{t(fieldError(key) ?? "")}</p>
    {/if}
  </div>
{/snippet}

{#snippet toggle(key: SwitchField, label: string, ariaLabel: string = label)}
  <div class="switch-row">
    <Switch
      id={`general-${key}`}
      aria-label={t(ariaLabel)}
      disabled={formDisabled || (key === "authEnabled" && !config.draft.webEnabled)}
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
  {#if config.snapshot && config.error && (!config.error.field || !fieldForPath(config.error.field))}
    <div class="application-error" role="alert">
      {#if config.error.code === "SETTINGS_CONFLICT"}
        <p>
          {t(
            "General settings changed elsewhere. Your draft is preserved. Reload the page before saving again.",
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
          {@render field("upstreamAddress", "Upstream DNS server")}
          {@render field("upstreamPort", "Upstream server port", "number", 1, 65535)}
          {@render field("dnsAddress", "Local DNS address")}
          {@render field("dnsPort", "Local DNS port", "number", 1, 65535)}
        </div>
        <div class="switches">
          {@render toggle("interceptDns", "Intercept queries on port 53")}
          {@render toggle("blockPtr", "Block reverse lookups (PTR)")}
          {@render toggle("filterAaaa", "Filter IPv6 answers (AAAA)")}
        </div>
        <Collapsible.Root class="advanced" bind:open={advancedOpen}>
          <Collapsible.Trigger class="advanced-trigger" disabled={formDisabled}>
            {#if advancedOpen}<GroupCollapse size={18} />{:else}<GroupExpand size={18} />{/if}
            {t("Timeout and limits")}
          </Collapsible.Trigger>
          <Collapsible.Content>
            <div class="field-grid">
              {@render field("timeoutMs", "Response timeout (ms)", "number", 1, UINT32_MAX)}
              {@render field("maxConcurrent", "Concurrent query limit", "number", 1, UINT32_MAX)}
              {@render field("maxIdleConns", "Idle connection limit", "number", 0, UINT32_MAX)}
            </div>
          </Collapsible.Content>
        </Collapsible.Root>
      </div>

      <div class="settings-group">
        <h3>{t("Traffic routing")}</h3>
        <div class="setting-field interface-field">
          <label for="general-links">{t("DNS interception interfaces")}</label>
          <div class="interface-picker">
            <input
              id="general-links"
              type="text"
              bind:value={config.draft.links}
              autocomplete="off"
              spellcheck="false"
              placeholder={t("Example: br0, br-lan")}
              aria-invalid={Boolean(fieldError("links"))}
              aria-describedby={fieldError("links") ? "general-links-error" : undefined}
            />
            <Select
              options={interfaceOptions}
              bind:selected={interfaceCandidate}
              onValueChange={addInterface}
              ariaLabel={t("Add interface")}
              disabled={formDisabled || !interfaceOptions.length}
              bounded
            >
              {#snippet trigger()}<Add size={20} /><span class="add-interface-label"
                  >{t("Add interface")}</span
                >{/snippet}
            </Select>
          </div>
          {#if fieldError("links")}
            <p id="general-links-error" class="field-error">{t(fieldError("links") ?? "")}</p>
          {/if}
        </div>
        <div class="switches">
          {@render toggle("ipv4", "Process IPv4 traffic")}
          {@render toggle("ipv6", "Process IPv6 traffic")}
          {@render toggle("showAllInterfaces", "Show all interfaces in selectors")}
        </div>
        <div class="field-grid">
          {@render field(
            "additionalTTLSeconds",
            "Additional IP address TTL (seconds)",
            "number",
            0,
            UINT32_MAX,
          )}
        </div>
      </div>

      <div class="settings-group">
        <h3>{t("Web interface")}</h3>
        {@render toggle("webEnabled", "Enable", "Enable web interface")}
        <fieldset class="web-fields" disabled={formDisabled || !config.draft.webEnabled}>
          <legend class="sr-only">{t("Web access options")}</legend>
          {@render toggle("authEnabled", "Enable authentication")}
          <div class="field-grid endpoint-grid">
            {@render field("webAddress", "Address")}
            {@render field("webPort", "Port", "number", 1, 65535)}
          </div>
        </fieldset>
      </div>

      <div class="settings-group">
        <h3>{t("Logging")}</h3>
        <div class="setting-field log-field">
          <span>{t("Log detail level")}</span>
          <Select
            options={logOptions}
            bind:selected={config.draft.logLevel}
            ariaLabel={t("Log detail level")}
            triggerClass={fieldError("logLevel") ? "invalid" : undefined}
            disabled={formDisabled}
            compact
            bounded
          />
          {#if fieldError("logLevel")}<p class="field-error" role="alert">
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
    border-radius: 0.5rem;
    background: var(--bg-light);
    color: var(--text);
    font: 400 0.95rem/1.4 var(--font);
  }
  input[type="number"] {
    appearance: textfield;
  }
  input[type="number"]::-webkit-inner-spin-button,
  input[type="number"]::-webkit-outer-spin-button {
    -webkit-appearance: none;
    margin: 0;
  }
  input[aria-invalid="true"],
  .log-field :global([data-select-trigger].invalid) {
    border-color: var(--red);
  }
  input:disabled,
  .interface-picker :global([data-select-trigger]:disabled),
  .log-field :global([data-select-trigger]:disabled) {
    opacity: 0.55;
  }
  input:focus-visible,
  :global(#general .advanced-trigger:focus-visible),
  .interface-picker :global([data-select-trigger]:focus-visible),
  .log-field :global([data-select-trigger]:focus-visible),
  :global(#general [data-switch-root]:focus-visible) {
    outline: 1px solid var(--accent);
    outline-offset: 1px;
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
  :global(#general .advanced) {
    margin-top: 0.85rem;
  }
  :global(#general .advanced-trigger) {
    display: inline-flex;
    align-items: center;
    gap: 0.4rem;
    max-width: 100%;
    padding: 0;
    border: 0;
    background: transparent;
    color: var(--text-2);
    cursor: pointer;
    font: 400 0.9rem/1.5 var(--font);
    text-align: left;
  }
  :global(#general .advanced-trigger:disabled) {
    opacity: 0.55;
    cursor: default;
  }
  :global(#general .advanced) .field-grid {
    margin-top: 1rem;
  }
  .interface-picker {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    min-width: 0;
  }
  .interface-picker input {
    flex: 1;
  }
  .interface-picker :global([data-select-trigger]) {
    padding: 0.5rem;
    gap: 0.35rem;
    color: var(--text-2);
  }
  .add-interface-label {
    font-size: 0.85rem;
    white-space: nowrap;
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
    border-radius: 0.5rem;
  }
  .field-error {
    margin: 0;
    font-size: 0.8rem;
    line-height: 1.5;
    overflow-wrap: anywhere;
  }
  .field-error,
  .application-error {
    color: var(--red);
  }
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
    .add-interface-label {
      display: none;
    }
    .field-grid,
    .endpoint-grid {
      grid-template-columns: minmax(0, 1fr);
      gap: 0.85rem;
    }
  }
</style>
