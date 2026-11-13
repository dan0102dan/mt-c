import { requestViewportRead } from "../lib/virtual/viewport";

type ScrollbarState = {
  dragging: boolean;
  dragStartY: number;
  scrollStart: number;
  scrollRatio: number;
  dragFraction: number | null;
  dragStartFraction: number;
  dragTravel: number;
  hideTimer: number | null;
  targetElement: HTMLElement | null;
  boundsTop: number;
  boundsBottom: number;
  headerElement: HTMLElement | null;
};

type OverlayScrollbarOptions = {
  targetSelector?: string;
  headerSelector?: string;
};

const HIDE_DELAY = 900;
const MIN_THUMB_HEIGHT = 28;

const getScrollTop = () => document.documentElement.scrollTop || document.body.scrollTop || 0;

const setScrollTop = (value: number) => {
  document.documentElement.scrollTop = value;
  document.body.scrollTop = value;
};

const setVisible = (track: HTMLDivElement, state: ScrollbarState) => {
  track.classList.add("is-visible");
  if (state.hideTimer) window.clearTimeout(state.hideTimer);

  state.hideTimer = window.setTimeout(() => {
    track.classList.remove("is-visible");
  }, HIDE_DELAY);
};

// Read geometry together before writing styles. Reading track.clientHeight
// after changing its top/bottom forces layout on every virtual-row update.
const updateThumb = (track: HTMLDivElement, thumb: HTMLDivElement, state: ScrollbarState) => {
  const target = state.targetElement ?? document.documentElement;
  const rect = target.getBoundingClientRect();
  const viewportHeight = window.innerHeight;
  const scrollHeight = document.documentElement.scrollHeight;
  const clientHeight = document.documentElement.clientHeight;
  const scrollTop = getScrollTop();
  const headerBottom = state.headerElement?.getBoundingClientRect().bottom ?? 0;
  const top = Math.max(rect.top, headerBottom, 8);
  const bottom = Math.max(viewportHeight - rect.bottom, 8);
  const trackHeight = viewportHeight - top - bottom;
  const hidden =
    rect.height <= 0 || viewportHeight <= 0 || scrollHeight <= clientHeight || trackHeight <= 0;
  track.classList.toggle("is-hidden", hidden);
  if (hidden) return;
  if (state.boundsTop !== top) track.style.top = `${top}px`;
  if (state.boundsBottom !== bottom) track.style.bottom = `${bottom}px`;
  state.boundsTop = top;
  state.boundsBottom = bottom;
  const thumbHeight = Math.min(
    Math.max((clientHeight / scrollHeight) * trackHeight, MIN_THUMB_HEIGHT),
    trackHeight - 4,
  );
  const scrollable = scrollHeight - clientHeight;
  // Measurements can change scrollHeight while the thumb is captured. Keep
  // its requested fraction, including both edges, until the drag finishes.
  const desiredTop =
    state.dragging && state.dragFraction !== null
      ? state.dragFraction * Math.max(0, scrollable)
      : scrollTop;
  if (Math.abs(desiredTop - scrollTop) > 0.5) setScrollTop(desiredTop);
  const thumbTop = scrollable > 0 ? (desiredTop / scrollable) * (trackHeight - thumbHeight) : 0;
  const height = `${thumbHeight}px`;
  const transform = `translateY(${thumbTop}px)`;
  if (thumb.style.height !== height) thumb.style.height = height;
  if (thumb.style.transform !== transform) thumb.style.transform = transform;
};

