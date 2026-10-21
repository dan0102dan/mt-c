<script lang="ts">
  import { Popover } from "bits-ui";
  import { onMount, untrack } from "svelte";

  import Pagination from "../../components/Pagination.svelte";
  import Button from "../../components/ui/Button.svelte";
  import GenericDialog from "../../components/ui/GenericDialog.svelte";
  import Placeholder from "../../components/ui/Placeholder.svelte";
  import Select from "../../components/ui/Select.svelte";
  import Tooltip from "../../components/ui/Tooltip.svelte";
  import { token } from "../../data/auth.svelte";
  import { locales, t } from "../../data/locale.svelte";

  import {
    Clear,
    LayoutList,
    ListOrdered,
    Pause,
    Play,
    Search,
    SortAsc,
    SortCountAsc,
    SortCountDesc,
    SortDesc,
    SortNeutral,
  } from "../../components/ui/icons";
  import { API_BASE, fetcher } from "../../utils/fetcher";
  import { parseCaptureState, type CaptureState } from "./dns-capture";
  import captureTranslations from "./dns-capture.ru.json";
  import { createDnsHistory } from "./dns-history";
  import { groupDnsBursts } from "./dns-query-bursts";
  import { dnsQueryType, filterDnsQueries, groupDnsQueries, type DnsQuery } from "./dns-query-data";
  import {
    nextDnsTimeSort,
    sortDnsByCount,
    sortDnsByTime,
    type DnsTimeSort,
  } from "./dns-query-sort";
  import { subscribeDnsQueries, type StreamState } from "./dns-query-stream";
  import streamTranslations from "./dns-query-stream.ru.json";
  import { settingsSection } from "./section-controls";

  let { active = true }: { active?: boolean } = $props();
  Object.assign(locales.ru, streamTranslations, captureTranslations);
  const section = settingsSection();
  const pageSize = 30;
  let queries = $state.raw<DnsQuery[]>([]);
  let clientNames = $state.raw<ReadonlyMap<string, string>>(new Map());
  // Empty placeholder until the server advertises its byte-derived bound.
  let capacity = $state(1);
  let capture = $state.raw<CaptureState | null>(null);
  let changing = $state(false);
  let confirmation = $state<"start" | "stop" | "clear" | null>(null);
  const confirmationLabel = $derived(
    confirmation === "clear"
      ? t("Clear history")
      : confirmation === "stop"
        ? t("Stop recording")
        : t("Start recording"),
  );
  const confirmationMessage = $derived(
    confirmation === "clear"
      ? t("Clear DNS query history?")
      : confirmation === "stop"
        ? t("DNS query history will be deleted for all viewers, and its memory will be freed.")
        : t(
            "History storage will use up to 1 MiB of RAM, with additional memory for streaming and the interface. Recording continues after closing this page until stopped or the service restarts.",
          ),
  );

  function confirmAction() {
    const action = confirmation;
    confirmation = null;
    if (action === "clear") void clearHistory();
    else if (action) void setRecording(action === "start");
  }

  let captureFailed = $state(false);
  let loading = $state(false);
  let clearing = $state(false);
  let failed = $state(false);
  let clearFailed = $state(false);
  let paused = $state(false);
  let visible = $state(false);
  let streamState = $state<StreamState>("connecting");
  let grouped = $state(false);
  let timeSort = $state<DnsTimeSort>("desc");
  let groupedSort = $state<"time" | "count">("time");
  let countSort = $state<DnsTimeSort>("desc");
  let search = $state("");
  let device = $state("");
  let page = $state(1);
  let requestGeneration = 0;
  let mounted = false;

  const clients = $derived([...new Set(queries.map((row) => row.client))].sort());
  const deviceOptions = $derived([
    { value: "", label: t("All devices") },
    ...(device && !clients.includes(device) ? [{ value: device, label: device }] : []),
    ...clients.map((client) => ({
      value: client,
      label: clientNames.get(client) ?? client,
      description: clientNames.has(client) ? client : undefined,
    })),
  ]);
  const filtered = $derived(filterDnsQueries(queries, search, device));
  const groupedRows = $derived(
    groupedSort === "count"
      ? sortDnsByCount(groupDnsQueries(filtered), countSort)
      : sortDnsByTime(groupDnsQueries(filtered), timeSort, (row) => row.lastTime),
  );
  const timeline = $derived(
    sortDnsByTime(groupDnsBursts(filtered), timeSort, (row) => row.time_ms),
  );
  const total = $derived(grouped ? groupedRows.length : timeline.length);
  const pages = $derived(Math.max(1, Math.ceil(total / pageSize)));
  const safePage = $derived(Math.min(page, pages));
  const groupedPage = $derived(groupedRows.slice((safePage - 1) * pageSize, safePage * pageSize));
  const queryPage = $derived(timeline.slice((safePage - 1) * pageSize, safePage * pageSize));
  const filtersKey = $derived(
    `${search}\u0000${device}\u0000${grouped}\u0000${timeSort}\u0000${groupedSort}\u0000${countSort}`,
  );
  const timeSortActive = $derived(!grouped || groupedSort === "time");
  const timeSortAria = $derived(
    timeSortActive ? (timeSort === "asc" ? "ascending" : "descending") : "none",
  );
  const countSortAria = $derived(
    groupedSort === "count" ? (countSort === "asc" ? "ascending" : "descending") : "none",
  );
  const countSortAction = $derived(
    groupedSort !== "count" || countSort === "asc"
      ? "Most requests first"
      : "Fewest requests first",
  );
  function sortByTime() {
    if (timeSortActive) timeSort = nextDnsTimeSort(timeSort);
    groupedSort = "time";
  }
  function sortByCount() {
    countSort = groupedSort === "count" ? nextDnsTimeSort(countSort) : "desc";
    groupedSort = "count";
  }
  const timeSortAction = $derived(
    (timeSort === "desc") === timeSortActive ? "Oldest first" : "Newest first",
  );
  const statusLabel = $derived(
    paused
      ? t("Viewing is paused; recording continues.")
      : streamState === "live" || streamState === "unavailable"
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
    mounted = true;
    const updateVisibility = () => (visible = !document.hidden);
    updateVisibility();
    document.addEventListener("visibilitychange", updateVisibility);
    return () => {
      mounted = false;
      requestGeneration++;
      document.removeEventListener("visibilitychange", updateVisibility);
    };
  });

  // Status subscriptions are read-only: GET/SSE never enables collection.
  $effect(() => {
    const accessToken = token.current;
    if (!active || !visible || paused || clearing || changing) return;
    const generation = ++requestGeneration;
    const history = untrack(() => createDnsHistory({ queries, capacity, capture, clientNames }));
    let frame = 0;
    const publish = () => {
      frame = 0;
      if (generation !== requestGeneration) return;
      const view = history.view();
      queries = view.queries;
      capacity = view.capacity;
      capture = view.capture;
      clientNames = view.clientNames;
    };
    const stop = untrack(() =>
      subscribeDnsQueries({
        url: `${API_BASE}/system/dns/stream`,
        token: accessToken || "",
        onEvent: (event) => {
          if (generation !== requestGeneration) return;
          history.ingest(event);
          if (!frame) frame = requestAnimationFrame(publish);
        },
        onState: (state) => {
          if (generation !== requestGeneration) return;
          streamState = state;
          loading = state === "connecting";
          failed = state === "reconnecting" || state === "unavailable";
        },
        onUnauthorized: () => token.reset(),
      }),
    );
    return () => {
      requestGeneration++;
      cancelAnimationFrame(frame);
      stop();
      loading = false;
    };
  });

  async function setRecording(enabled: boolean) {
    if (changing || clearing) return;
    requestGeneration++;
    changing = true;
    captureFailed = false;
    try {
      const result = await fetcher<unknown>("/system/dns/capture", {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ enabled }),
        silent: true,
        timeoutMs: 8000,
      });
      if (!mounted) return;
      capture = parseCaptureState(result);
      capacity = capture.capacity;
      queries = [];
      page = 1;
      paused = false;
    } catch {
      if (mounted) captureFailed = true;
    } finally {
      if (mounted) changing = false;
    }
  }
  async function clearHistory() {
    if (clearing || changing) return;
    requestGeneration++;
    loading = false;
    clearing = true;
    clearFailed = false;
    try {
      const result = await fetcher<Partial<CaptureState> & { cleared: boolean }>(
        "/system/dns/queries",
        { method: "DELETE", silent: true, timeoutMs: 8000 },
      );
      if (!mounted) return;
      queries = [];
      page = 1;
      failed = false;
      if (typeof result.enabled === "boolean") capture = parseCaptureState(result);
    } catch {
      if (mounted) clearFailed = true;
    } finally {
      if (mounted) clearing = false;
    }
  }
  const displayTime = (timestamp: number) =>
    new Date(timestamp).toLocaleTimeString(undefined, { hour12: false });
  const displayDate = (timestamp: number) =>
    new Date(timestamp).toLocaleDateString("ru-RU", {
      day: "2-digit",
      month: "2-digit",
      year: "numeric",
    });
  const exactTime = (timestamp: number) =>
    `${new Date(timestamp).toLocaleDateString()}, ${displayTime(timestamp)}.${String(new Date(timestamp).getMilliseconds()).padStart(3, "0")}`;
  const queryLabel = (qtype: number) =>
    qtype === 1 ? "IPv4" : qtype === 28 ? "IPv6" : qtype === 65 ? "HTTPS" : dnsQueryType(qtype);
