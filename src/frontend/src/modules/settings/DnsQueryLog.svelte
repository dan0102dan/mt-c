<script lang="ts">
  import { untrack } from "svelte";

  import { t } from "../../data/locale.svelte";
  import { fetcher } from "../../utils/fetcher";
  import { settingsSection } from "./section-controls";
  import {
    dnsQueryType,
    filterDnsQueries,
    groupDnsQueries,
    type DnsQuery,
    type DnsQueryResponse,
  } from "./dns-query-data";

  let { active = true }: { active?: boolean } = $props();

  const section = settingsSection();
  const pageSize = 30;
  let queries = $state<DnsQuery[]>([]);
  let capacity = $state(1000);
  let loading = $state(false);
  let clearing = $state(false);
  let failed = $state(false);
  let paused = $state(false);
  let grouped = $state(false);
  let search = $state("");
  let device = $state("");
  let page = $state(1);
  let requestGeneration = 0;

  const clients = $derived([...new Set(queries.map((row) => row.client))].sort());
  const filtered = $derived(filterDnsQueries(queries, search, device));
  const groupedRows = $derived(groupDnsQueries(filtered));
  const total = $derived(grouped ? groupedRows.length : filtered.length);
  const pages = $derived(Math.max(1, Math.ceil(total / pageSize)));
  const safePage = $derived(Math.min(page, pages));
  const groupedPage = $derived(groupedRows.slice((safePage - 1) * pageSize, safePage * pageSize));
  const queryPage = $derived(filtered.slice((safePage - 1) * pageSize, safePage * pageSize));
  const filtersKey = $derived(`${search}\u0000${device}\u0000${grouped}`);

  $effect(() => {
    filtersKey;
    page = 1;
  });

  async function refresh() {
    if (loading || clearing) return;
    const generation = ++requestGeneration;
    loading = true;
    try {
      const response = await fetcher.get<DnsQueryResponse>("/system/dns/queries", {
        silent: true,
        timeoutMs: 8000,
      });
      if (generation !== requestGeneration) return;
      queries = response.queries ?? [];
      capacity = response.capacity || 1000;
      failed = false;
    } catch {
      if (generation === requestGeneration) failed = true;
    } finally {
      if (generation === requestGeneration) loading = false;
    }
  }

  async function clearHistory() {
    if (clearing || !confirm(t("Clear DNS query history?"))) return;
    // Invalidate any slow GET response before the DELETE starts.
    ++requestGeneration;
    loading = false;
    clearing = true;
    try {
      await fetcher<{ cleared: boolean }>("/system/dns/queries", {
        method: "DELETE",
        silent: true,
        timeoutMs: 8000,
      });
      queries = [];
      page = 1;
      failed = false;
    } catch {
      failed = true;
    } finally {
      clearing = false;
    }
  }

  // Tab panels remain mounted when hidden; do not poll other tabs or a
  // background browser window. Pausing only freezes the UI polling.
  $effect(() => {
    if (!active || paused) return;
    const update = () => {
      if (!document.hidden) untrack(() => void refresh());
    };
    update();
    const timer = setInterval(update, 3000);
    document.addEventListener("visibilitychange", update);
    return () => {
      clearInterval(timer);
      document.removeEventListener("visibilitychange", update);
    };
  });

  const displayTime = (timestamp: number) => new Date(timestamp).toLocaleString();
</script>

<section
  class="dns-query-log"
  aria-labelledby="dns-query-title"
  aria-busy={loading || clearing}
  use:section={() => ({
    title: t("DNS queries"),
    dirty: false,
    canSave: false,
    onSave: async () => {},
    saveLabel: t("Save Changes"),
  })}
