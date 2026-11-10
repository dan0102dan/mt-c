<script lang="ts">
  let { mode = "add" }: { mode?: "add" | "record" | "stop" } = $props();
</script>

<svg
  width="22"
  height="22"
  viewBox="0 0 24 24"
  fill="none"
  stroke="currentColor"
  stroke-width="2"
  stroke-linecap="round"
  stroke-linejoin="round"
  aria-hidden="true"
  class:record={mode === "record"}
  class:stop={mode === "stop"}
>
  <rect class="outline" x="2" y="2" width="20" height="20" rx="10" ry="10" />
  <g class="symbol" fill="currentColor" stroke="none">
    <rect class="bar horizontal" x="7" y="11" width="10" height="2" rx="1" ry="1" />
    <rect class="bar vertical" x="11" y="7" width="2" height="10" rx="1" ry="1" />
  </g>
</svg>

<style>
  svg {
    --morph-duration: 300ms;
    --morph-ease: cubic-bezier(0.4, 0, 0.2, 1);
  }

  /* Both arms round into the same dot, rather than collapsing a stroked path
     to scale(0) while a separate dot starts a non-reversible bounce. */
  .bar,
  .outline {
    transition:
      x var(--morph-duration) var(--morph-ease),
      y var(--morph-duration) var(--morph-ease),
      width var(--morph-duration) var(--morph-ease),
      height var(--morph-duration) var(--morph-ease),
      rx var(--morph-duration) var(--morph-ease),
      ry var(--morph-duration) var(--morph-ease);
  }
  .bar {
    rx: 1px;
    ry: 1px;
  }
  .horizontal {
    x: 7px;
    y: 11px;
    width: 10px;
    height: 2px;
  }
  .vertical {
    x: 11px;
    y: 7px;
    width: 2px;
    height: 10px;
  }
  .record .bar,
  .stop .bar {
    x: 10px;
    y: 10px;
    width: 4px;
    height: 4px;
    rx: 2px;
    ry: 2px;
  }
  /* Fade the union, not each overlapping arm, so the centre stays uniform. */
  .symbol {
    opacity: 1;
    transition: opacity var(--morph-duration) var(--morph-ease);
  }
  .stop .symbol {
    opacity: 0;
  }

  /* Keep one outline mounted through the circle <-> square transition. */
  .outline {
    x: 2px;
    y: 2px;
    width: 20px;
    height: 20px;
    rx: 10px;
    ry: 10px;
  }
  .stop .outline {
    x: 3px;
    y: 3px;
    width: 18px;
    height: 18px;
    rx: 2px;
    ry: 2px;
  }

  @media (prefers-reduced-motion: reduce) {
    .bar,
    .symbol,
    .outline {
      transition: none;
    }
  }
</style>
