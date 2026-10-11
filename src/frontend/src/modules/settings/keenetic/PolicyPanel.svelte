<script lang="ts">
  import { onMount } from "svelte";

  import Button from "../../../components/ui/Button.svelte";
  import Placeholder from "../../../components/ui/Placeholder.svelte";
  import Select from "../../../components/ui/Select.svelte";
  import Switch from "../../../components/ui/Switch.svelte";
  import { locales, t } from "../../../data/locale.svelte";
  import {
    fetchPolicySettings,
    policy,
    policyDirty,
    refreshPolicies,
    savePolicySettings,
  } from "./state.svelte";

  import { toast } from "../../../utils/events";
  import { settingsSection } from "../section-controls";
  import ru from "./ru.json";

  // This module and its translations are never imported by generic builds.
  Object.assign(locales.ru, ru);
  const section = settingsSection();
  const dirty = $derived(policyDirty());
  const selected = $derived(policy.policies.find((p) => p.id === policy.draft.policy_id));
  const valid = $derived(
    !policy.draft.enabled ||
      Boolean(selected?.available && policy.listLoaded && !policy.listFailed),
  );
  const options = $derived(
    policy.policies.map((p) => ({
      value: p.id,
      label: p.name || p.id,
      description: `${p.id}${p.available ? "" : ` · ${t("Policy unavailable")}`}`,
    })),
  );
  const status = $derived(
    {
      disabled: "Policy filter is off. MT works for all devices as before.",
      ready: "Policy filter is active.",
      applying: "Applying policy filter...",
      unavailable:
        "The policy is unavailable. MT does not process new traffic until it becomes available or the filter is disabled.",
      error: "Could not apply the policy filter. Check the system log and retry or disable it.",
    }[policy.runtime.state],
  );

  onMount(() => {
    void fetchPolicySettings();
    const timer = setInterval(() => {
      if (
        policy.loaded &&
        !policy.saving &&
        (policy.runtime.state !== "disabled" || policy.draft.enabled)
      )
        void fetchPolicySettings(true);
    }, 3000);
    return () => clearInterval(timer);
  });
  $effect(() => {
    if (
      policy.loaded &&
      policy.draft.enabled &&
      !policy.listLoaded &&
      !policy.listing &&
      !policy.listFailed
    )
      void refreshPolicies();
  });
  async function save() {
    try {
      await savePolicySettings();
      toast.success(t("Policy settings saved"));
    } catch {
      /* Shared fetcher reports failures; disk retry also stays visible below. */
    }
  }
</script>

<section
  class="policy-settings"
  aria-labelledby="keenetic-policy-title"
  aria-busy={policy.saving}
  use:section={() => ({
    title: t("Keenetic policy"),
    canSave: dirty && policy.loaded && !policy.saving && valid,
    onSave: () => void save(),
    saveLabel: policy.saving ? t("Saving...") : t("Save Changes"),
  })}
>
  <h2 id="keenetic-policy-title">{t("Keenetic policy")}</h2>
  <p class="description">
    {t(
      "MT rules still choose destinations and interfaces. This setting only selects which devices they apply to.",
    )}
  </p>

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
    <fieldset disabled={policy.saving}>
      <div class="switch-row">
        <label for="keenetic-policy-enabled">{t("Use Keenetic policy")}</label>
        <Switch
          id="keenetic-policy-enabled"
          bind:checked={policy.draft.enabled}
          aria-label={t("Use Keenetic policy")}
        />
      </div>
      {#if policy.draft.enabled}
        <div class="policy-choice">
          <span id="keenetic-policy-label">{t("Router policy")}</span>
          <div class="picker-row">
            <Select
              {options}
              bind:selected={policy.draft.policy_id}
              ariaLabel={t("Router policy")}
              disabled={policy.listing || policy.saving}
              missingLabel={policy.draft.policy_id
                ? `${policy.draft.policy_id} · ${t("Policy unavailable")}`
                : t("Select a policy")}
            />
            <Button
              disabled={policy.listing || policy.saving}
              onclick={() => void refreshPolicies()}
            >
              {policy.listing ? t("Loading...") : t("Refresh policies")}
            </Button>
          </div>
          {#if policy.listFailed}
            <p class="notice" role="alert">
              {t(
                "Failed to load Keenetic policies. Your selection has been kept. Retry the refresh or disable the filter.",
              )}
            </p>
          {:else if policy.listLoaded && !policy.policies.length && !policy.draft.policy_id}
            <p class="notice">
              {t("No policies found. Create a policy in Keenetic, then refresh this list.")}
            </p>
          {:else if policy.listLoaded && policy.draft.policy_id && !selected?.available}
            <p class="notice" role="alert">
              {t(
                "The selected policy is missing or unavailable. Select another policy or disable the filter.",
              )}
            </p>
          {/if}
        </div>
        <fieldset class="modes">
          <legend>{t("Processing mode")}</legend>
          <label class="mode">
            <input
              type="radio"
              name="keenetic-policy-mode"
              value="include"
              bind:group={policy.draft.mode}
            />
            <span>{t("Process only devices in the selected policy")}</span>
          </label>
          <label class="mode">
            <input
              type="radio"
              name="keenetic-policy-mode"
              value="exclude"
              bind:group={policy.draft.mode}
            />
            <span>{t("Do not process devices in the selected policy")}</span>
          </label>
          <p class="description">
            {t("Unprocessed devices use normal Keenetic routing. Their traffic is not blocked.")}
          </p>
        </fieldset>
      {/if}
    </fieldset>
    <p class="runtime" role="status">{t(status)}</p>
    {#if dirty}<p class="description">{t("Changes will take effect after saving.")}</p>{/if}
    {#if policy.persistencePending}
      <p class="notice" role="alert">
        {t("Changes are applied but not saved to disk. Retry saving.")}
      </p>
    {/if}
    <p class="description">
      {t("Already established connections may keep their previous route until they reconnect.")}
    </p>
  {/if}
</section>

<style>
  h2 {
    margin: 0 0 1rem;
    color: var(--text-2);
    font: 600 1.3rem / 1.4 var(--font);
  }
  fieldset {
    min-width: 0;
    border: 0;
    padding: 0;
    margin: 0;
  }
  .description {
    color: var(--text-2);
    line-height: 1.5;
    margin: 0.6rem 0;
  }
  .switch-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 1rem;
    padding: 1rem 0;
  }
  .policy-choice {
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
    margin: 0.5rem 0 1.25rem;
  }
  .picker-row {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.75rem;
    min-width: 0;
  }
  .picker-row :global(.select-wrap),
  .picker-row :global([data-select-trigger]) {
    max-width: 100%;
    min-width: 0;
  }
  .modes legend {
    margin-bottom: 0.75rem;
  }
  .mode {
    display: flex;
    align-items: flex-start;
    gap: 0.65rem;
    padding: 0.5rem 0;
    cursor: pointer;
    line-height: 1.5;
  }
  .mode input {
    margin-top: 0.25rem;
    accent-color: var(--accent);
  }
  .notice {
    color: var(--text);
    line-height: 1.5;
    border-left: 2px solid var(--accent);
    padding-left: 0.75rem;
    margin: 0.5rem 0;
  }
  .runtime {
    line-height: 1.5;
    margin-bottom: 0.5rem;
  }
  .policy-settings :global(button:focus-visible),
  .mode input:focus-visible {
    outline: 2px solid var(--accent);
    outline-offset: 3px;
  }
  @media (max-width: 570px) {
    .picker-row {
      align-items: stretch;
    }
  }
</style>
