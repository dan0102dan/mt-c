<script lang="ts">
  import GenericDialog from "../../components/ui/GenericDialog.svelte";
  import Button from "../../components/ui/Button.svelte";
  import Checkbox from "../../components/ui/Checkbox.svelte";
  import Select from "../../components/ui/Select.svelte";
  import { randomId } from "../../utils/defaults";
  import { newProfileId } from "../settings/profiles-data";
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

  type ChoiceMap = Record<string, Choice>;
  type Props = {
    open: boolean;
    fileName: string;
    imported: PortableConfig;
    current: Snapshot;
    interfaces: { id: string; name?: string }[];
    policies: { id: string; name: string; available: boolean }[];
    policySupported: boolean;
    busy?: boolean;
    onclose: () => void;
    onapply: (plan: ImportPlan) => Promise<void>;
  };
  let {
    open, fileName, imported, current, interfaces, policies,
    policySupported, busy = false, onclose, onapply,
  }: Props = $props();

  const names: Record<Section, string> = {
    groups: "Группы", subscriptions: "Подписки", profiles: "Профили", settings: "Настройки",
  };
  const choiceOptions = [
    { value: "keep", label: "Оставить текущий" },
    { value: "replace", label: "Использовать импортируемый" },
    { value: "copy", label: "Создать копию" },
  ];
  let mode = $state<"merge" | "replace">("merge");
  let enabled = $state<Record<Section, boolean>>({
    groups: true, subscriptions: true, profiles: true, settings: true,
  });
  let choices = $state<ChoiceMap>({});
  let mappings = $state<Record<string, string>>({});
  let policyId = $state("");
  const generatedIds = new Map<string, string>();
  function newId(section: "groups" | "subscriptions" | "profiles", old: string) {
    const key = conflictKey(section, old);
    if (!generatedIds.has(key))
      generatedIds.set(key, section === "profiles" ? newProfileId() : randomId());
    return generatedIds.get(key)!;
  }

  const selected = $derived(
    new Set(sections.filter((section) => enabled[section] && imported[section] !== undefined)),
  );
  const plan = $derived.by(() =>
    planImport(current, imported, {
      mode,
      selected,
      choices: new Map(Object.entries(choices)),
      interfaceMap: new Map(Object.entries(mappings).filter(([, value]) => Boolean(value))),
      policyId: policyId || undefined,
      newId,
    }),
  );

  const routeNames = $derived.by(() => {
    const names = new Set<string>();
    if (selected.has("profiles") || selected.has("groups") || selected.has("subscriptions")) {
      for (const p of imported.profiles ?? [])
        for (const iface of p.interfaces) if (iface !== "blackhole") names.add(iface);
      for (const section of ["groups", "subscriptions"] as const) {
        if (!selected.has(section)) continue;
        for (const route of imported[section] ?? [])
          if (!route.profile && route.interface && route.interface !== "blackhole")
            names.add(route.interface);
      }
    }
    return [...names].filter((iface) => !interfaces.some((current) => current.id === iface));
  });
  const missingInterfaces = $derived(routeNames.filter((name) => !mappings[name]));
  const inputPolicy = $derived.by(() => {
    if (!selected.has("settings")) return undefined;
    const source = imported.settings?.keeneticPolicy;
    if (!source || typeof source !== "object" || Array.isArray(source)) return undefined;
    return source as { enabled?: boolean; policyId?: string; mode?: string };
  });
  const missingPolicy = $derived.by(() => {
    if (!inputPolicy) return false;
    if (!policySupported) return true;
    if (!inputPolicy.enabled) return false;
    const id = policyId || inputPolicy.policyId;
    return !policies.some((p) => p.id === id && p.available);
  });
  const errors = $derived([
    ...plan.problems,
    ...missingInterfaces.map((value) => `Интерфейс «${value}» не найден на устройстве`),
    ...(missingPolicy
      ? [policySupported ? "Укажите доступную политику Keenetic" : "Эта сборка не поддерживает политику Keenetic"]
      : []),
  ]);
  const hasWork = $derived(plan.changes.some((change) => change.kind !== "unchanged"));
  function summary(section: Section) {
    const changes = plan.changes.filter((change) => change.section === section);
    const added = changes.filter((change) => change.kind === "add").length;
    const updated = changes.filter((change) => change.kind === "update").length;
    const removed = changes.filter((change) => change.kind === "remove").length;
    const unchanged = changes.filter((change) => change.kind === "unchanged").length;
    return [added && `+${added}`, updated && `~${updated}`, removed && `−${removed}`,
      unchanged && `=${unchanged}`].filter(Boolean).join(" · ") || "Без изменений";
  }
  function visibleValue(value: unknown, field: string) {
    if (value === undefined) return "—";
    if (/token|password|secret|key|credential/i.test(field)) return "••••";
    let printed = typeof value === "string" ? value : JSON.stringify(value);
    printed = printed.replace(/([?&](?:token|key|password|secret)=)[^&#]+/gi, "$1•••");
    return printed.length > 130 ? printed.slice(0, 130) + "…" : printed;
  }
  function setChoice(key: string, choice: string) {
    choices = { ...choices, [key]: choice as Choice };
  }
</script>

<GenericDialog {open} title="Импорт конфига" maxWidth={800} on:close={onclose}>
  <div slot="body" class="transfer-body" aria-busy={busy}>
    <p class="file-name">Файл: <strong>{fileName}</strong></p>
    <p class="intro">Проверьте изменения перед применением. До подтверждения текущая конфигурация не изменится.</p>
    <label class="mode">
      Режим импорта
      <Select
        compact
        selected={mode}
        options={[
          { value: "merge", label: "Объединить с текущей конфигурацией" },
          { value: "replace", label: "Заменить выбранные разделы" },
        ]}
        onValueChange={(value) => (mode = value === "replace" ? "replace" : "merge")}
        ariaLabel="Режим импорта"
        disabled={busy}
      />
    </label>

    <div class="section-list">
      {#each sections as section}
        {#if imported[section] !== undefined}
          <div class="section-line">
            <label>
              <Checkbox
                checked={enabled[section]}
                disabled={busy}
                on:change={(event) => (enabled = { ...enabled, [section]: event.detail.checked })}
              />
              <span>{names[section]}</span>
            </label>
            <span class="summary">{selected.has(section) ? summary(section) : "Пропустить"}</span>
          </div>
        {/if}
      {/each}
    </div>

    {#if routeNames.length}
      <section class="mapping" aria-label="Сопоставление интерфейсов">
        <h3>Интерфейсы устройства</h3>
        {#each routeNames as iface (iface)}
          <label class="mapping-line">
            <code>{iface}</code>
            <Select
              compact
              selected={mappings[iface] || ""}
              options={[
                { value: "", label: "Выберите интерфейс" },
                ...interfaces.map((item) => ({
                  value: item.id, label: item.name ? `${item.name} (${item.id})` : item.id,
                })),
              ]}
              onValueChange={(value) => (mappings = { ...mappings, [iface]: value })}
              ariaLabel={`Новый интерфейс для ${iface}`}
              disabled={busy}
            />
          </label>
        {/each}
      </section>
    {/if}

    {#if inputPolicy?.enabled && policySupported}
      <section class="mapping" aria-label="Сопоставление политики">
        <h3>Политика маршрутизации</h3>
        <p>Исходная политика: <code>{inputPolicy.policyId || "не указана"}</code></p>
        <Select
          compact
          selected={policyId || inputPolicy.policyId || ""}
          options={[
            { value: "", label: "Выберите политику" },
            ...policies.filter((item) => item.available).map((item) => ({
              value: item.id, label: item.name || item.id,
            })),
          ]}
          onValueChange={(value) => (policyId = value)}
          ariaLabel="Сопоставление политики Keenetic"
          disabled={busy}
        />
      </section>
    {/if}

    {#if plan.conflicts.length && mode === "merge"}
      <section class="mapping" aria-label="Конфликты">
        <h3>Конфликты объектов</h3>
        {#each plan.conflicts as conflict (conflictKey(conflict.section, conflict.id))}
          <label class="mapping-line">
            <span>{conflict.name} <small>({names[conflict.section]})</small></span>
            <Select
              compact
              selected={choices[conflictKey(conflict.section, conflict.id)] || "keep"}
              options={choiceOptions}
              onValueChange={(value) => setChoice(conflictKey(conflict.section, conflict.id), value)}
              ariaLabel={`Как обработать конфликт ${conflict.name}`}
              disabled={busy}
            />
          </label>
        {/each}
      </section>
    {/if}

    {#if plan.includedDependencies.length}
      <p class="hint">Для выбранных объектов будут также добавлены связанные профили:
        {plan.includedDependencies.join(", ")}</p>
    {/if}

    <details class="changes" open={errors.length > 0 || plan.changes.some((c) => c.kind === "remove")}>
      <summary>Предпросмотр изменений</summary>
      <div class="change-list">
        {#each plan.changes.filter((change) => change.kind !== "unchanged") as change, index (index)}
          <article class="change">
            <div class="change-head">
              <span>{names[change.section]} · {change.name}</span>
              <small>{change.kind === "add" ? "Добавится" : change.kind === "remove" ? "Удалится" : "Изменится"}</small>
            </div>
            {#each change.fields as field (field.field)}
              <div class="field-line">
                <code>{field.field}</code>
                <span>{visibleValue(field.before, field.field)} → {visibleValue(field.after, field.field)}</span>
              </div>
            {/each}
          </article>
        {/each}
        {#if !hasWork}<p>Изменений нет.</p>{/if}
      </div>
    </details>

    {#if errors.length}
      <div class="validation" role="alert">
        {#each errors as message (message)}<p>{message}</p>{/each}
      </div>
    {/if}
    {#if plan.changes.some((change) => change.section === "settings" && change.kind !== "unchanged")}
      <p class="hint">Для некоторых системных настроек может потребоваться перезапуск службы.</p>
    {/if}
  </div>
  <div slot="actions" class="actions">
    <Button type="button" onclick={onclose} disabled={busy}>Отмена</Button>
    <Button type="button" disabled={busy || !!errors.length || !hasWork}
      onclick={() => void onapply(plan)}>{busy ? "Применение…" : "Применить изменения"}</Button>
  </div>
</GenericDialog>

<style>
  .transfer-body { display: flex; flex-direction: column; gap: .85rem; min-width: 0;
    max-height: min(66vh, 640px); overflow-y: auto; padding: .15rem .25rem 1rem; }
  .file-name, .intro, .hint { margin: 0; color: var(--text-2); line-height: 1.45; font-size: .9rem; }
  .file-name strong { color: var(--text); overflow-wrap: anywhere; }
  .mode { display: flex; flex-wrap: wrap; align-items: center; gap: 1rem; justify-content: space-between; }
  .mode :global(.select-wrap) { min-width: min(290px, 100%); }
  .section-list { border: 1px solid var(--bg-light-extra); border-radius: .6rem; overflow: hidden; }
  .section-line { display: flex; justify-content: space-between; align-items: center;
    gap: .75rem; padding: .65rem .75rem; border-bottom: 1px solid var(--bg-light-extra); }
  .section-line:last-child { border-bottom: 0; }
  .section-line label { display: flex; align-items: center; gap: .75rem; font-weight: 600; }
  .summary, small { color: var(--text-2); font-size: .82rem; }
  .mapping { padding: .75rem; background: var(--bg-light); border-radius: .6rem; }
  h3 { font-size: 1rem; margin: 0 0 .65rem; }
  .mapping p { margin: .5rem 0; }
  .mapping-line { display: flex; gap: .75rem; align-items: center;
    justify-content: space-between; margin: .5rem 0; min-width: 0; }
  .mapping-line > span, .mapping-line > code { overflow-wrap: anywhere; min-width: 0; }
  .mapping-line :global(.select-wrap) { width: min(330px, 65%); flex-shrink: 0; }
  .changes { border-top: 1px solid var(--bg-light-extra); padding-top: .65rem; }
  summary { cursor: pointer; font-weight: 600; }
  .change-list { max-height: 260px; overflow-y: auto; margin-top: .6rem; }
  .change { padding: .55rem 0; border-bottom: 1px solid var(--bg-light-extra); }
  .change-head, .field-line { display: flex; gap: .7rem; justify-content: space-between; }
  .change-head { font-weight: 600; }
  .field-line { font-size: .78rem; padding-top: .25rem; overflow-wrap: anywhere; }
  .field-line code { flex-shrink: 0; }
  .field-line span { color: var(--text-2); text-align: right; }
  .validation { color: var(--danger); font-size: .85rem; }
  .validation p { margin: .25rem 0; }
  .actions { display: flex; justify-content: flex-end; gap: .5rem; width: 100%; }
  @media (max-width: 570px) {
    .mapping-line { align-items: stretch; flex-direction: column; }
    .mapping-line :global(.select-wrap) { width: 100%; }
    .section-line { align-items: flex-start; }
  }
</style>
