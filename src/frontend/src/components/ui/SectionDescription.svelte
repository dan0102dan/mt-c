<script lang="ts">
  import type { Snippet } from "svelte";

  type Props = {
    children: Snippet;
  };

  let { children }: Props = $props();
</script>

<p class="settings-description">{@render children()}</p>

<style>
  .settings-description {
    margin: 0 0 var(--space-5);
    padding-inline: var(--panel-inset);
    color: var(--text-2);
    font: 400 0.95rem / var(--line-height-body) var(--font);
    overflow-wrap: anywhere;
  }
</style>
