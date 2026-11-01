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
  <circle class="outline" cx="12" cy="12" r="10" />
  <path class="plus" d="M8 12h8" />
  <path class="plus" d="M12 8v8" />
  <circle class="dot" cx="12" cy="12" r="2" fill="currentColor" stroke="none" />
  <rect class="square" x="3" y="3" width="18" height="18" rx="2" />
</svg>

<style>
  .plus,
  .dot,
  .square,
  .outline {
    transform-origin: 12px 12px;
    transition:
      transform 220ms cubic-bezier(0.2, 0, 0.2, 1),
      opacity 160ms ease;
  }
  .plus {
    vector-effect: non-scaling-stroke;
  }
  .dot,
  .square {
    transform: scale(0);
    opacity: 0;
  }
  .record .plus,
  .stop .plus {
    transform: scale(0);
  }
  .record .dot,
  .stop .square {
    transform: scale(1);
    opacity: 1;
  }
  .record .dot {
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
  .stop .outline {
    opacity: 0;
    transform: scale(0.9);
  }
  @media (prefers-reduced-motion: reduce) {
    .record .dot {
      animation: none;
    }
    .plus,
    .dot,
    .square,
    .outline {
      transition: none;
    }
  }
</style>