>
  <div class="section-heading">
    <div>
      <h2 id="dns-query-title">{t("DNS queries")}</h2>
      <p class="subtitle">{t("Requests captured by the DNS proxy. Cached or encrypted DNS traffic is not visible.")}</p>
    </div>
    <span class="count" aria-live="polite">{queries.length} / {capacity}</span>
  </div>

  <div class="filters">
    <label class="search-field">
      <span>{t("Search domain")}</span>
      <input
        data-page-search
        type="search"
        placeholder={t("Search domains...")}
        bind:value={search}
        autocomplete="off"
      />
    </label>
    <label class="device-field">
      <span>{t("Device")}</span>
      <select bind:value={device} aria-label={t("Device")}>
        <option value="">{t("All devices")}</option>
        {#if device && !clients.includes(device)}
          <option value={device}>{device}</option>
        {/if}
        {#each clients as client (client)}
          <option value={client}>{client}</option>
        {/each}
      </select>
    </label>
    <div class="actions">
      <button type="button" class:chosen={grouped} aria-pressed={grouped} onclick={() => (grouped = !grouped)}>
        {grouped ? t("Individual queries") : t("Group by domain")}
      </button>
      <button type="button" aria-pressed={paused} onclick={() => (paused = !paused)}>
        {paused ? t("Resume") : t("Pause")}
      </button>
      <button type="button" disabled={loading || clearing} onclick={() => void refresh()}>
        {t("Refresh")}
      </button>
      <button class="clear-button" type="button" disabled={clearing || !queries.length} onclick={() => void clearHistory()}>
        {t("Clear history")}
      </button>
    </div>
  </div>

  {#if failed}
    <div class="error-message" role="alert">
      {t("Failed to load DNS queries")}
      <button type="button" onclick={() => void refresh()} disabled={loading || clearing}>{t("Retry")}</button>
    </div>
  {/if}

  {#if !queries.length && loading}
    <p class="empty-message">{t("Loading DNS queries...")}</p>
  {:else if !total}
    <p class="empty-message">{queries.length ? t("No matching DNS queries") : t("No DNS queries captured yet")}</p>
  {:else}
    <div class="table-scroll" tabindex="0" role="region" aria-label={t("DNS query results")}>
      <table>
        <thead>
          {#if grouped}
            <tr><th>{t("Domain")}</th><th>{t("Requests")}</th><th>{t("Clients")}</th><th>{t("Last request")}</th></tr>
          {:else}
            <tr><th>{t("Time")}</th><th>{t("Client")}</th><th>{t("Domain")}</th><th>{t("Type")}</th><th>{t("Protocol")}</th></tr>
          {/if}
        </thead>
        <tbody>
          {#if grouped}
            {#each groupedPage as item (item.domain)}
              <tr>
                <td class="domain">{item.domain}</td>
                <td class="number">{item.count}</td>
                <td class="client-list" title={item.clients.join(", ")}>{item.clients.join(", ")}</td>
                <td class="timestamp">{displayTime(item.lastTime)}</td>
              </tr>
            {/each}
          {:else}
            {#each queryPage as row (row.id)}
              <tr>
                <td class="timestamp">{displayTime(row.time_ms)}</td>
                <td class="client-list">{row.client}</td>
                <td class="domain">{row.domain}</td>
                <td>{dnsQueryType(row.qtype)}</td>
                <td>{row.protocol.toUpperCase()}</td>
              </tr>
            {/each}
          {/if}
        </tbody>
      </table>
    </div>
  {/if}

  <div class="footer">
    <span>{t("Showing")} {total ? (safePage - 1) * pageSize + 1 : 0}–{Math.min(safePage * pageSize, total)} {t("of")} {total}</span>
    {#if pages > 1}
      <div class="pagination">
        <button type="button" disabled={safePage <= 1} onclick={() => (page = safePage - 1)} aria-label={t("Previous page")}>‹</button>
        <span>{safePage} / {pages}</span>
        <button type="button" disabled={safePage >= pages} onclick={() => (page = safePage + 1)} aria-label={t("Next page")}>›</button>
      </div>
    {/if}
  </div>
</section>

<style>
  .dns-query-log { min-width: 0; }
  .section-heading { display: flex; flex-wrap: wrap; align-items: flex-start; justify-content: space-between; gap: 0.6rem 1rem; margin-bottom: 1rem; }
  h2 { margin: 0 0 0.35rem; color: var(--text-2); font: 600 1.3rem / 1.4 var(--font); }
  .subtitle { margin: 0; color: var(--text-2); font-size: 0.87rem; line-height: 1.5; }
  .count { font: 0.83rem / 1.5 var(--font-mono); color: var(--text-2); padding-top: 0.35rem; white-space: nowrap; }
  .filters { display: flex; align-items: flex-end; flex-wrap: wrap; gap: 0.7rem; margin-bottom: 0.8rem; }
  .search-field, .device-field { display: grid; gap: 0.3rem; min-width: 0; }
  .search-field { flex: 1 1 14rem; }
  .device-field { flex: 0 1 13rem; }
  label span { color: var(--text-2); font-size: 0.85rem; }
  input, select { box-sizing: border-box; width: 100%; max-width: 100%; height: 2.4rem; border: 1px solid var(--border-light); border-radius: 0.5rem; background: var(--bg-light); color: var(--text); padding: 0.45rem 0.7rem; font: 0.9rem var(--font); }
  input:focus-visible, select:focus-visible, button:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
  .actions { display: flex; flex: 1 1 100%; gap: 0.5rem; flex-wrap: wrap; }
  button { background: var(--bg-light); color: var(--text); border: 1px solid var(--border-light); border-radius: 0.5rem; padding: 0.45rem 0.75rem; cursor: pointer; font: 0.86rem var(--font); min-height: 2.35rem; }
  button:hover:not(:disabled), button.chosen { border-color: var(--accent); background: var(--bg-light-extra); }
  button:disabled { opacity: 0.45; cursor: default; }
  .clear-button { margin-left: auto; color: var(--red); }
  .error-message { display: flex; gap: 1rem; flex-wrap: wrap; align-items: center; color: var(--red); margin: 0.6rem 0; font-size: 0.88rem; }
  .error-message button { color: var(--text); }
  .empty-message { color: var(--text-2); text-align: center; padding: 2.5rem 0.5rem; margin: 0; }
  .table-scroll { width: 100%; max-width: 100%; overflow: auto; border: 1px solid var(--border-light); border-radius: 0.6rem; }
  table { width: 100%; min-width: 640px; border-collapse: collapse; text-align: left; font-size: 0.87rem; }
  th { color: var(--text-2); font-weight: 500; background: var(--bg-medium); position: sticky; top: 0; }
  th, td { padding: 0.67rem 0.8rem; border-bottom: 1px solid var(--border-light); }
  tr:last-child td { border-bottom: 0; }
  tbody tr:hover { background: var(--bg-light); }
  td.domain { overflow-wrap: anywhere; min-width: 10rem; max-width: 21rem; }
  .client-list { font-family: var(--font-mono); font-size: 0.8rem; overflow-wrap: anywhere; max-width: 18rem; }
  .timestamp { color: var(--text-2); white-space: nowrap; font-size: 0.81rem; }
  .number { font-variant-numeric: tabular-nums; }
  .footer { display: flex; align-items: center; justify-content: space-between; gap: 1rem; flex-wrap: wrap; font-size: 0.85rem; color: var(--text-2); padding: 0.6rem 0; }
  .pagination { display: flex; align-items: center; gap: 0.8rem; }
  .pagination button { min-height: 2rem; min-width: 2rem; padding: 0; font-size: 1.3rem; }
  @media (max-width: 700px) {
    .search-field, .device-field { flex: 1 1 100%; }
    .actions { gap: 0.45rem; }
    .actions button { flex: 1 1 auto; }
    .clear-button { margin-left: 0; }
    .table-scroll { border-radius: 0.4rem; }
  }
</style>
