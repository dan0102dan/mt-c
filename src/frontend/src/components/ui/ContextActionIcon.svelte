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
  <path class="plus" d="M8 12h8" />
  <path class="plus" d="M12 8v8" />
  <circle class="dot" cx="12" cy="12" r="2" fill="currentColor" stroke="none" />
</svg>

<style>
  .plus,
  .dot {
    transform-origin: 12px 12px;
    transition:
      transform 220ms cubic-bezier(0.2, 0, 0.2, 1),
      opacity 160ms ease;
  }
  .plus {
    vector-effect: non-scaling-stroke;
  }
  .dot {
    transform: scale(0);
    opacity: 0;
  }

  /* A rounded rect starts as the original circle, then its sides straighten
     into the stop square. The same outline stays mounted in both states. */
  .outline {
    x: 2px;
    y: 2px;
    width: 20px;
    height: 20px;
    rx: 10px;
    ry: 10px;
    transition:
      x 300ms cubic-bezier(0.2, 0, 0.2, 1),
      y 300ms cubic-bezier(0.2, 0, 0.2, 1),
      width 300ms cubic-bezier(0.2, 0, 0.2, 1),
      height 300ms cubic-bezier(0.2, 0, 0.2, 1),
      rx 300ms cubic-bezier(0.2, 0, 0.2, 1),
      ry 300ms cubic-bezier(0.2, 0, 0.2, 1);
  }
  .stop .outline {
    x: 3px;
    y: 3px;
    width: 18px;
    height: 18px;
    rx: 2px;
    ry: 2px;
  }

  .record .plus,
  .stop .plus {
    transform: scale(0);
  }
  .record .dot {
    transform: scale(1);
    opacity: 1;
    transition: none;
    animation: dot-bounce 320ms cubic-bezier(0.2, 0, 0.2, 1);
  }
  @keyframes dot-bounce {
    0% {
      transform: scale(0);
      opacity: 0;
    }
    70% {
      transform: scale(1.1);
      opacity: 1;
    }
    100% {
      transform: scale(1);
      opacity: 1;
    }
  }
  @media (prefers-reduced-motion: reduce) {
    .record .dot {
      animation: none;
    }
    .plus,
    .dot,
    .outline {
      transition: none;
    }
  }
</style>
