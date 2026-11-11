<script lang="ts">
  // Design port: MagiTrickle/MagiTrickle@4f94aa6 by shevernitskiy.
  // The application store owns transport/history, never this virtualized view.
  import { onDestroy, tick, untrack, type Snippet } from "svelte";

  import Button from "../../components/ui/Button.svelte";
  import { Clear, Filter, Save, ScrollToBottom } from "../../components/ui/icons";
  import Select from "../../components/ui/Select.svelte";
  import Tooltip from "../../components/ui/Tooltip.svelte";

  import {
    LOG_LEVELS,
    logExportParts,
    logLevel,
    logText,
    logWindow,
    matchesLog,
    type LogEntry,
  } from "./log-view";

  type Props = {
    entries: readonly LogEntry[];
    revision?: number;
    onclear: () => void;
    controls?: Snippet;
    labels: {
      filter: string;
      level: string;
      save: string;
      clear: string;
      scrollToBottom: string;
      results: string;
      empty: string;
      session?: string;
    };
    status?: string;
  };

  let { entries, revision = 0, onclear, controls, labels, status = "" }: Props = $props();
  const LINE_HEIGHT = 16;
  const SCROLL_TO_BOTTOM_DELTA = 40;
  let level = $state("trace");
  let filter = $state("");
  let container = $state<HTMLDivElement>();
  let container_height = $state(0);
  let scroll_top = $state(0);
  let follow_tail = $state(true);
  let export_url: string | undefined;
  let export_timer: ReturnType<typeof setTimeout> | undefined;

  let items_filtered = $derived.by(() => {
    void revision; // Plain ingest array is published once per batch, not per record.
    return entries.filter((item) => matchesLog(item, level, filter));
  });
  let window = $derived(logWindow(items_filtered.length, scroll_top, container_height));
  let visible_items = $derived(items_filtered.slice(window.start, window.end));
  let hasEntries = $derived.by(() => { void revision; return entries.length > 0; });

  function scrollTopChanges() {
    if (!container) return;
    scroll_top = container.scrollTop;
    follow_tail =
      container.scrollHeight - container.clientHeight - scroll_top < SCROLL_TO_BOTTOM_DELTA;
  }

  function scrollToBottom() {
    if (!container) return;
    follow_tail = true;
    container.focus({ preventScroll: true });
    container.scrollTop = container.scrollHeight;
    scroll_top = container.scrollTop;
  }

  function resetScroll() {
    follow_tail = false;
    scroll_top = 0;
    if (container) container.scrollTop = 0;
  }

  $effect(() => {
    void items_filtered.at(-1)?.id;
    void items_filtered.length;
    void container_height;
    const target = container;
    const follow = untrack(() => follow_tail);
    let cancelled = false;
    if (target && follow) {
      void tick().then(() => {
        if (!cancelled) {
          target.scrollTop = target.scrollHeight;
          scroll_top = target.scrollTop;
        }
      });
    }
    return () => { cancelled = true; };
  });

  function clearLines() {
    if (!entries.length) return;
    onclear();
    resetScroll();
    follow_tail = true;
  }

  function releaseExport() {
    clearTimeout(export_timer);
    if (export_url) URL.revokeObjectURL(export_url);
    export_url = undefined;
  }
  function saveLogs() {
    if (!entries.length) return;
    releaseExport();
    // All received records, not just the filter; works with a dead backend.
    export_url = URL.createObjectURL(new Blob(logExportParts(entries), { type: "text/plain" }));
    const link = document.createElement("a");
    link.href = export_url;
    link.download = `${new Date().toISOString().replace(/[:.]/g, "-")}-mt-c.log`;
    document.body.appendChild(link);
    try { link.click(); } finally { link.remove(); }
    export_timer = setTimeout(releaseExport, 30000);
  }
  onDestroy(releaseExport);
</script>

