<script lang="ts">
  import type { Snippet } from "svelte";

  type Props = {
    id: string;
    title: string;
    children: Snippet;
    class?: string;
  };

  let { id, title, children, class: className = "" }: Props = $props();
</script>

<section class={`settings-group ${className}`} aria-labelledby={id}>
  <h3 {id}>{title}</h3>
  {@render children()}
</section>

<style>
  .settings-group {
    min-width: 0;
    padding: 0 var(--panel-inset) var(--space-6);
    border-bottom: 1px solid var(--bg-light-extra);
  }
  .settings-group:last-child {
    border-bottom: 0;
    padding-bottom: 0;
  }
  h3 {
    margin: 0 0 var(--space-4);
    color: var(--text);
    font: 600 var(--text-body) / var(--line-height-body) var(--font);
  }
</style>
