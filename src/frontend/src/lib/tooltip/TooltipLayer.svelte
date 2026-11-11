<script lang="ts">
  import { hide, tooltip } from "./tooltip.svelte";
</script>

<svelte:window
  onkeydown={(event) => {
    if (event.key === "Escape" && tooltip.visible) hide();
  }}
/>

{#if tooltip.visible}
  <div
    id="global-tooltip"
    class="tooltip-content"
    class:focused={tooltip.focused}
    role="tooltip"
    style:left="{tooltip.x}px"
    style:top="{tooltip.y}px"
    style:opacity={tooltip.opacity}
  >
    {tooltip.text}
  </div>
{/if}

<style>
  .tooltip-content {
    position: fixed;
    z-index: 20;
    pointer-events: none;
    background-color: var(--bg-dark);
    color: var(--text);
    border: 1px solid var(--bg-light-extra);
    padding: 0.2rem 0.5rem;
    border-radius: 0.5rem;
    font-size: smaller;
    box-sizing: border-box;
    width: max-content;
    max-width: min(24rem, calc(100vw - 16px));
    white-space: pre-line;
    overflow-wrap: anywhere;
    transition: opacity 0.1s ease-out;
  }

  @media (hover: none), (pointer: coarse) {
    .tooltip-content:not(.focused) {
      display: none;
    }
  }
</style>
