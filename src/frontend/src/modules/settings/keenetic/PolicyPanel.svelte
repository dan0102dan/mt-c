<script lang="ts">
  import { getContext, onMount } from "svelte";

  import Button from "../../../components/ui/Button.svelte";
  import FormField from "../../../components/ui/FormField.svelte";
  import Placeholder from "../../../components/ui/Placeholder.svelte";
  import SectionDescription from "../../../components/ui/SectionDescription.svelte";
  import Select from "../../../components/ui/Select.svelte";
  import SwitchField from "../../../components/ui/SwitchField.svelte";
  import { locales, t } from "../../../data/locale.svelte";
  import {
    acceptPolicyImport,
    fetchPolicySettings,
    policy,
    policyDirty,
    refreshPolicies,
    savePolicySettings,
    selectedPolicyId,
    stagePolicyImport,
  } from "./state.svelte";

  import {
    CONFIG_TRANSFER_CONTEXT,
    type ConfigTransferControls,
  } from "../../config-transfer/context";
  import { settingsSection } from "../section-controls";
  import ru from "./ru.json";

  // This module and its translations are never imported by generic builds.
  Object.assign(locales.ru, ru);
  const section = settingsSection();
  const transfer = getContext<ConfigTransferControls | undefined>(CONFIG_TRANSFER_CONTEXT);
  const dirty = $derived(policyDirty());
  const selectedId = $derived(selectedPolicyId());
  const selected = $derived(policy.policies.find((p) => p.id === selectedId));
  const valid = $derived(
    !policy.draft.enabled || Boolean(selectedId && policy.listLoaded && !policy.listFailed),
  );
  const options = $derived([
    ...(policy.policies.some((p) => p.name === "mt" || p.id === "mt")
      ? []
      : [{ value: "mt", label: "mt" }]),
    ...policy.policies.map((p) => ({ value: p.id, label: p.name || p.id })),
  ]);

  onMount(() => {
    const unregister = transfer?.registerPolicy({
      get: () => (policy.loaded ? { ...policy.draft } : undefined),
      dirty: () => policy.loaded && policyDirty(),
      saving: () => policy.saving,
      valid: () => valid,
      stage: stagePolicyImport,
      accept: acceptPolicyImport,
      refresh: async () => {
        // The user can start a new draft while a whole-config request is in
        // flight. Keep it, and replace only a clean baseline with fresh data.
        await fetchPolicySettings(false, { fresh: true });
        if (policy.failed) throw new Error("Policy settings refresh failed");
      },
    });
    void fetchPolicySettings();
    if (!policy.listLoaded && !policy.listFailed) void refreshPolicies();
    const timer = setInterval(() => {
      if (
        policy.loaded &&
        !policy.saving &&
        (policy.runtime.state !== "disabled" || policy.draft.enabled)
      )
        void fetchPolicySettings(true);
    }, 3000);
    return () => {
      clearInterval(timer);
      unregister?.();
    };
  });
  async function save() {
    await savePolicySettings();
  }
</script>

<section
  id="keenetic"
  class="policy-settings"
  aria-labelledby="keenetic-policy-title"
  aria-busy={policy.saving}
  use:section={() => ({
    dirty,
    validationMessage:
      !valid && !policy.listing
        ? policy.listFailed
          ? t("Could not load policies. Reload the page to retry.")
          : undefined
        : undefined,
    canSave: dirty && policy.loaded && !policy.saving && valid,
    onSave: save,
  })}
>
  <h2 id="keenetic-policy-title" class="sr-only">{t("Keenetic")}</h2>
  <SectionDescription>
    {t("Choose which devices mt-c processes using a Keenetic router policy.")}
  </SectionDescription>

  {#if !policy.loaded}
    <Placeholder variant={policy.failed ? "error" : "loading"} minHeight="auto">
      {policy.failed ? t("Failed to load policy settings") : t("Loading...")}
      {#snippet actions()}
        {#if policy.failed}<Button
            disabled={policy.loading}
            onclick={() => void fetchPolicySettings()}>{t("Retry")}</Button
          >{/if}
      {/snippet}
    </Placeholder>
  {:else}
    <fieldset class="policy-layout" class:enabled={policy.draft.enabled} disabled={policy.saving}>
      <div class="policy-switches">
        <SwitchField
          id="keenetic-policy-enabled"
          bind:checked={policy.draft.enabled}
          label={t("Use policy based filter")}
        />
        {#if policy.draft.enabled}
          <SwitchField
            id="keenetic-policy-mode"
            bind:checked={
              () => policy.draft.mode === "exclude",
              (checked) => (policy.draft.mode = checked ? "exclude" : "include")
            }
            ariaLabel={t("Processing mode")}
          >
            {policy.draft.mode === "include"
              ? t("Include: MT processes only devices in the selected policy")
              : t("Exclude: MT processes all devices except those in the selected policy")}
          </SwitchField>
        {/if}
      </div>
      {#if policy.draft.enabled}
        <div class="policy-fields">
          <FormField
            label={t("Router policy")}
            hint={!policy.listing && (policy.listFailed || !selected?.available)
              ? policy.listFailed
                ? t("Could not load policies. Reload the page to retry.")
                : t("The filter will become active when the policy is available.")
              : undefined}
          >
            <Select
              {options}
              compact
              bounded
              triggerClass="form-control"
              bind:selected={() => selectedId, (id) => (policy.draft.policy_id = id)}
              ariaLabel={t("Router policy")}
              disabled={policy.listing || policy.saving}
              missingLabel={policy.draft.policy_id
                ? `${policy.draft.policy_id} · ${t("Policy unavailable")}`
                : "mt"}
            />
          </FormField>
        </div>
      {/if}
    </fieldset>
    {#if policy.persistencePending}
      <p class="notice" role="alert">
        {t("Changes are applied but not saved to disk. Retry saving.")}
      </p>
    {/if}
  {/if}
</section>

<style>
  fieldset {
    min-width: 0;
    border: 0;
    padding: 0;
    margin: 0;
  }
  .policy-layout {
    padding-inline: var(--panel-inset);
  }
  .policy-layout.enabled {
    display: grid;
    grid-template-columns: minmax(0, 3fr) minmax(0, 2fr);
    gap: var(--space-6);
    align-items: start;
  }
  .policy-switches,
  .policy-fields {
    min-width: 0;
  }
  .policy-fields {
    padding-block: var(--space-2);
  }
  .policy-fields :global(.select-wrap),
  .policy-fields :global([data-select-trigger]) {
    width: 100%;
    max-width: 100%;
    min-width: 0;
  }
  .notice {
    margin: var(--space-2) 0;
    padding-inline: var(--panel-inset);
    color: var(--text);
    font: 400 var(--text-small) / var(--line-height-body) var(--font);
  }
  .policy-settings :global(button:focus-visible) {
    outline: 2px solid var(--accent);
    outline-offset: 3px;
  }
  @media (max-width: 700px) {
    .policy-layout.enabled {
      grid-template-columns: minmax(0, 1fr);
      gap: var(--space-4);
    }
    .policy-fields {
      padding-block: 0;
    }
  }
</style>