export const initOverlayScrollbar = (options: OverlayScrollbarOptions = {}) => {
  document.documentElement.classList.add("has-overlay-scrollbar");
  document.body.classList.add("has-overlay-scrollbar");

  const track = document.createElement("div");
  track.className = "overlay-scrollbar";

  const thumb = document.createElement("div");
  thumb.className = "overlay-scrollbar__thumb";

  track.appendChild(thumb);
  document.body.appendChild(track);

  const state: ScrollbarState = {
    dragging: false,
    dragStartY: 0,
    scrollStart: 0,
    scrollRatio: 1,
    dragFraction: null,
    dragStartFraction: 0,
    dragTravel: 0,
    hideTimer: null,
    targetElement: null,
    boundsTop: 0,
    boundsBottom: 0,
    headerElement: null,
  };

  const resizeObserver = new ResizeObserver(() => update());
  resizeObserver.observe(document.documentElement);

  const resolveTarget = () => {
    if (!options.targetSelector) return;

    const targets = Array.from(document.querySelectorAll<HTMLElement>(options.targetSelector));
    const target =
      targets.find((candidate) => {
        const style = window.getComputedStyle(candidate);
        if (style.display === "none" || style.visibility === "hidden") return false;
        const rect = candidate.getBoundingClientRect();
        return rect.height > 0 && rect.width > 0;
      }) ?? null;

    const header = options.headerSelector
      ? document.querySelector<HTMLElement>(options.headerSelector)
      : null;
    if (target !== state.targetElement || header !== state.headerElement) {
      if (state.targetElement) resizeObserver.unobserve(state.targetElement);
      if (state.headerElement) resizeObserver.unobserve(state.headerElement);
      state.targetElement = target;
      state.headerElement = header;
      if (target) resizeObserver.observe(target);
      if (header) resizeObserver.observe(header);
    }
  };

  let targetDirty = true;
  const read = () => {
    if (targetDirty) {
      resolveTarget();
      targetDirty = false;
    }
    updateThumb(track, thumb, state);
  };
  const update = (resolve = false) => {
    targetDirty ||= resolve;
    requestViewportRead(read);
  };

  update(true);
  window.addEventListener(
    "scroll",
    () => {
      setVisible(track, state);
      update();
    },
    { passive: true },
  );
  window.addEventListener("resize", () => update(true));
  track.addEventListener("mouseenter", () => setVisible(track, state));

  if (options.targetSelector) {
    const mutationObserver = new MutationObserver((records) => {
      // Virtual rows are continuously added/removed. They cannot change the
      // active collection/header; resolve selectors only on tab or root changes.
      const tabsChanged = records.some(
        (record) =>
          record.type === "attributes" &&
          record.target instanceof Element &&
          record.target.hasAttribute("data-tabs-content"),
      );
      if (tabsChanged || !state.targetElement?.isConnected || !state.headerElement?.isConnected)
        update(true);
    });
    mutationObserver.observe(document.body, {
      childList: true,
      subtree: true,
      attributes: true,
      attributeFilter: ["data-state"],
    });
  }

  thumb.addEventListener("pointerdown", (event) => {
    state.dragging = true;
    state.dragStartY = event.clientY;
    state.scrollStart = getScrollTop();

    const scrollHeight = document.documentElement.scrollHeight;
    const clientHeight = document.documentElement.clientHeight;

    const trackHeight = track.clientHeight;
    const thumbHeight = thumb.offsetHeight;

    const maxThumbTop = trackHeight - thumbHeight;
    const scrollable = scrollHeight - clientHeight;

    state.scrollRatio = maxThumbTop > 0 && scrollable > 0 ? scrollable / maxThumbTop : 1;
    state.dragTravel = maxThumbTop;
    state.dragStartFraction = scrollable > 0 ? state.scrollStart / scrollable : 0;
    state.dragFraction = state.dragStartFraction;

    thumb.setPointerCapture(event.pointerId);
    setVisible(track, state);
  });

  thumb.addEventListener("pointermove", (event) => {
    if (!state.dragging) return;

    const delta = event.clientY - state.dragStartY;
    state.dragFraction =
      state.dragTravel > 0
        ? Math.min(1, Math.max(0, state.dragStartFraction + delta / state.dragTravel))
        : 0;
    setScrollTop(
      state.dragFraction * Math.max(0, document.documentElement.scrollHeight - window.innerHeight),
    );
  });

  const stopDragging = (event: PointerEvent) => {
    if (!state.dragging) return;

    state.dragging = false;
    state.dragFraction = null;
    try {
      thumb.releasePointerCapture(event.pointerId);
    } catch {
      // no-op
    }
  };

  thumb.addEventListener("pointerup", stopDragging);
  thumb.addEventListener("pointercancel", stopDragging);

  track.addEventListener("pointerdown", (event) => {
    if (event.target !== track) return;

    const trackRect = track.getBoundingClientRect();
    const offset = event.clientY - trackRect.top;

    const scrollHeight = document.documentElement.scrollHeight;
    const clientHeight = document.documentElement.clientHeight;

    const trackHeight = track.clientHeight;
    const thumbHeight = thumb.offsetHeight;

    const maxThumbTop = trackHeight - thumbHeight;
    const scrollable = scrollHeight - clientHeight;

    if (maxThumbTop <= 0) return;

    const targetThumbTop = Math.min(Math.max(offset - thumbHeight / 2, 0), maxThumbTop);
    setScrollTop((targetThumbTop / maxThumbTop) * scrollable);

    setVisible(track, state);
  });
};