</script>

{#snippet timestamp(value: number)}
  <span class="cell-value timestamp-value" title={exactTime(value)}>
    <span>{displayTime(value)}</span>
    <span>{displayDate(value)}</span>
  </span>
{/snippet}

{#snippet timeSortButton(label: string)}
  <Button small type="button" class="time-sort" title={t(timeSortAction)} onclick={sortByTime}>
    {t(label)}
    <span class="sort-icon" aria-hidden="true">
      {#if !timeSortActive}<SortNeutral size={16} />{:else if timeSort === "desc"}
        <SortAsc size={16} />
      {:else}
        <SortDesc size={16} />
      {/if}
    </span>
  </Button>
{/snippet}

{#snippet clientIdentity(address: string)}
  <span class="client-identity">
    {#if clientNames.get(address)}<span class="client-name" title={clientNames.get(address)}
        >{clientNames.get(address)}</span
      ><span class="client-address" title={address}>{address}</span>
    {:else}<span class="client-name" title={address}>{address}</span>{/if}
  </span>
{/snippet}

<GenericDialog
  open={confirmation !== null}
  title={confirmation === "start"
    ? t("Enable DNS recording?")
    : confirmation === "stop"
      ? `${t("Stop recording")}?`
      : confirmationLabel}
  maxWidth={480}
  on:close={() => (confirmation = null)}
  on:submit={confirmAction}
>
  <p slot="body" class="confirmation-message">{confirmationMessage}</p>
  <div slot="actions" class="confirmation-actions">
    <Button onclick={() => (confirmation = null)}>{t("Cancel")}</Button>
    <Button class="accent" onclick={confirmAction}>{confirmationLabel}</Button>
  </div>
</GenericDialog>

<section
  class="dns-query-log"
  aria-labelledby="dns-query-title"
  aria-busy={loading || clearing || changing}
  use:section={() => ({
    title: t("DNS queries"),
    dirty: false,
    canSave: false,
    onSave: async () => {},
    saveLabel: t("Save Changes"),
  })}
>
  <h2 id="dns-query-title">{t("DNS queries")}</h2>
  <div class="dns-panel">
    <div class="capture-toolbar">
      <Button
        small
        type="button"
        disabled={changing || clearing || streamState === "unavailable"}
        onclick={() => (confirmation = capture?.enabled ? "stop" : "start")}
      >
        {capture?.enabled ? t("Stop recording") : t("Start recording")}
      </Button>
      {#if capture?.enabled}
        {#if capture.dropped}<span class="dropped-count"
            >{t("Queries not recorded")}: {capture.dropped}</span
          >{/if}
      {/if}
    </div>
    {#if captureFailed}
      <div class="dns-message">
        <Placeholder variant="error" compact minHeight="auto"
          >{t("Failed to change DNS recording")}</Placeholder
        >
      </div>
    {/if}
    {#if capture && !capture.enabled}
      <div class="dns-message">
        <Placeholder variant="empty" compact minHeight="auto">
          <strong>{t("DNS recording is off")}</strong><br />{t(
            "No DNS history is collected until you start recording.",
          )}
        </Placeholder>
      </div>
    {:else}
      <div class="dns-toolbar">
        <label class="search-field"
          ><span aria-hidden="true"><Search size={20} /></span>
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
          <Select options={deviceOptions} bind:selected={device} ariaLabel={t("Device")} />
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
              {#if grouped}<LayoutList size={20} />{:else}<ListOrdered size={20} />{/if}
            </Button>
          </Tooltip>
          <Tooltip value={paused ? t("Resume") : t("Pause")}>
            <Button
              small
              type="button"
              aria-label={paused ? t("Resume") : t("Pause")}
              aria-pressed={paused}
              onclick={() => (paused = !paused)}
            >
              {#if paused}<Play size={20} aria-hidden="true" />{:else}<Pause
                  size={20}
                  aria-hidden="true"
                />{/if}
            </Button>
          </Tooltip>
          <Tooltip value={t("Clear history")}>
            <Button
              small
              type="button"
              aria-label={t("Clear history")}
              disabled={clearing || changing || !queries.length}
              inactive={clearing || changing || !queries.length}
              onclick={() => (confirmation = "clear")}><Clear size={20} /></Button
            >
          </Tooltip>
        </div>
      </div>
      {#if clearFailed}
        <div class="dns-message">
          <Placeholder variant="error" compact minHeight="auto"
            >{t("Failed to clear DNS queries")}</Placeholder
          >
        </div>
      {/if}
      {#if !queries.length && loading}
        <div class="dns-message">
          <Placeholder variant="loading" compact minHeight="auto"
            >{t("Loading DNS queries...")}</Placeholder
          >
        </div>
      {:else if !total}
        {#if !failed}<div class="dns-message">
            <Placeholder variant="empty" compact minHeight="auto">
              {queries.length ? t("No matching DNS queries") : t("No DNS queries captured yet")}
            </Placeholder>
          </div>{/if}
      {:else}
        <table class="dns-table" class:grouped aria-label={t("DNS query results")}>
          <colgroup>
            {#if grouped}<col class="domain-column" /><col class="number-column" /><col
                class="clients-column"
              /><col class="time-column" />
            {:else}<col class="time-column" /><col class="client-column" /><col
                class="domain-column"
              /><col class="type-column" /><col class="protocol-column" />{/if}
          </colgroup>
          <thead
            >{#if grouped}<tr
                ><th scope="col">{t("Domain")}</th><th
                  scope="col"
                  class="count-sort-header"
                  aria-sort={countSortAria}
                >
                  <Button
                    small
                    type="button"
                    class="time-sort"
                    title={t(countSortAction)}
                    onclick={sortByCount}
                  >
                    {t("Requests")}
                    <span class="sort-icon" aria-hidden="true">
                      {#if groupedSort !== "count"}<SortNeutral size={16} />
                      {:else if countSort === "desc"}<SortCountDesc size={16} />
                      {:else}<SortCountAsc size={16} />{/if}
                    </span>
                  </Button>
                </th><th scope="col">{t("Clients")}</th><th
                  scope="col"
                  class="time-sort-header"
                  aria-sort={timeSortAria}>{@render timeSortButton("Time")}</th
                ></tr
              >
            {:else}<tr
                ><th scope="col" class="time-sort-header" aria-sort={timeSortAria}
                  >{@render timeSortButton("Time")}</th
                ><th scope="col">{t("Client")}</th><th scope="col">{t("Domain")}</th><th scope="col"
                  >{t("Type")}</th
                ><th scope="col">{t("Protocol")}</th></tr
              >{/if}</thead
          >
          <tbody>
            {#if grouped}
              {#each groupedPage as item (item.domain)}<tr>
                  <td class="domain"
                    ><span class="cell-label" aria-hidden="true">{t("Domain")}</span><span
                      class="cell-value">{item.domain}</span
                    ></td
                  >
                  <td class="number"
                    ><span class="cell-label" aria-hidden="true">{t("Requests")}</span><span
                      class="cell-value">{item.count}</span
                    ></td
                  >
                  <td
                    ><span class="cell-label" aria-hidden="true">{t("Clients")}</span><span
                      class="cell-value client-identities"
                      >{#each item.clients as client (client)}{@render clientIdentity(
                          client,
                        )}{/each}</span
                    ></td
                  >
                  <td class="timestamp"
                    ><span class="cell-label" aria-hidden="true">{t("Time")}</span
                    >{@render timestamp(item.lastTime)}</td
                  >
                </tr>{/each}
            {:else}
              {#each queryPage as row (row.id)}<tr>
                  <td class="timestamp"
                    ><span class="cell-label" aria-hidden="true">{t("Time")}</span
                    >{@render timestamp(row.time_ms)}</td
                  >
                  <td
                    ><span class="cell-label" aria-hidden="true">{t("Client")}</span><span
                      class="cell-value">{@render clientIdentity(row.client)}</span
                    ></td
                  >
                  <td class="domain"
                    ><span class="cell-label" aria-hidden="true">{t("Domain")}</span><span
                      class="cell-value"
                      title={row.domain}>{row.domain}</span
                    ></td
                  >
                  <td
                    ><span class="cell-label" aria-hidden="true">{t("Type")}</span>
                    <div class="cell-value">
                      <Popover.Root>
                        <Popover.Trigger class="query-details"
                          >{row.queries
                            .map((query) => queryLabel(query.qtype))
                            .join(" · ")}{#if row.queries.length > 1}
                            {" "}
                            <span class="number">({row.queries.length})</span>{/if}</Popover.Trigger
                        >
                        <Popover.Portal>
                          <Popover.Content
                            role="dialog"
                            class="dns-query-popover"
                            sideOffset={6}
                            collisionPadding={12}
                            aria-label={t("DNS query details")}
                          >
                            {#each row.queries as query (query.id)}<span class="raw-query"
                                >{exactTime(query.time_ms)} — {dnsQueryType(query.qtype)} (TYPE {query.qtype})</span
                              >{/each}
                          </Popover.Content>
                        </Popover.Portal>
                      </Popover.Root>
                    </div></td
                  >
                  <td
                    ><span class="cell-label" aria-hidden="true">{t("Protocol")}</span><span
                      class="cell-value">{row.protocol.toUpperCase()}</span
                    ></td
                  >
                </tr>{/each}
            {/if}
          </tbody>
        </table>
      {/if}
      <div class="dns-summary">
        <span class="count">{t("Stored requests")}: {queries.length}</span>
        <span class="connection-state" role="status">{statusLabel}</span>
      </div>
      <Pagination
        totalItems={total}
        {pageSize}
        currentPage={safePage}
        onPageChange={(value) => (page = value)}
      />
    {/if}
    {#if failed}<div class="dns-message">
        <Placeholder variant="error" compact minHeight="auto">
          {streamState === "unavailable"
            ? t("DNS history unavailable; DNS forwarding is still running.")
            : t("Failed to load DNS queries")}
        </Placeholder>
      </div>{/if}
  </div>
</section>

<style>
  .confirmation-message {
    margin: 0;
    color: var(--text-2);
    font: 400 1rem / 1.5 var(--font);
  }
  .confirmation-actions {
    display: flex;
    flex-wrap: wrap;
    justify-content: flex-end;
    gap: 0.5rem;
    margin-top: 0.5rem;
  }

  .dns-query-log {
    min-width: 0;
  }
  .client-identity,
  .client-identities {
    display: flex;
    flex-direction: column;
    min-width: 0;
    gap: 0.1rem;
    overflow-wrap: anywhere;
  }
  .client-identities {
    gap: 0.4rem;
  }
  .client-address {
    color: var(--text-2);
    font-size: 0.8rem;
  }
  .client-name,
  .client-address {
    overflow: hidden;
    white-space: nowrap;
    text-overflow: ellipsis;
  }
  h2 {
    margin: 0 0 1rem;
    color: var(--text-2);
    font: 600 1.3rem / 1.4 var(--font);
  }
  .dns-panel {
    min-width: 0;
    background-color: var(--bg-medium);
    border: 1px solid var(--bg-light-extra);
    border-radius: 0.5rem;
  }
  .capture-toolbar,
  .dns-toolbar {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: 0.5rem;
    padding: 0.5rem;
    background-color: var(--bg-light);
  }
  .capture-toolbar {
    border-radius: 0.5rem 0.5rem 0 0;
    border-bottom: 1px solid var(--bg-light-extra);
  }
  .dropped-count {
    margin-left: auto;
    color: var(--text-2);
    font-size: 0.85rem;
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
    flex-wrap: wrap;
    gap: 0.35rem;
    margin-left: auto;
    min-width: 0;
  }
  .dns-actions :global(button[aria-pressed="true"]) {
    color: var(--text);
    background-color: var(--bg-dark);
    border-color: var(--bg-light-extra);
  }
  .dns-panel :global(button:focus-visible),
  :global(.query-details:focus-visible) {
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
  .time-column {
    width: 5.5rem;
  }
  .client-column {
    width: 24%;
  }
  .type-column {
    width: 8.5rem;
  }
  .protocol-column {
    width: 5.5rem;
  }
  .number-column {
    width: 7rem;
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
  .dns-table :global(button.time-sort) {
    width: 100%;
    padding: 0;
    border: 0;
    border-radius: 0;
    background: transparent;
    color: inherit;
    font: inherit;
    justify-content: flex-start;
    text-align: left;
    user-select: none;
    transition: color 0.12s ease;
  }
  .dns-table :global(button.time-sort:hover) {
    color: var(--text);
  }
  .sort-icon {
    margin-left: 0.4rem;
    display: flex;
    align-items: center;
    flex-shrink: 0;
    color: var(--text-2);
  }
  td {
    height: 2rem;
    padding: 0.1rem 0.5rem;
    vertical-align: middle;
  }
  .dns-table:not(.grouped) td {
    height: 3rem;
  }
  .dns-table:not(.grouped) .domain .cell-value,
  .dns-table:not(.grouped) :global(.query-details) {
    display: -webkit-box;
    -webkit-box-orient: vertical;
    -webkit-line-clamp: 2;
    line-clamp: 2;
    overflow: hidden;
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
  .timestamp-value {
    display: inline-flex;
    flex-direction: column;
  }
  .timestamp {
    color: var(--text-2);
    font-size: 0.8rem;
    font-variant-numeric: tabular-nums;
    white-space: nowrap;
  }
  .number {
    color: var(--text-2);
    font-size: 0.9rem;
    font-variant-numeric: tabular-nums;
  }
  :global(.query-details) {
    display: block;
    min-width: 0;
    padding: 0;
    border: 0;
    background: transparent;
    color: inherit;
    font: 400 0.85rem / 1.5 var(--font);
    text-align: left;
    overflow-wrap: anywhere;
    cursor: pointer;
    text-decoration: underline dotted;
    text-underline-offset: 0.2rem;
  }
  :global(.dns-query-popover) {
    z-index: 20;
    box-sizing: border-box;
    max-width: min(26rem, calc(100vw - 24px));
    padding: 0.65rem;
    border: 1px solid var(--bg-light-extra);
    border-radius: 0.5rem;
    background: var(--bg-dark-extra);
    color: var(--text-2);
    font: 400 0.8rem / 1.5 var(--font);
    box-shadow: var(--shadow-popover);
    overflow-wrap: anywhere;
  }
  :global(.dns-query-popover .raw-query) {
    display: block;
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
    .dns-actions {
      flex: 1 1 100%;
    }
    .dns-table,
    .dns-table tbody {
      display: block;
    }
    .dns-table colgroup {
      display: none;
    }
    /* Keep both sort controls reachable when rows become mobile cards. */
    .dns-table thead,
    .dns-table thead tr {
      display: flex;
      flex-wrap: wrap;
      gap: 0.5rem;
    }
    .dns-table .time-sort-header,
    .dns-table .count-sort-header {
      display: block;
      padding: 0.5rem 0.35rem;
    }
    .dns-table th:not(.time-sort-header):not(.count-sort-header) {
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
    .dns-table td,
    .dns-table:not(.grouped) td {
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
