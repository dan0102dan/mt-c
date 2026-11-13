<script lang="ts">
  import type { Snippet } from "svelte";
  import type { HTMLAttributes } from "svelte/elements";

  type Props = HTMLAttributes<HTMLDivElement> & {
    children: Snippet;
    header?: Snippet;
    footer?: Snippet;
  };

  let { children, header, footer, class: className = "", ...rest }: Props = $props();
</script>

<div class={`panel-surface ${className}`} {...rest}>
  {#if header}<div class="panel-surface-header">{@render header()}</div>{/if}
  {@render children()}
  {#if footer}<div class="panel-surface-footer">{@render footer()}</div>{/if}
</div>

<style>
  .panel-surface {
    min-width: 0;
    border: 1px solid var(--bg-light-extra);
    border-radius: var(--radius-panel);
    background: var(--bg-medium);
  }
  .panel-surface-header {
    min-width: 0;
    padding: var(--panel-inset);
    border-radius: var(--radius-panel) var(--radius-panel) 0 0;
    background: var(--bg-light);
  }
  .panel-surface-header:empty {
    display: none;
  }
  .panel-surface-footer {
    min-width: 0;
    padding: var(--panel-inset);
    color: var(--text-2);
    font: 400 var(--text-small) / var(--line-height-body) var(--font);
  }
</style>
