<script lang="ts">
  import type { Snippet } from "svelte";
  import type { HTMLAttributes } from "svelte/elements";

  import { settingsSection, type SectionControls } from "./section-controls";

  type Props = HTMLAttributes<HTMLElement> & {
    id: string;
    title: string;
    headingId?: string;
    controls: () => Omit<SectionControls, "dialog" | "toolbarFeedback">;
    dialog?: Snippet;
    toolbarFeedback?: Snippet;
    showFeedback?: boolean;
    children: Snippet;
  };

  let {
    id,
    title,
    headingId = `${id}-heading`,
    controls,
    dialog,
    toolbarFeedback,
    showFeedback = false,
    children,
    ...rest
  }: Props = $props();

  const register = settingsSection();
</script>

<section
  {id}
  aria-labelledby={headingId}
  {...rest}
  use:register={() => ({
    ...controls(),
    dialog,
    toolbarFeedback: showFeedback ? toolbarFeedback : undefined,
  })}
>
  <h2 id={headingId} class="sr-only">{title}</h2>
  {@render children()}
</section>

<style>
  .sr-only {
    position: absolute;
    width: 1px;
    height: 1px;
    padding: 0;
    margin: -1px;
    overflow: hidden;
    clip-path: inset(50%);
    border: 0;
  }
</style>
