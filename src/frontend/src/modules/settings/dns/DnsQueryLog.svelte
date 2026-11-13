<script lang="ts">
  import { Popover } from "bits-ui";
  import { onMount, untrack } from "svelte";

  import Pagination from "../../../components/Pagination.svelte";
  import Button from "../../../components/ui/Button.svelte";
  import GenericDialog from "../../../components/ui/GenericDialog.svelte";
  import PanelSurface from "../../../components/ui/PanelSurface.svelte";
  import Placeholder from "../../../components/ui/Placeholder.svelte";
  import SearchField from "../../../components/ui/SearchField.svelte";
  import SectionDescription from "../../../components/ui/SectionDescription.svelte";
  import Select from "../../../components/ui/Select.svelte";
  import Tooltip from "../../../components/ui/Tooltip.svelte";
  import { requireAuthentication, token } from "../../../data/auth.svelte";
  import { locales, t } from "../../../data/locale.svelte";

  import {
    Clear,
    LayoutList,
    ListOrdered,
    Pause,
    Play,
    SortAsc,
    SortCountAsc,
    SortCountDesc,
    SortDesc,
    SortNeutral,
  } from "../../../components/ui/icons";
  import { API_BASE, fetcher } from "../../../utils/fetcher";
  import { settingsSection } from "../section-controls";
  import {
    dnsQueryType,
    filterDnsQueries,
    groupDnsBursts,
    groupDnsQueries,
    nextDnsTimeSort,
    sortDnsByCount,
    sortDnsByTime,
    type DnsQuery,
    type DnsTimeSort,
  } from "./data";
  import { createDnsHistory, parseCaptureState, type CaptureState } from "./history";
  import translations from "./ru.json";
  import { subscribeDnsQueries, type StreamState } from "./stream";

  let { active = true }: { active?: boolean } = $props();
  Object.assign(locales.ru, translations);
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
  // A capture/clear failure is more actionable than a transport failure. Keep
  // one status message while any confirmed query rows remain available below.
  const errorMessage = $derived(
    captureFailed
      ? t("Failed to change DNS recording")
      : clearFailed
        ? t("Failed to clear DNS queries")
        : failed
          ? streamState === "unavailable"
            ? t("DNS history unavailable; DNS forwarding is still running.")
            : t("Failed to load DNS queries")
          : "",
  );
  const statusLabel = $derived(paused ? t("Viewing is paused; recording continues.") : "");
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
        onUnauthorized: requireAuthentication,
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
    clearFailed = false;
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
    captureFailed = false;
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
  id="dns"
  class="dns-query-log"
  aria-labelledby="dns-query-title"
  aria-busy={loading || clearing || changing}
  use:section={() => ({
    dirty: false,
    canSave: false,
    onSave: async () => {},
    saveLabel: t("Save Changes"),
    contextAction: {
      label: capture?.enabled ? t("Stop recording") : t("Start recording"),
      icon: capture?.enabled ? "stop" : "record",
      pressed: capture?.enabled ?? false,
      disabled: changing || clearing || streamState === "unavailable",
      onclick: () => (confirmation = capture?.enabled ? "stop" : "start"),
    },
  })}
