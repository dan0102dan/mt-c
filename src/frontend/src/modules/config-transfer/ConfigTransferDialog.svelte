<script lang="ts">
  import { Tabs } from "bits-ui";
  import { untrack } from "svelte";

  import Button from "../../components/ui/Button.svelte";
  import Checkbox from "../../components/ui/Checkbox.svelte";
  import GenericDialog from "../../components/ui/GenericDialog.svelte";
  import Placeholder from "../../components/ui/Placeholder.svelte";
  import Select from "../../components/ui/Select.svelte";
  import Switch from "../../components/ui/Switch.svelte";
  import Tooltip from "../../components/ui/Tooltip.svelte";
  import { t } from "../../data/locale.svelte";

  import { Check, Scan } from "../../components/ui/icons";
  import { randomId } from "../../utils/defaults";
  import { newProfileId } from "../settings/profiles-data";
  import { unsupportedSettings } from "./adapter";
  import {
    conflictKey,
    planImport,
    sections,
    type Choice,
    type ImportPlan,
    type PortableConfig,
    type Section,
    type Snapshot,
  } from "./model";

  type ImportTab = "groups" | "subscriptions" | "settings";
  const tabs: ImportTab[] = ["groups", "subscriptions", "settings"];
  const tabFor = (section: Section): ImportTab => (section === "profiles" ? "settings" : section);
  type ChoiceMap = Record<string, Choice>;
  type Props = {
    open: boolean;
    initialSection: ImportTab;
    imported: PortableConfig;
    current: Snapshot;
    policies: { id: string; name: string; available: boolean }[];
    policySupported: boolean;
    busy?: boolean;
    onclose: () => void;
    onapply: (plan: ImportPlan) => Promise<void>;
  };
  let {
    open,
    initialSection,
    imported,
    current,
    policies,
    policySupported,
    busy = false,
    onclose,
    onapply,
  }: Props = $props();

  const names: Record<Section, string> = $derived({
    groups: t("Groups"),
    subscriptions: t("Subscriptions"),
    profiles: t("Profiles"),
    settings: t("Settings"),
  });
  const settingNames: Record<string, string> = $derived({
    keeneticPolicy: t("Routing policy"),
  });
  const choiceOptions = $derived([
    { value: "keep", label: t("Keep current") },
    { value: "replace", label: t("Use imported") },
    { value: "copy", label: t("Create a copy") },
  ]);
  let replaceMode = $state(false);
  const mode = $derived(replaceMode ? "replace" : "merge");
  function showTab(value: string) {
    if (tabs.includes(value as ImportTab)) activeTab = value as ImportTab;
  }
  const entries = $derived(
    sections.flatMap<{ key: string; section: Section; index: number; name: string; meta: string }>(
      (section) => {
        if (imported[section] === undefined) return [];
        if (section === "settings")
          return Object.keys(imported.settings ?? {}).map((field) => ({
            key: `settings:${field}`,
            section,
            index: -1,
            name: settingNames[field] ?? field,
            meta: "",
          }));
        const items = imported[section] ?? [];
        // An explicit empty collection is an importable instruction in Replace
        // mode. Give it a checkbox; an absent section still has no entry.
        if (!items.length)
          return [
            {
              key: `${section}:empty`,
              section,
              index: -1,
              name: names[section],
              meta: "",
            },
          ];
        return items.map((item, index) => ({
          key: `${section}:${index}`,
          section,
          index,
          name: item.name || `${names[section]} ${index + 1}`,
          meta:
            section === "groups"
              ? `#${imported.groups![index].rules.length}`
              : section === "profiles"
                ? imported.profiles![index].interfaces.join(" → ")
                : visibleValue(imported.subscriptions![index].url, "url"),
        }));
      },
    ),
  );
  // Full exports include empty collections. Prefer actual objects/fields over
  // empty placeholders so a profile export opened from Groups selects profiles.
  // Empty-only files still preserve the source page for explicit Replace.
  const initialTab = untrack(() => {
    const populated = entries.filter((entry) => entry.key !== `${entry.section}:empty`);
    const available = populated.length ? populated : entries;
    return available.some((entry) => tabFor(entry.section) === initialSection)
      ? initialSection
      : (tabs.find((tab) => available.some((entry) => tabFor(entry.section) === tab)) ??
          initialSection);
  });
  let activeTab = $state<ImportTab>(initialTab);
  let excluded = $state(
    new Set<string>(
      untrack(() =>
        entries.filter((entry) => tabFor(entry.section) !== initialTab).map((entry) => entry.key),
      ),
    ),
  );
  const visibleEntries = $derived(entries.filter((entry) => tabFor(entry.section) === activeTab));
  const selectedCounts = $derived.by(() => {
    const counts: Record<ImportTab, number> = { groups: 0, subscriptions: 0, settings: 0 };
    for (const entry of entries) {
      if (!excluded.has(entry.key)) counts[tabFor(entry.section)]++;
    }
    return counts;
  });
  const tabEntriesSelected = $derived(selectedCounts[activeTab]);
  const selectedCount = $derived(tabs.reduce((count, tab) => count + selectedCounts[tab], 0));
  function toggleEntry(key: string, checked: boolean) {
    const next = new Set(excluded);
    if (checked) next.delete(key);
    else next.add(key);
    excluded = next;
  }
  function selectAll(value: boolean) {
    const next = new Set(excluded);
    for (const entry of visibleEntries) {
      if (value) next.delete(entry.key);
      else next.add(entry.key);
    }
    excluded = next;
  }
  const selected = $derived(
    new Set(entries.filter((entry) => !excluded.has(entry.key)).map((entry) => entry.section)),
  );
  const input = $derived.by(() => {
    const groups = imported.groups?.filter(
      (_, index) => selected.has("groups") && !excluded.has(`groups:${index}`),
    );
    const subscriptions = imported.subscriptions?.filter(
      (_, index) => selected.has("subscriptions") && !excluded.has(`subscriptions:${index}`),
    );
    const referenced = new Set(
      [...(groups ?? []), ...(subscriptions ?? [])].map((item) => item.profile),
    );
    return {
      ...imported,
      settings:
        imported.settings === undefined
          ? undefined
          : Object.fromEntries(
              Object.entries(imported.settings).filter(
                ([field]) => !excluded.has(`settings:${field}`),
              ),
            ),
      groups,
      subscriptions,
      profiles: imported.profiles?.filter(
        (item, index) =>
          (selected.has("profiles") && !excluded.has(`profiles:${index}`)) ||
          referenced.has(item.id),
      ),
    };
  });
  let choices = $state<ChoiceMap>({});
  let policyId = $state("");
  const generatedIds = new Map<string, string>();
  const reservedIds = untrack(() => {
    const ids = new Set<string>();
    for (const source of [current, imported]) {
      for (const section of ["groups", "subscriptions", "profiles"] as const) {
        for (const item of source[section] ?? []) ids.add(item.id);
      }
      for (const group of source.groups ?? []) {
        for (const rule of group.rules) ids.add(rule.id);
      }
    }
    return ids;
  });
  function newId(section: "groups" | "subscriptions" | "profiles", old: string) {
    const key = conflictKey(section, old);
    if (!generatedIds.has(key)) {
      let id: string;
      do {
        id = section === "profiles" ? newProfileId() : randomId();
      } while (reservedIds.has(id));
      generatedIds.set(key, id);
      reservedIds.add(id);
    }
    return generatedIds.get(key)!;
  }

  const plan = $derived.by(() =>
    planImport(current, input, {
      mode,
      selected,
      choices: new Map(Object.entries(choices)),
      policyId: policyId || undefined,
      newId,
    }),
  );

  const inputPolicy = $derived.by(() => {
    if (!selected.has("settings")) return undefined;
    const source = input.settings?.keeneticPolicy;
    if (!source || typeof source !== "object" || Array.isArray(source)) return undefined;
    return source as { enabled?: boolean; policyId?: string; mode?: string };
  });
  const missingPolicy = $derived.by(() => {
    if (!inputPolicy) return false;
    if (!policySupported) return true;
    if (!inputPolicy.enabled) return false;
    const id = policyId || inputPolicy.policyId;
    return !id;
  });
  const errors = $derived([
    ...plan.problems.map(problemMessage),
    ...(selected.has("settings")
      ? unsupportedSettings(input.settings ?? {}, current.settings).map(
          (key) => `${t("Unsupported setting")}: ${key}`,
        )
      : []),
    ...(missingPolicy
      ? [policySupported ? t("Select policy") : t("This build does not support Keenetic policies")]
      : []),
  ]);
  function problemMessage(problem: string) {
    const separator = problem.indexOf(": ");
    const reason = problem.slice(0, separator);
    const value = problem.slice(separator + 2);
    if (reason === "Invalid routing profile") {
      const split = value.indexOf(": ");
      const id = value.slice(0, split);
      const error = value.slice(split + 2);
      const profile = plan.next.profiles.find((item) => item.id === id);
      return `${t("Profile")} ${profile?.name.trim() || id}: ${t(error)}`;
    }
    if (reason === "Missing routing profile") {
      const removedProfile = current.profiles.some((profile) => profile.id === value);
      const changedRoutes = new Set(
        plan.changes
          .filter(
            (change) =>
              (change.section === "groups" || change.section === "subscriptions") &&
              change.kind !== "unchanged",
          )
          .map((change) => change.id),
      );
      const objects = [...plan.next.groups, ...plan.next.subscriptions]
        .filter(
          (route) => route.profile === value && (removedProfile || changedRoutes.has(route.id)),
        )
        .map((route) => route.name || names["groups"]);
      return `${t("Routing profiles are missing for")}: ${objects.join(", ")}. ${t("Deselect these objects or include their profiles in the file.")}`;
    }
    if (reason === "Profile contains duplicate interfaces after mapping")
      return `${t(reason)}: ${value}`;
    return `${t("Duplicate object ID")}: ${value}`;
  }
  function visibleValue(value: unknown, field: string) {
    if (value === undefined) return "—";
    if (/token|password|secret|key|credential/i.test(field)) return "••••";
    let printed = typeof value === "string" ? value : JSON.stringify(value);
    printed = printed.replace(/([?&](?:token|key|password|secret)=)[^&#]+/gi, "$1•••");
    return printed;
  }
  function setChoice(key: string, choice: string) {
    choices = { ...choices, [key]: choice as Choice };
  }
</script>

<GenericDialog {open} title={t("Import Config")} maxWidth={420} on:close={onclose}>
  <div slot="body" class="import-config" aria-busy={busy}>
    <Tabs.Root value={activeTab} onValueChange={showTab}>
      <div class="import-tabs">
        <Tabs.List aria-label={t("Import sections")}>
          {#each tabs as tab}
            <Tabs.Trigger value={tab} disabled={busy} aria-label={names[tab]}>
              {names[tab]}
              <span class="selected-count" aria-hidden="true">{selectedCounts[tab]}</span>
            </Tabs.Trigger>
          {/each}
        </Tabs.List>
      </div>
      {#each tabs as tab}
        {#if activeTab === tab}<Tabs.Content value={tab}>
            <div class="controls-row">
              <div class="controls">
                <Button
                  type="button"
                  disabled={busy || !visibleEntries.length}
                  onclick={() => selectAll(true)}
                >
                  <Check size={16} /><span class="with-icon">{t("All")}</span>
                </Button>
                <Button
                  type="button"
                  disabled={busy || !visibleEntries.length}
                  onclick={() => selectAll(false)}
                >
                  <Scan size={16} /><span class="with-icon">{t("Reset")}</span>
                </Button>
              </div>
              <div class="selected-count">{tabEntriesSelected} / {visibleEntries.length}</div>
            </div>
            <div class="import-list">
              {#each sections.filter((section) => tabFor(section) === tab) as section}
                {#if visibleEntries.some((entry) => entry.section === section)}
                  <section aria-label={names[section]}>
                    {#if section === "profiles"}<h3>{names[section]}</h3>{/if}
                    {#each visibleEntries.filter((entry) => entry.section === section) as entry (entry.key)}
                      <label class="group-option">
                        <Checkbox
                          ariaLabel={entry.name}
                          checked={!excluded.has(entry.key)}
                          disabled={busy}
                          on:change={(event) => toggleEntry(entry.key, event.detail.checked)}
                        />
                        <div
                          class="group-info"
                          class:subscription-info={section === "subscriptions"}
                        >
                          <span class="group-name">{entry.name}</span>
                          {#if entry.meta}<span class="group-meta">{entry.meta}</span>{/if}
                        </div>
                      </label>
                    {/each}
                  </section>
                {/if}
              {/each}
              {#if !visibleEntries.length}<Placeholder
                  variant="empty"
                  compact
                  iconSize={22}
                  minHeight="120px">{t("This file has no data for this section")}</Placeholder
                >{/if}
            </div>
          </Tabs.Content>{/if}
      {/each}
    </Tabs.Root>
    {#if inputPolicy?.enabled && policySupported}
      <section class="mapping" aria-label={t("Policy mapping")}>
        <h3>{t("Routing policy")}</h3>
        <p>{t("Original policy")}: <code>{inputPolicy.policyId || t("Not specified")}</code></p>
        <Select
          class="form-select"
          selected={policyId || inputPolicy.policyId || ""}
          options={[
            { value: "", label: t("Select policy") },
            ...(inputPolicy.policyId && !policies.some((item) => item.id === inputPolicy.policyId)
              ? [{ value: inputPolicy.policyId, label: inputPolicy.policyId }]
              : []),
            ...policies.map((item) => ({
              value: item.id,
              label: item.name || item.id,
            })),
          ]}
          onValueChange={(value) => (policyId = value)}
          ariaLabel={t("Keenetic policy mapping")}
          disabled={busy}
        />
      </section>
    {/if}

    {#if plan.conflicts.length && mode === "merge"}
      <section class="mapping" aria-label={t("Conflicts")}>
        <h3>{t("Objects already exist")}</h3>
        {#each plan.conflicts as conflict (conflictKey(conflict.section, conflict.id))}
          <label class="mapping-line">
            <span>{conflict.name} <small>({names[conflict.section]})</small></span>
            <Select
              class="form-select"
              selected={choices[conflictKey(conflict.section, conflict.id)] || "keep"}
              options={choiceOptions}
              onValueChange={(value) =>
                setChoice(conflictKey(conflict.section, conflict.id), value)}
              ariaLabel={`${t("Resolve conflict for")} ${conflict.name}`}
              disabled={busy}
            />
          </label>
        {/each}
      </section>
    {/if}
  </div>
  <div slot="actions" class="dialog-actions">
    <div class="mode-toggle">
      <span class:active={!replaceMode}>{t("Append")}</span>
      <Switch bind:checked={replaceMode} disabled={busy} aria-label={t("Replace")} />
      <span class:active={replaceMode}>{t("Replace")}</span>
    </div>
    <div class="buttons">
      <Button type="button" onclick={onclose} disabled={busy}>{t("Cancel")}</Button>
      <Tooltip value={errors.join("\n") || t("Import")}
        ><Button
          type="button"
          aria-description={errors.length ? errors.join("\n") : undefined}
          disabled={busy || !selectedCount || !!errors.length}
          onclick={() => {
            if (!busy && selectedCount && !errors.length) void onapply(plan);
          }}
        >
          {busy ? t("Applying…") : t("Import")}
        </Button></Tooltip
      >
    </div>
  </div>
</GenericDialog>

<style>
  .import-config {
    display: flex;
    flex-direction: column;
    gap: 1rem;
    min-width: 0;
    padding: 0.25rem 0.2rem;
    max-height: 60vh;
    overflow-y: auto;
    overflow-wrap: anywhere;
  }

  .controls-row {
    display: flex;
    justify-content: space-between;
    align-items: center;
    gap: 0.5rem;
    flex-wrap: wrap;
  }

  .controls {
    display: inline-flex;
    gap: 0.5rem;
  }

  .with-icon {
    margin-left: 0.25rem;
  }

  .mode-toggle {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    font-size: 0.85rem;
    color: var(--text-2);
  }

  .mode-toggle span {
    transition: color 0.2s ease;
  }

  .mode-toggle span.active {
    color: var(--text);
    font-weight: 500;
  }

  .import-list {
    min-width: 0;
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
    padding: 0.5rem 0;
  }

  .group-option {
    display: flex;
    gap: 0.75rem;
    align-items: center;
    padding: 0.75rem 1rem;
    border-radius: 0.55rem;
    border: 1px solid var(--bg-light-extra);
    background-color: var(--bg-light);
    position: relative;
  }

  .group-info {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.75rem;
    flex: 1;
    min-width: 0;
  }

  .group-name {
    font-weight: 600;
    color: var(--text);
    word-break: break-word;
    flex: 1;
  }

  .group-meta {
    font-size: 0.8rem;
    color: var(--text-2);
    overflow-wrap: anywhere;
    max-width: 55%;
    min-width: 0;
    font-variant-numeric: tabular-nums;
  }

  .subscription-info {
    flex-direction: column;
    align-items: stretch;
    gap: 0.25rem;
  }

  .subscription-info .group-meta {
    max-width: 100%;
  }

  .dialog-actions {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-top: 0.75rem;
  }

  @media (max-width: 500px) {
    .dialog-actions {
      flex-direction: column;
      align-items: stretch;
      gap: 1rem;
    }

    .mode-toggle {
      justify-content: center;
    }

    .buttons {
      display: flex;
      width: 100%;
      justify-content: space-between;
    }
  }

  .buttons {
    display: flex;
    gap: 0.5rem;
  }

  .selected-count {
    font-size: 0.85rem;
    color: var(--text-2);
  }

  .import-list section {
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
  }
  h3 {
    margin: 0;
    font-size: 1rem;
  }
  .mapping {
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
  }
  .mapping p {
    margin: 0;
  }
  .mapping-line {
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
    min-width: 0;
  }
  .import-config :global(.form-select) {
    width: 100%;
    min-width: 0;
  }
  .import-config :global(.form-select [data-select-trigger]) {
    width: 100%;
    box-sizing: border-box;
    background: var(--bg-dark-extra);
    border: 1px solid var(--bg-light-extra);
    padding: 0.75rem;
  }
  .import-config :global(.form-select .selected) {
    width: 100%;
    justify-content: space-between;
  }
  .import-config :global(.checkbox .box) {
    pointer-events: none;
  }
  .import-tabs :global([data-tabs-list]) {
    display: flex;
    flex-wrap: wrap;
    gap: 0.5rem;
  }
  .import-tabs :global([data-tabs-trigger]) {
    display: inline-flex;
    align-items: center;
    gap: 0.5rem;
    min-width: 0;
    max-width: 100%;
    padding: 0.6rem;
    border: 1px solid var(--bg-light-extra);
    border-radius: 0.5rem;
    background: var(--bg-light);
    color: var(--text-2);
    font: 400 1rem var(--font);
    overflow-wrap: anywhere;
    cursor: pointer;
  }
  .import-tabs :global([data-tabs-trigger][data-state="active"]),
  .import-tabs :global([data-tabs-trigger]:hover) {
    background: var(--bg-light-extra);
    color: var(--text);
  }
  .import-tabs :global([data-tabs-trigger]:focus-visible) {
    outline: 2px solid var(--accent);
    outline-offset: 2px;
  }
  .import-config :global([data-tabs-content]) {
    margin-top: 1rem;
  }
</style>
