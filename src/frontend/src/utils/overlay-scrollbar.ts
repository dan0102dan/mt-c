type ScrollbarState = {
  dragging: boolean;
  dragStartY: number;
  scrollStart: number;
  scrollRatio: number;
  hideTimer: number | null;
  rafId: number | null;
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

const updateBounds = (track: HTMLDivElement, state: ScrollbarState) => {
  const target = state.targetElement ?? document.documentElement;
  const rect = target.getBoundingClientRect();
  const viewportHeight = window.innerHeight;
  const margin = 8;

  if (rect.height <= 0 || viewportHeight <= 0) {
    track.classList.add("is-hidden");
    return;
  }

  track.classList.remove("is-hidden");

  const headerBottom = state.headerElement?.getBoundingClientRect().bottom ?? 0;
  const top = Math.max(rect.top, headerBottom, margin);
  const bottom = Math.max(viewportHeight - rect.bottom, margin);

  state.boundsTop = top;
  state.boundsBottom = bottom;

  track.style.top = `${state.boundsTop}px`;
  track.style.bottom = `${state.boundsBottom}px`;
};

const updateThumb = (track: HTMLDivElement, thumb: HTMLDivElement, state: ScrollbarState) => {
  if (state.rafId) return;

  state.rafId = window.requestAnimationFrame(() => {
    state.rafId = null;

    updateBounds(track, state);

    const scrollHeight = document.documentElement.scrollHeight;
    const clientHeight = document.documentElement.clientHeight;
    const scrollTop = getScrollTop();
    if (scrollHeight <= clientHeight || state.boundsTop + state.boundsBottom >= clientHeight) {
      track.classList.add("is-hidden");
      return;
    }

    track.classList.remove("is-hidden");

    const trackHeight = track.clientHeight;
    const maxThumbHeight = trackHeight - 4;

    const thumbHeight = Math.min(
      Math.max((clientHeight / scrollHeight) * trackHeight, MIN_THUMB_HEIGHT),
      maxThumbHeight,
    );

    const maxThumbTop = trackHeight - thumbHeight;
    const scrollable = scrollHeight - clientHeight;
    const thumbTop = scrollable > 0 ? (scrollTop / scrollable) * maxThumbTop : 0;

    thumb.style.height = `${thumbHeight}px`;
    thumb.style.transform = `translateY(${thumbTop}px)`;
  });
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
    hideTimer: null,
    rafId: null,
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

  const update = () => {
    resolveTarget();
    updateThumb(track, thumb, state);
  };

  resolveTarget();
  updateBounds(track, state);
  update();

  window.addEventListener(
    "scroll",
    () => {
      setVisible(track, state);
      updateThumb(track, thumb, state);
    },
    { passive: true },
  );

  window.addEventListener("resize", () => {
    updateBounds(track, state);
    update();
  });

  track.addEventListener("mouseenter", () => setVisible(track, state));

  if (options.targetSelector) {
    const mutationObserver = new MutationObserver(() => {
      resolveTarget();
      updateBounds(track, state);
      update();
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

    thumb.setPointerCapture(event.pointerId);
    setVisible(track, state);
  });

  thumb.addEventListener("pointermove", (event) => {
    if (!state.dragging) return;

    const delta = event.clientY - state.dragStartY;
    setScrollTop(state.scrollStart + delta * state.scrollRatio);
  });

  const stopDragging = (event: PointerEvent) => {
    if (!state.dragging) return;

    state.dragging = false;
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
