<script lang="ts">
  import { onDestroy, type Snippet } from "svelte";

  import { hideFor, show, updateFor } from "../../lib/tooltip/tooltip.svelte";

  let { value, children }: { value: string; children: Snippet } = $props();
  let triggerEl: HTMLElement;
  let hovered = false;
  let focused = false;

  $effect(() => updateFor(triggerEl, value));

  function focusIn() {
    focused = true;
    void show(triggerEl, value, true);
  }

  function focusOut(event: FocusEvent) {
    // Moving between controls inside the same trigger is not a dismissal.
    if (event.relatedTarget instanceof Node && triggerEl.contains(event.relatedTarget)) return;
    focused = false;
    if (hovered) void show(triggerEl, value);
    else hideFor(triggerEl);
  }

  onDestroy(() => hideFor(triggerEl));
</script>

<div
  role="tooltip"
  data-value={value}
  bind:this={triggerEl}
  style:display="inline-flex"
  onpointerenter={() => {
    hovered = true;
    void show(triggerEl, value, focused);
  }}
  onpointerleave={() => {
    hovered = false;
    if (!focused) hideFor(triggerEl);
  }}
  onfocusin={focusIn}
  onfocusout={focusOut}
>
  {@render children()}
</div>
