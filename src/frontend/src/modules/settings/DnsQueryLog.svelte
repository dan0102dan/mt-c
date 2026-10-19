<script lang="ts">
  import { onMount, untrack } from "svelte";

  import Pagination from "../../components/Pagination.svelte";
  import Button from "../../components/ui/Button.svelte";
  import Placeholder from "../../components/ui/Placeholder.svelte";
  import Select from "../../components/ui/Select.svelte";
  import Tooltip from "../../components/ui/Tooltip.svelte";
  import { Clear, Info, LayoutList, Refresh, Search, Sigma } from "../../components/ui/icons";
  import { token } from "../../data/auth.svelte";
  import { locales, t } from "../../data/locale.svelte";
  import { API_BASE, fetcher } from "../../utils/fetcher";
  import {
    dnsQueryType,
    filterDnsQueries,
    groupDnsQueries,
    type DnsQuery,
    type DnsQueryResponse,
  } from "./dns-query-data";
  import { applyDnsEvent, subscribeDnsQueries, type StreamState } from "./dns-query-stream";
  import streamTranslations from "./dns-query-stream.ru.json";
  import { settingsSection } from "./section-controls";

  type Props = { active?: boolean };
  let { active = true }: Props = $props();
  Object.assign(locales.ru, streamTranslations);

  const section = settingsSection();
  const pageSize = 30;
  let queries = $state<DnsQuery[]>([]);
  let capacity = $state(1000);
  let loading = $state(false);
  let clearing = $state(false);
  let failed = $state(false);
  let clearFailed = $state(false);
  let paused = $state(false);
  let visible = $state(false);
  let streamRevision = $state(0);
  let streamState = $state<StreamState>("connecting");
  let grouped = $state(false);
  let search = $state("");
  let device = $state("");
  let page = $state(1);
  let requestGeneration = 0;

  const clients = $derived([...new Set(queries.map((row) => row.client))].sort());
  const deviceOptions = $derived([
    { value: "", label: t("All devices") },
    ...(device && !clients.includes(device) ? [{ value: device, label: device }] : []),
    ...clients.map((client) => ({ value: client, label: client })),
  ]);
  const filtered = $derived(filterDnsQueries(queries, search, device));
  const groupedRows = $derived(groupDnsQueries(filtered));
  const total = $derived(grouped ? groupedRows.length : filtered.length);
  const pages = $derived(Math.max(1, Math.ceil(total / pageSize)));
  const safePage = $derived(Math.min(page, pages));
  const groupedPage = $derived(groupedRows.slice((safePage - 1) * pageSize, safePage * pageSize));
  const queryPage = $derived(filtered.slice((safePage - 1) * pageSize, safePage * pageSize));
  const filtersKey = $derived(`${search}\u0000${device}\u0000${grouped}`);
  // A healthy stream is the default, not a separate status banner.
  const statusLabel = $derived(
    paused
      ? t("Paused")
      : streamState === "live"
        ? ""
        : streamState === "reconnecting"
          ? t("Reconnecting...")
          : t("Connecting..."),
  );

  $effect(() => {
    filtersKey;
    page = 1;
  });

  onMount(() => {
    const updateVisibility = () => (visible = !document.hidden);
    updateVisibility();
    document.addEventListener("visibilitychange", updateVisibility);
    return () => {
      requestGeneration++;
      document.removeEventListener("visibilitychange", updateVisibility);
    };
  });

  // One subscription while visible; snapshots on reconnect replace state.
  $effect(() => {
    streamRevision;
    const accessToken = token.current;
    if (!active || !visible || paused || clearing) return;
    const generation = ++requestGeneration;
    const stop = untrack(() => subscribeDnsQueries({
      url: `${API_BASE}/system/dns/stream`,
      token: accessToken || "",
      onEvent: (event) => {
        if (generation !== requestGeneration) return;
        const next = applyDnsEvent({ queries, capacity }, event);
        queries = next.queries;
        capacity = next.capacity;
        failed = false;
        loading = false;
      },
      onState: (state) => {
        if (generation !== requestGeneration) return;
        streamState = state;
        loading = state === "connecting";
        failed = state === "reconnecting";
      },
      onUnauthorized: () => token.reset(),
    }));
    return () => {
      requestGeneration++;
      stop();
      loading = false;
    };
  });

  async function refresh() {
    if (loading || clearing) return;
    if (!paused) {
      streamRevision++;
      return;
    }
    // Explicit refresh while paused is still a single snapshot GET.
    const generation = ++requestGeneration;
    loading = true;
    try {
      const response = await fetcher.get<DnsQueryResponse>("/system/dns/queries", {
        silent: true,
        timeoutMs: 8000,
      });
      if (generation !== requestGeneration) return;
      const next = applyDnsEvent({ queries, capacity }, {
        event: "snapshot", data: JSON.stringify(response),
      });
      queries = next.queries;
      capacity = next.capacity;
      failed = false;
    } catch {
      if (generation === requestGeneration) failed = true;
    } finally {
      if (generation === requestGeneration) loading = false;
    }
  }

  async function clearHistory() {
    if (clearing || !confirm(t("Clear DNS query history?"))) return;
    // Invalidate the old stream before DELETE so older data cannot overwrite the clear.
    ++requestGeneration;
    loading = false;
    clearing = true;
    clearFailed = false;
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
      clearFailed = true;
    } finally {
      clearing = false;
    }
  }

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
  <!-- Direct section heading: SettingsPanel observes it for the shared sticky toolbar. -->
  <h2 id="dns-query-title">{t("DNS queries")}</h2>

  <div class="dns-panel">
    <div class="dns-toolbar">
      <label class="search-field">
        <span aria-hidden="true"><Search size={20} /></span>
        <span class="sr-only">{t("Search domain")}</span>
        <input
          data-page-search
          type="search"
          placeholder={t("Search domains...")}
          bind:value={search}
          autocomplete="off"
        />
      </label>
      <div class="device-field">
        <Select options={deviceOptions} bind:selected={device} compact ariaLabel={t("Device")} />
      </div>
      <div class="dns-actions">
        <Tooltip value={grouped ? t("Individual queries") : t("Group by domain")}>
          <Button
            small
            type="button"
            aria-label={grouped ? t("Individual queries") : t("Group by domain")}
            aria-pressed={grouped}
            onclick={() => (grouped = !grouped)}
          >
            {#if grouped}<LayoutList size={20} />{:else}<Sigma size={20} />{/if}
          </Button>
        </Tooltip>
        <Button small type="button" aria-pressed={paused} onclick={() => (paused = !paused)}>
          {paused ? t("Resume") : t("Pause")}
        </Button>
        <Tooltip value={t("Refresh")}>
          <Button
            small
            type="button"
            aria-label={t("Refresh")}
            disabled={loading || clearing}
            inactive={loading || clearing}
            onclick={() => void refresh()}
          ><Refresh size={20} /></Button>
        </Tooltip>
        <Tooltip value={t("Clear history")}>
          <Button
            small
            type="button"
            aria-label={t("Clear history")}
            disabled={clearing || !queries.length}
            inactive={clearing || !queries.length}
            onclick={() => void clearHistory()}
          ><Clear size={20} /></Button>
        </Tooltip>
      </div>
    </div>

    {#if failed}
      <div class="dns-message">
        <Placeholder variant="error" compact minHeight="auto">
          {t("Failed to load DNS queries")}
          {#snippet actions()}
            <Button disabled={loading || clearing} onclick={() => void refresh()}>{t("Retry")}</Button>
          {/snippet}
        </Placeholder>
      </div>
    {/if}
    {#if clearFailed}
      <div class="dns-message">
        <Placeholder variant="error" compact minHeight="auto">{t("Failed to clear DNS queries")}</Placeholder>
      </div>
    {/if}

    {#if !queries.length && loading}
      <div class="dns-message">
        <Placeholder variant="loading" compact minHeight="auto">{t("Loading DNS queries...")}</Placeholder>
      </div>
    {:else if !total}
      {#if !failed}
        <div class="dns-message">
          <Placeholder variant="empty" compact minHeight="auto">
            {queries.length ? t("No matching DNS queries") : t("No DNS queries captured yet")}
          </Placeholder>
        </div>
      {/if}
    {:else}
      <table class="dns-table" class:grouped aria-label={t("DNS query results")}>
        <colgroup>
          {#if grouped}
            <col class="domain-column" /><col class="number-column" />
            <col class="clients-column" /><col class="time-column" />
          {:else}
            <col class="time-column" /><col class="client-column" /><col class="domain-column" />
            <col class="type-column" /><col class="protocol-column" />
          {/if}
        </colgroup>
        <thead>
          {#if grouped}
            <tr>
              <th scope="col">{t("Domain")}</th><th scope="col">{t("Requests")}</th>
              <th scope="col">{t("Clients")}</th><th scope="col">{t("Last request")}</th>
            </tr>
          {:else}
            <tr>
              <th scope="col">{t("Time")}</th><th scope="col">{t("Client")}</th>
              <th scope="col">{t("Domain")}</th><th scope="col">{t("Type")}</th>
              <th scope="col">{t("Protocol")}</th>
            </tr>
          {/if}
        </thead>
        <tbody>
          {#if grouped}
            {#each groupedPage as item (item.domain)}
              <tr>
                <td class="domain"><span class="cell-label" aria-hidden="true">{t("Domain")}</span><span class="cell-value">{item.domain}</span></td>
                <td class="number"><span class="cell-label" aria-hidden="true">{t("Requests")}</span><span class="cell-value">{item.count}</span></td>
                <td><span class="cell-label" aria-hidden="true">{t("Clients")}</span><span class="cell-value">{item.clients.join(", ")}</span></td>
                <td class="timestamp"><span class="cell-label" aria-hidden="true">{t("Last request")}</span><span class="cell-value">{displayTime(item.lastTime)}</span></td>
              </tr>
            {/each}
          {:else}
            {#each queryPage as row (row.id)}
              <tr>
                <td class="timestamp"><span class="cell-label" aria-hidden="true">{t("Time")}</span><span class="cell-value">{displayTime(row.time_ms)}</span></td>
                <td><span class="cell-label" aria-hidden="true">{t("Client")}</span><span class="cell-value">{row.client}</span></td>
                <td class="domain"><span class="cell-label" aria-hidden="true">{t("Domain")}</span><span class="cell-value">{row.domain}</span></td>
                <td><span class="cell-label" aria-hidden="true">{t("Type")}</span><span class="cell-value">{dnsQueryType(row.qtype)}</span></td>
                <td><span class="cell-label" aria-hidden="true">{t("Protocol")}</span><span class="cell-value">{row.protocol.toUpperCase()}</span></td>
              </tr>
            {/each}
          {/if}
        </tbody>
      </table>
    {/if}

    <div class="dns-summary">
      <span class="count">{t("Requests")}: {queries.length} / {capacity}</span>
      <span class="connection-state" role="status">{statusLabel}</span>
      <Tooltip value={t("Requests captured by the DNS proxy. Cached or encrypted DNS traffic is not visible.")}>
        <Button small type="button" aria-label={t("Requests captured by the DNS proxy. Cached or encrypted DNS traffic is not visible.")}><Info size={16} /></Button>
      </Tooltip>
    </div>
    <Pagination totalItems={total} {pageSize} currentPage={safePage} onPageChange={(value) => (page = value)} />
  </div>
</section>

<style>
  .dns-query-log {
    min-width: 0;
  }
  h2 {
    margin: 0 0 1rem;
    color: var(--text-2);
    font: 600 1.3rem / 1.4 var(--font);
  }
  /* Same panel and alternating rows as ProfilePanel and the rule lists. */
  .dns-panel {
    min-width: 0;
    background-color: var(--bg-medium);
    border: 1px solid var(--bg-light-extra);
    border-radius: 0.5rem;
  }
  .dns-toolbar {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: 0.5rem;
    padding: 0.5rem;
    background-color: var(--bg-light);
    border-radius: 0.5rem 0.5rem 0 0;
  }
  .search-field {
    display: flex;
    align-items: center;
    gap: 0.4rem;
    flex: 1 1 12rem;
    min-width: 0;
    padding: 0.3rem 0.5rem;
    background: var(--bg-medium);
    border: 1px solid var(--bg-light-extra);
    border-radius: 0.5rem;
    color: var(--text-2);
  }
  .search-field > span:first-child {
    display: inline-flex;
    flex-shrink: 0;
  }
  .search-field:focus-within {
    border-color: var(--accent);
  }
  input {
    box-sizing: border-box;
    width: 100%;
    min-width: 0;
    padding: 0;
    border: none;
    outline: none;
    background: transparent;
    color: var(--text);
    font: 400 1rem / 1.5 var(--font);
  }
  input::placeholder {
    color: var(--text-2);
  }
  .device-field {
    min-width: 0;
    max-width: 100%;
  }
  .device-field :global(.select-wrap),
  .device-field :global([data-select-trigger]) {
    min-width: 0;
    max-width: 100%;
  }
  .dns-actions {
    display: flex;
    align-items: center;
    gap: 0.35rem;
    margin-left: auto;
    flex-shrink: 0;
  }
  .dns-actions :global(button[aria-pressed="true"]) {
    color: var(--text);
    background-color: var(--bg-dark);
    border-color: var(--bg-light-extra);
  }
  .dns-panel :global(button:focus-visible) {
    outline: 2px solid var(--accent);
    outline-offset: 2px;
  }
  .dns-message {
    padding: 1rem 0.5rem;
  }
  .dns-table {
    width: 100%;
    border-collapse: collapse;
    table-layout: fixed;
    text-align: left;
    color: var(--text);
    font: 400 1rem / 1.5 var(--font);
  }
  .time-column, .client-column {
    width: 21%;
  }
  .type-column, .protocol-column {
    width: 9%;
  }
  .number-column {
    width: 12%;
  }
  .clients-column {
    width: 31%;
  }
  th {
    padding: 0.6rem 0.5rem 0.2rem;
    border-bottom: 1px solid var(--bg-light-extra);
    color: var(--text-2);
    font-size: 0.9rem;
    font-weight: 400;
    overflow-wrap: anywhere;
  }
  td {
    height: 2rem;
    padding: 0.1rem 0.5rem;
    vertical-align: middle;
  }
  tbody tr:nth-child(even) {
    background-color: var(--bg-light);
  }
  .cell-value {
    min-width: 0;
    overflow-wrap: anywhere;
  }
  .cell-label {
    display: none;
  }
  .timestamp, .number {
    color: var(--text-2);
    font-size: 0.9rem;
    font-variant-numeric: tabular-nums;
  }
  .dns-summary {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: 0.5rem;
    padding: 0.35rem 0.5rem;
    color: var(--text-2);
    font-size: 0.85rem;
  }
  .count {
    margin-right: auto;
    font-variant-numeric: tabular-nums;
  }
  .connection-state:empty {
    display: none;
  }
  .sr-only {
    position: absolute;
    width: 1px;
    height: 1px;
    padding: 0;
    margin: -1px;
    overflow: hidden;
    clip-path: inset(50%);
    white-space: nowrap;
    border: 0;
  }
  @media (max-width: 700px) {
    .search-field {
      flex-basis: 100%;
    }
    .device-field {
      flex: 1 1 8rem;
    }
    .dns-table, .dns-table tbody {
      display: block;
    }
    .dns-table colgroup {
      display: none;
    }
    .dns-table thead {
      position: absolute;
      width: 1px;
      height: 1px;
      overflow: hidden;
      clip-path: inset(50%);
      white-space: nowrap;
    }
    .dns-table tbody tr {
      display: grid;
      grid-template-columns: minmax(0, 1fr);
      gap: 0.25rem;
      padding: 0.5rem 0.35rem 0.45rem;
    }
    .dns-table td {
      display: grid;
      grid-template-columns: 5.6rem minmax(0, 1fr);
      align-items: baseline;
      gap: 0.35rem;
      min-width: 0;
      height: auto;
      padding: 0.05rem 0;
    }
    .dns-table td.domain {
      order: -1;
    }
    .cell-label {
      display: block;
      text-align: right;
      color: var(--text-2);
      font: 400 0.9rem / 1.5 var(--font);
      overflow-wrap: anywhere;
    }
  }
</style>
