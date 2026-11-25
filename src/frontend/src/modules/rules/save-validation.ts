import "./save-validation.css";

export type SaveValidationTarget =
  | { panelId: string; field: "name" | "url" | "priority" }
  | { panelId: string; field: "rule"; ruleId: string };

export function nextSaveValidationTarget(
  targets: Iterable<SaveValidationTarget>,
  previous?: SaveValidationTarget,
) {
  let first: SaveValidationTarget | undefined;
  for (const target of targets) {
    first ??= target;
    if (!previous) return target;
    if (
      target.panelId === previous.panelId &&
      target.field === previous.field &&
      (target.field !== "rule" || (previous.field === "rule" && target.ruleId === previous.ruleId))
    ) {
      previous = undefined;
    }
  }
  return first;
}

function clearSaveHighlight(this: HTMLElement) {
  delete this.dataset.saveError;
  this.removeEventListener("blur", clearSaveHighlight);
  this.removeEventListener("input", clearSaveHighlight);
}

export function focusSaveValidationField(field: HTMLElement | null | undefined) {
  if (!field) return;
  field.scrollIntoView({ block: "center", inline: "nearest", behavior: "instant" });
  field.focus({ preventScroll: true });
  if (document.activeElement !== field) return;

  if (field.hasAttribute("data-save-error")) {
    // Clicking Save can leave the same field focused on touch devices.
    const pulse = field
      .getAnimations()
      .find(
        (animation) =>
          animation instanceof CSSAnimation && animation.animationName === "save-error-underline",
      );
    if (pulse) pulse.currentTime = 0;
  } else {
    field.dataset.saveError = "";
    field.addEventListener("blur", clearSaveHighlight);
    field.addEventListener("input", clearSaveHighlight);
  }
}