>
  <h2 id="dns-query-title" class="sr-only">{t("DNS queries")}</h2>
  <SectionDescription>
    {t(
      "Record and inspect DNS queries from your devices. Use search and filters to find requests.",
    )}
  </SectionDescription>
  <PanelSurface class="dns-panel">
    {#snippet header()}
      {#if !capture || capture.enabled}
        <div class="dns-toolbar">
          <SearchField
            class="dns-search"
            label={t("Search domain")}
            placeholder={t("Search domains...")}
            bind:value={search}
          />
          <div class="device-field">
            <Select options={deviceOptions} bind:selected={device} ariaLabel={t("Device")} />
          </div>
          <div class="dns-actions">
            <Tooltip value={grouped ? t("Individual queries") : t("Group by domain")}>
              <Button
                type="button"
                aria-label={grouped ? t("Individual queries") : t("Group by domain")}
                aria-pressed={grouped}
                onclick={() => (grouped = !grouped)}
              >
                {#if grouped}<LayoutList size={22} />{:else}<ListOrdered size={22} />{/if}
              </Button>
            </Tooltip>
            <Tooltip value={paused ? t("Resume") : t("Pause")}>
              <Button
                type="button"
                aria-label={paused ? t("Resume") : t("Pause")}
                aria-pressed={paused}
                onclick={() => (paused = !paused)}
              >
                {#if paused}<Play size={22} aria-hidden="true" />{:else}<Pause
                    size={22}
                    aria-hidden="true"
                  />{/if}
              </Button>
            </Tooltip>
            <Tooltip value={t("Clear history")}>
              <Button
                type="button"
                aria-label={t("Clear history")}
                disabled={clearing || changing || !queries.length}
                inactive={clearing || changing || !queries.length}
                onclick={() => (confirmation = "clear")}><Clear size={22} /></Button
              >
            </Tooltip>
          </div>
          {#if capture?.enabled && capture.dropped}
            <span class="dropped-count">{t("Queries not recorded")}: {capture.dropped}</span>
          {/if}
        </div>
      {/if}
    {/snippet}
    {#if errorMessage}
      <div class="dns-message">
        <Placeholder variant="error" embedded compact minHeight="auto">{errorMessage}</Placeholder>
      </div>
    {/if}
    {#if capture && !capture.enabled}
      {#if !errorMessage}<div class="dns-message">
          <Placeholder
            variant="empty"
            embedded
            minHeight="auto"
            subtitle={t("No DNS history is collected until you start recording.")}
          >
            {t("DNS recording is off")}
          </Placeholder>
        </div>{/if}
    {:else}
      {#if !queries.length && loading && !errorMessage}
        <div class="dns-message">
          <Placeholder variant="loading" embedded compact minHeight="auto"
            >{t("Loading DNS queries...")}</Placeholder
          >
        </div>
      {:else if !total}
        {#if !errorMessage}<div class="dns-message">
            <Placeholder variant="empty" embedded compact minHeight="auto">
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
                      class="cell-value"
                      title={item.domain}>{item.domain}</span
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
                    >
                      {#each item.clients.slice(0, item.clients.length > 2 ? 1 : 2) as client (client)}
                        {@render clientIdentity(client)}
                      {/each}
                      {#if item.clients.length > 2}
                        <Popover.Root>
                          <Popover.Trigger
                            class="dns-more-clients"
                            aria-label={`${t("Show all clients")} (${item.clients.length})`}
                            >+{item.clients.length - 1}</Popover.Trigger
                          >
                          <Popover.Portal>
                            <Popover.Content
                              role="dialog"
                              class="dns-query-popover dns-clients-popover"
                              aria-label={t("Clients")}
                              sideOffset={6}
                              collisionPadding={12}
                            >
                              {#each item.clients as client (client)}
                                <div class="dns-client-popover-row">
                                  {@render clientIdentity(client)}
                                </div>
                              {/each}
                            </Popover.Content>
                          </Popover.Portal>
                        </Popover.Root>
                      {/if}</span
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
  </PanelSurface>
</section>

<style>
  .confirmation-message {
    margin: 0;
    color: var(--text-2);
    font: 400 var(--text-body) / var(--line-height-body) var(--font);
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
    font-size: var(--text-caption);
  }
  .client-name,
  .client-address {
    overflow: hidden;
    white-space: nowrap;
    text-overflow: ellipsis;
  }
  /* Reserve two single-line slots so name resolution cannot resize rows.
   * More clients are available in the popover instead of growing the table. */
  .dns-table.grouped .client-identities {
    height: 2.5rem;
    gap: 0.1rem;
    justify-content: center;
  }
  .dns-table.grouped .client-identity {
    flex-direction: row;
    align-items: baseline;
    gap: 0.35rem;
    line-height: 1.2rem;
    white-space: nowrap;
    overflow-wrap: normal;
  }
  .dns-table.grouped .client-identity .client-name {
    min-width: 0;
  }
  .dns-table.grouped .client-identity .client-address {
    flex-shrink: 0;
    max-width: 48%;
  }
  .dns-table :global(button.dns-more-clients) {
    width: fit-content;
    min-height: 1.2rem;
    padding: 0;
    border: 0;
    background: transparent;
    color: var(--text-2);
    font: inherit;
    line-height: 1.2rem;
    cursor: pointer;
  }
  .dns-table :global(button.dns-more-clients:hover) {
    color: var(--text);
  }
  .dns-toolbar {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: var(--space-2);
  }
  .dns-toolbar :global(.dns-search) {
    flex: 1 1 12rem;
  }
  .dropped-count {
    flex-basis: 100%;
    color: var(--text-2);
    font-size: var(--text-small);
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
  :global(.dns-panel button:focus-visible),
  :global(.query-details:focus-visible) {
    outline: 2px solid var(--accent);
    outline-offset: 2px;
  }
  .dns-message {
    padding: var(--space-4) var(--panel-inset);
  }
  .dns-table {
    width: 100%;
    border-collapse: collapse;
    table-layout: fixed;
    text-align: left;
    color: var(--text);
    font: 400 var(--text-body) / var(--line-height-body) var(--font);
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
    font-size: var(--text-small);
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
  .dns-table.grouped td {
    height: 3.25rem;
  }
  .dns-table.grouped .domain .cell-value {
    display: -webkit-box;
    -webkit-box-orient: vertical;
    -webkit-line-clamp: 2;
    line-clamp: 2;
    overflow: hidden;
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
    font-size: var(--text-caption);
    font-variant-numeric: tabular-nums;
    white-space: nowrap;
  }
  .number {
    color: var(--text-2);
    font-size: var(--text-small);
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
    font: 400 var(--text-caption) / var(--line-height-body) var(--font);
    box-shadow: var(--shadow-popover);
    overflow-wrap: anywhere;
  }
  :global(.dns-query-popover .raw-query) {
    display: block;
  }
  :global(.dns-clients-popover) {
    max-height: min(24rem, calc(100vh - 24px));
    overflow-y: auto;
  }
  :global(.dns-clients-popover .dns-client-popover-row + .dns-client-popover-row) {
    margin-top: 0.5rem;
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
    .dns-toolbar :global(.dns-search) {
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
    .dns-table:not(.grouped) td,
    .dns-table.grouped td {
      display: grid;
      grid-template-columns: 5.6rem minmax(0, 1fr);
      align-items: baseline;
      gap: 0.35rem;
      min-width: 0;
      height: auto;
      padding: 0.05rem 0;
    }
    /* Baseline alignment depends on the client's font metrics: switching from
     * an IP-only label to a name+IP used to move the card by one pixel. */
    .dns-table.grouped td {
      align-items: start;
    }
    .dns-table td.domain {
      order: -1;
    }
    .cell-label {
      display: block;
      text-align: right;
      color: var(--text-2);
      font: 400 var(--text-small) / var(--line-height-body) var(--font);
      overflow-wrap: anywhere;
    }
  }
</style>