<div class="logs-controls">
  <div class="filter">
    <Filter size={22} opacity={0.5} />
    <input
      type="search"
      class="filter-input"
      data-page-search
      aria-label={labels.filter}
      placeholder={labels.filter}
      bind:value={filter}
      oninput={resetScroll}
    />
    <Select
      options={Object.keys(LOG_LEVELS).map((value) => ({ label: value, value }))}
      bind:selected={level}
      onValueChange={resetScroll}
      ariaLabel={labels.level}
      style="width: 100px; flex-shrink: 0"
    />
  </div>
  <div class="logs-controls-actions">
    {#if controls}{@render controls()}{/if}
    <Tooltip value={labels.save}>
      <Button type="button" onclick={saveLogs} aria-label={labels.save} disabled={!hasEntries}>
        <Save size={22} />
      </Button>
    </Tooltip>
    <Tooltip value={labels.clear}>
      <Button type="button" onclick={clearLines} aria-label={labels.clear} disabled={!hasEntries}>
        <Clear size={22} />
      </Button>
    </Tooltip>
    <Tooltip value={labels.scrollToBottom}>
      <Button type="button" onclick={scrollToBottom} aria-label={labels.scrollToBottom}>
        <ScrollToBottom size={22} />
      </Button>
    </Tooltip>
  </div>
</div>

{#if status}<div class="status" role="status">{status}</div>{/if}

<div
  bind:this={container}
  bind:clientHeight={container_height}
  onscroll={scrollTopChanges}
  class="container"
  role="region"
  aria-label={labels.results}
  tabindex="-1"
>
  <div class="spacer" style:height={`${items_filtered.length * LINE_HEIGHT + 10}px`}></div>
  {#if !items_filtered.length}<div class="empty">{labels.empty}</div>{/if}
  {#each visible_items as item, index (item.id)}
    <div class="line" class:session={item.kind === "session"}
      style:top={`${(window.start + index) * LINE_HEIGHT + 5}px`}>
      <span class="time">{logText(item.fields.time)}</span>
      {#if item.kind === "session"}
        {labels.session ?? item.message}
      {:else}
        <span class={Object.hasOwn(LOG_LEVELS, item.level) ? item.level : "unknown"}
          >{logLevel(item.level)[1]}</span>
        {logText(item.message)}
        {#each Object.entries(item.fields) as [key, value]}
          {#if key !== "time"}
            <span class="key">{logText(key)}=</span><span class="value">{logText(value)}</span>{" "}
          {/if}
        {/each}
      {/if}
    </div>
  {/each}
</div>

<style>
  .container {
    position: relative;
    height: calc(100vh - 310px);
    min-height: 12rem;
    overflow: auto;
    overflow-anchor: none;
    padding: 0 0.5rem;
    border-radius: 0.5rem;
    background-color: var(--bg-dark-extra);
    border: 1px solid var(--bg-light-extra);
  }
  .spacer { position: relative; }
  .logs-controls {
    display: flex;
    align-items: end;
    gap: 0.5rem;
    padding: 0.5rem 0;
    margin-bottom: 0.5rem;
  }
  .logs-controls-actions {
    display: flex;
    align-items: end;
    justify-content: end;
    gap: 0.5rem;
    flex-shrink: 0;
  }
  .filter {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    background-color: var(--bg-dark-extra);
    padding: 0.5rem;
    border: 1px solid var(--bg-light-extra);
    border-radius: 0.5rem;
    flex: 1;
    min-width: 0;
  }
  .filter-input {
    border: none;
    background-color: transparent;
    font: 400 1rem var(--font);
    color: var(--text);
    border-bottom: 1px solid transparent;
    width: 100%;
    min-width: 0;
  }
  .filter-input:focus-visible { outline: none; border-bottom-color: var(--accent); }
  .line {
    position: absolute;
    height: 16px;
    line-height: 16px;
    font-family: var(--font-mono);
    font-size: 0.8rem;
    white-space: nowrap;
  }
  .time, .status, .empty, .unknown, .session { color: var(--text-2); }
  .status { margin-bottom: 0.5rem; }
  .empty { position: absolute; top: 0.5rem; }
  .key { color: var(--violet); }
  .trace { color: #00aaff; }
  .debug { color: var(--text-2); }
  .info { color: #66bb6a; }
  .warn { color: #ffd600; }
  .error { color: #ff3d3d; }
  .fatal { color: #ff4500; }
  .panic { color: #ff1493; }
  @media (max-width: 700px) {
    .logs-controls { flex-wrap: wrap; }
    .filter { flex-basis: 100%; }
    .logs-controls-actions { margin-left: auto; }
  }
</style>
