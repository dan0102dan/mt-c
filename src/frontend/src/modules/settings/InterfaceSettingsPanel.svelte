<script lang="ts">
  import InfoDialog from "../../components/InfoDialog.svelte";
  import Button from "../../components/ui/Button.svelte";
  import FormField from "../../components/ui/FormField.svelte";
  import FormSection from "../../components/ui/FormSection.svelte";
  import SectionDescription from "../../components/ui/SectionDescription.svelte";
  import Select from "../../components/ui/Select.svelte";
  import SwitchField from "../../components/ui/SwitchField.svelte";
  import { showGroupPriority, showSubscriptionPriority } from "../../data/interface.svelte";
  import { locale, t } from "../../data/locale.svelte";
  import { updates } from "../../data/updates.svelte";
  import UpdateControl from "../updates/UpdateControl.svelte";
  import SettingsSection from "./SettingsSection.svelte";

  import { Info } from "../../components/ui/icons";

  let { active }: { active: boolean } = $props();
  let infoIsOpen = $state(false);

  const languages = [
    { value: "en", label: "English" },
    { value: "ru", label: "Русский" },
  ];

  $effect(() => {
    if (!active) {
      infoIsOpen = false;
      updates.dialogOpen = false;
    }
  });
</script>

<SettingsSection
  id="interface"
  title={t("Interface")}
  controls={() => ({ canSave: false, dirty: false, onSave: async () => {} })}
>
  <SectionDescription>
    {t("Changes apply immediately and are saved only in this browser.")}
  </SectionDescription>

  <div class="interface-form">
    <FormSection id="interface-display-heading" title={t("Display")}>
      <div class="language-field">
        <FormField label={t("Language")}>
          <Select
            options={languages}
            bind:selected={locale.current}
            ariaLabel={t("Language")}
            triggerClass="form-control"
          />
        </FormField>
      </div>
    </FormSection>

    <FormSection id="interface-priorities-heading" title={t("Priorities")}>
      <SwitchField
        id="show-group-priorities"
        label={t("Show group priorities")}
        bind:checked={showGroupPriority.current}
      />
      <SwitchField
        id="show-subscription-priorities"
        label={t("Show subscription priorities")}
        bind:checked={showSubscriptionPriority.current}
      />
      <p class="priority-hint">{t("Hiding priorities does not change their values or routing.")}</p>
    </FormSection>

    <FormSection id="interface-about-heading" title={t("About this app")}>
      <div class="about-row">
        <div class="build-info">
          <span class="app-name">MagiTrickle</span>
          <UpdateControl />
        </div>
        <Button small class="about-button" onclick={() => (infoIsOpen = true)}>
          <Info size={16} aria-hidden="true" />
          <span>{t("About")}</span>
        </Button>
      </div>
    </FormSection>
  </div>
</SettingsSection>

<InfoDialog bind:open={infoIsOpen} />

<style>
  .interface-form {
    display: grid;
    gap: var(--space-6);
  }
  .language-field {
    max-width: 18rem;
  }
  .language-field :global(.select-wrap) {
    width: 100%;
    max-width: 100%;
  }
  .priority-hint {
    margin: var(--space-2) 0 0;
    color: var(--text-2);
    font: 400 var(--text-small) / var(--line-height-body) var(--font);
  }
  .about-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: var(--space-4);
    min-width: 0;
  }
  .build-info {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: var(--space-3);
    min-width: 0;
  }
  .app-name {
    font-size: var(--text-body);
  }
  .build-info :global(.version-trigger) {
    max-width: 100%;
  }
  .about-row :global(.about-button) {
    gap: var(--space-2);
    flex-shrink: 0;
  }
</style>
