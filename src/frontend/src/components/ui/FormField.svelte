<script lang="ts">
  import type { Snippet } from "svelte";

  type Props = {
    id?: string;
    label: string;
    error?: string;
    hint?: string;
    children: Snippet;
    class?: string;
  };

  let { id, label, error, hint, children, class: className = "" }: Props = $props();
</script>

<div class={`form-field ${className}`}>
  {#if id}
    <label for={id}>{label}</label>
  {:else}
    <span class="field-label">{label}</span>
  {/if}
  {@render children()}
  {#if error}
    <p id={id ? `${id}-error` : undefined} class="field-error" role="alert">{error}</p>
  {:else if hint}
    <p id={id ? `${id}-hint` : undefined} class="field-hint">{hint}</p>
  {/if}
</div>

<style>
  .form-field {
    min-width: 0;
    display: grid;
    align-content: start;
    gap: var(--space-2);
  }
  label,
  .field-label {
    color: var(--text-2);
    font: 400 var(--text-small) / var(--line-height-body) var(--font);
  }
  .form-field :global(.form-control) {
    box-sizing: border-box;
    width: 100%;
    min-width: 0;
    min-height: var(--control-height);
    padding: var(--space-2);
    border: 1px solid var(--bg-light-extra);
    border-radius: var(--radius-panel);
    background: var(--bg-light);
    color: var(--text);
    font: 400 var(--text-body) / var(--line-height-body) var(--font);
  }
  .form-field :global(.form-control .selected) {
    width: 100%;
    justify-content: space-between;
  }
  .form-field :global(.form-control .selected-text) {
    align-items: flex-start;
  }
  .form-field :global(.form-control .selected-value) {
    padding-left: 0;
  }
  .form-field :global(.form-control[aria-invalid="true"]),
  .form-field :global(.form-control.invalid) {
    border-color: var(--red);
  }
  .form-field :global(.form-control:focus-visible) {
    outline: 1px solid var(--accent);
    outline-offset: 1px;
  }
  .form-field :global(.form-control:disabled) {
    opacity: 0.55;
  }
  .form-field :global(input[type="number"]) {
    appearance: textfield;
  }
  .form-field :global(input[type="number"]::-webkit-inner-spin-button),
  .form-field :global(input[type="number"]::-webkit-outer-spin-button) {
    -webkit-appearance: none;
    margin: 0;
  }
  .field-error,
  .field-hint {
    margin: 0;
    font: 400 var(--text-caption) / var(--line-height-body) var(--font);
    overflow-wrap: anywhere;
  }
  .field-error {
    color: var(--red);
  }
  .field-hint {
    color: var(--text-2);
  }
</style>
