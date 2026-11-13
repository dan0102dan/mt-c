<script lang="ts">
  import type { Snippet } from "svelte";

  import Switch from "./Switch.svelte";

  type Props = {
    id: string;
    label?: string;
    ariaLabel?: string;
    children?: Snippet;
    checked: boolean;
    disabled?: boolean;
  };

  let {
    id,
    label,
    ariaLabel = label,
    children,
    checked = $bindable(false),
    disabled = false,
  }: Props = $props();
</script>

<div class="switch-row">
  <Switch {id} bind:checked {disabled} aria-label={ariaLabel} />
  <label for={id}
    >{#if children}{@render children()}{:else}{label}{/if}</label
  >
</div>

<style>
  .switch-row {
    display: flex;
    align-items: flex-start;
    gap: 0.65rem;
    padding-block: var(--space-2);
    font: 400 var(--text-body) / var(--line-height-body) var(--font);
  }
  .switch-row :global([data-switch-root]) {
    margin-top: calc((1.5rem - 18px) / 2);
  }
  .switch-row :global([data-switch-root]:focus-visible) {
    outline: 2px solid var(--accent);
    outline-offset: 2px;
  }
  label {
    min-width: 0;
    cursor: pointer;
    overflow-wrap: anywhere;
  }
</style>
