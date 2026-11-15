let motion: { finish: () => void; cancel: () => void } | undefined;

/** Let the browser animate the document; only explicit input cancels our move. */
export function scrollToDocumentBoundary(edge: "start" | "end") {
  motion?.finish();
  const top =
    edge === "start" ? 0 : Math.max(0, document.documentElement.scrollHeight - window.innerHeight);
  const smooth = !window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  if (smooth && Math.abs(window.scrollY - top) >= 1) {
    const controller = new AbortController();
    const current = {
      finish() {
        controller.abort();
        if (motion === current) motion = undefined;
      },
      cancel() {
        if (motion !== current) return;
        current.finish();
        window.scrollTo({ top: window.scrollY, behavior: "instant" });
      },
    };
    motion = current;
    const options = { passive: true, signal: controller.signal };
    // Wheel/touch scrolling already interrupts native smooth scrolling. A
    // scripted scroll here can overwrite input the compositor has just applied.
    window.addEventListener("wheel", current.finish, options);
    window.addEventListener("touchstart", current.finish, options);
    window.addEventListener(
      "pointerdown",
      (event) => {
        if (event.pointerType === "touch") current.finish();
        else current.cancel();
      },
      options,
    );
    window.addEventListener(
      "keydown",
      (event) => {
        if (
          [
            "ArrowUp",
            "ArrowDown",
            "PageUp",
            "PageDown",
            "Home",
            "End",
            " ",
            "Tab",
            "Escape",
          ].includes(event.key)
        )
          current.cancel();
      },
      { signal: controller.signal },
    );
    document.addEventListener(
      "scrollend",
      (event) => {
        if (event.target === document) current.finish();
      },
      options,
    );
  }

  window.scrollTo({ top, behavior: smooth ? "smooth" : "instant" });
}

/** Home/End and ordinary navigation retain their native browser behavior. */
export function initDocumentScrollShortcuts(): () => void {
  function onKey(event: KeyboardEvent) {
    if (
      event.defaultPrevented ||
      event.isComposing ||
      event.altKey ||
      event.shiftKey ||
      !(event.ctrlKey || event.metaKey) ||
      !["ArrowUp", "ArrowDown"].includes(event.key)
    )
      return;

    const target = event
      .composedPath()
      .find((node): node is HTMLElement => node instanceof HTMLElement);
    if (
      target?.closest(
        'input, textarea, select, [contenteditable]:not([contenteditable="false"]), [role="textbox"], [role="combobox"], [role="listbox"], [role="menu"], [role="dialog"], [data-popover-content]',
      ) ||
      document.querySelector(
        '[data-dialog-content][data-state="open"], [data-alert-dialog-content][data-state="open"], [data-popover-content][data-state="open"], [data-dropdown-menu-content][data-state="open"], [data-select-content][data-state="open"]',
      ) ||
      window.getSelection()?.isCollapsed === false
    )
      return;

    event.preventDefault();
    scrollToDocumentBoundary(event.key === "ArrowUp" ? "start" : "end");
  }

  window.addEventListener("keydown", onKey);
  return () => {
    window.removeEventListener("keydown", onKey);
    motion?.cancel();
  };
}
