<script lang="ts">
  import type { Snippet } from "svelte";

  import { Search } from "./icons";

  type Props = {
    value: string;
    label: string;
    placeholder?: string;
    disabled?: boolean;
    expandable?: boolean;
    id?: string;
    name?: string;
    class?: string;
    style?: string;
    icon?: Snippet;
    oninput?: (event: Event) => void;
  };

  let {
    value = $bindable(""),
    label,
    placeholder = label,
    disabled = false,
    expandable = false,
    id,
    name,
    class: className = "",
    style,
    icon,
    oninput,
  }: Props = $props();
  let input: HTMLInputElement;

  function handleShortcut(event: KeyboardEvent) {
    if (
      event.defaultPrevented ||
      event.isComposing ||
      event.altKey ||
      event.shiftKey ||
      !(event.metaKey || event.ctrlKey) ||
      (event.code !== "KeyF" && event.key.toLowerCase() !== "f") ||
      document.querySelector('[data-dialog-content][data-state="open"]') ||
      disabled ||
      !input?.getClientRects().length
    )
      return;
    event.preventDefault();
    input.focus();
    input.select();
  }

  function focusInput(event: PointerEvent) {
    if (disabled || event.target instanceof HTMLInputElement) return;
    // Preserve focus while clicking the icon of an expanded mobile search.
    event.preventDefault();
    input?.focus();
  }

  function handleEscape(event: KeyboardEvent) {
    if (event.key !== "Escape") return;
    event.preventDefault();
    value = "";
    oninput?.(event);
    input.blur();
  }
</script>

<svelte:window onkeydown={handleShortcut} />

<label
  class={`search-container ${className}`}
  class:expandable
  class:disabled
  {style}
  onpointerdown={focusInput}
>
  <span class="icon-wrapper" aria-hidden="true">
    {#if icon}{@render icon()}{:else}<Search size={22} />{/if}
  </span>
  <span class="input-wrapper">
    <input
      bind:this={input}
      bind:value
      {id}
      {name}
      {disabled}
      {placeholder}
      {oninput}
      onkeydown={handleEscape}
      aria-label={label}
      type="search"
      class="search-input"
      data-page-search
    />
  </span>
</label>

<style>
  .search-container {
    --search-icon-size: 22px;
    --search-collapsed-width: calc(var(--search-icon-size) + 1.2rem + 2px);
    box-sizing: border-box;
    display: flex;
    align-items: center;
    min-width: 0;
    width: 100%;
    padding: 0.6rem;
    border: 1px solid var(--bg-light-extra);
    border-radius: var(--radius-panel);
    background: var(--bg-light);
    color: var(--text-2);
    font: 400 var(--text-body) var(--font);
    white-space: nowrap;
    cursor: text;
    transition:
      background-color 0.1s ease-in-out,
      border-color 0.1s ease-in-out,
      color 0.1s ease-in-out,
      width 0.3s cubic-bezier(0.25, 1, 0.5, 1);
  }
  .search-container:hover {
    background: var(--bg-light-extra);
    color: var(--text);
  }
  .search-container:focus-within,
  .search-container:has(.search-input:not(:placeholder-shown)) {
    background: var(--bg-light);
    color: var(--text);
    border-color: var(--accent);
    max-width: 100%;
  }
  .icon-wrapper {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
  }
  .icon-wrapper :global(svg) {
    width: var(--search-icon-size);
    height: var(--search-icon-size);
  }
  .input-wrapper {
    display: block;
    width: 100%;
    min-width: 0;
    margin-left: 0.3rem;
    overflow: hidden;
    transition:
      width 0.3s cubic-bezier(0.25, 1, 0.5, 1),
      margin-left 0.3s cubic-bezier(0.25, 1, 0.5, 1);
  }
  .search-input {
    box-sizing: border-box;
    appearance: none;
    width: 100%;
    min-width: 0;
    margin: 0;
    padding: 0;
    border: 0;
    outline: none;
    background: transparent;
    color: inherit;
    font: inherit;
    transition: opacity 0.2s ease;
  }
  .search-input::-webkit-search-cancel-button {
    cursor: pointer;
  }
  .disabled {
    opacity: 0.5;
    cursor: default;
  }
  .expandable {
    width: auto;
  }
  .expandable .input-wrapper {
    width: min(700px, 50vw);
  }
  @media (max-width: 570px) {
    .expandable {
      width: var(--search-collapsed-width);
      height: var(--search-collapsed-width);
      min-height: var(--search-collapsed-width);
      cursor: pointer;
    }
    .expandable .input-wrapper {
      width: 0;
      margin-left: 0;
    }
    .expandable .search-input {
      opacity: 0;
    }
    .expandable:focus-within,
    .expandable:has(.search-input:not(:placeholder-shown)) {
      width: 100%;
      cursor: text;
    }
    .expandable:focus-within .input-wrapper,
    .expandable:has(.search-input:not(:placeholder-shown)) .input-wrapper {
      width: 100%;
      margin-left: 0.3rem;
    }
    .expandable:focus-within .search-input,
    .expandable:has(.search-input:not(:placeholder-shown)) .search-input {
      opacity: 1;
    }
  }
  @media (prefers-reduced-motion: reduce) {
    .search-container,
    .input-wrapper,
    .search-input {
      transition: none;
    }
  }
</style>
