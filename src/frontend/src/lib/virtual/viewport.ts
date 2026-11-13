import { scrollMotionDuration, scrollMotionProgress } from "../../utils/scroll-motion";

export type VirtualViewport = {
  scrollTop: number;
  height: number;
  width: number;
  velocity: number;
  direction: number;
  interaction: boolean;
  dragSource: HTMLElement | undefined;
};

type Subscriber = (viewport: VirtualViewport) => void;
const subscribers = new Set<Subscriber>();
const reads = new Set<() => void>();
let frame = 0;
let interactionPending = false;
let previousTop = 0;
let previousTime = 0;
let dragSource: HTMLElement | undefined;
let listening = false;
let boundary:
  | {
      edge: "start" | "end";
      started: number;
      from: number;
      duration: number;
      progress: number;
      target: number;
      stableFrames: number;
    }
  | undefined;

export function virtualBoundaryScrollActive(): boolean {
  return boundary !== undefined;
}

export function virtualBoundaryScrollAnimating(): boolean {
  return boundary !== undefined && boundary.progress < 1;
}

function cancelBoundaryScroll() {
  boundary = undefined;
}

function settleBoundaryScroll(now: number) {
  if (!boundary) return;
  const target =
    boundary.edge === "start"
      ? 0
      : Math.max(0, document.documentElement.scrollHeight - window.innerHeight);
  const progress = boundary.duration
    ? Math.max(0, Math.min(1, (now - boundary.started) / boundary.duration))
    : 1;
  boundary.progress = progress;
  const position = boundary.from + (target - boundary.from) * scrollMotionProgress(progress);
  const atTarget = Math.abs(window.scrollY - target) < 1;
  boundary.stableFrames =
    progress === 1 && atTarget && target === boundary.target ? boundary.stableFrames + 1 : 0;
  boundary.target = target;
  // Drive the short animation ourselves: native smooth scrolling can be
  // cancelled by virtual layout updates, and its original end can move.
  if (Math.abs(window.scrollY - position) >= 0.5)
    window.scrollTo({ top: position, behavior: "instant" });
  // Nested panel/rule measurements can move the document edge after the native
  // shortcut's first jump. Retain the requested edge until those commits settle.
  if (boundary.stableFrames >= 4 || now - boundary.started >= 1500) cancelBoundaryScroll();
}

function onBoundaryKey(event: KeyboardEvent) {
  const modifierArrow =
    (event.ctrlKey || event.metaKey) && ["ArrowUp", "ArrowDown"].includes(event.key);
  const edgeKey = ["Home", "End"].includes(event.key);
  const target = event
    .composedPath()
    .find((node): node is HTMLElement => node instanceof HTMLElement);
  if (
    event.defaultPrevented ||
    event.isComposing ||
    event.altKey ||
    event.shiftKey ||
    (!modifierArrow && !edgeKey) ||
    target?.closest(
      'input, textarea, select, [contenteditable]:not([contenteditable="false"]), [role="textbox"], [role="combobox"], [role="listbox"], [role="menu"], [role="dialog"], [data-popover-content]',
    ) ||
    document.querySelector('[data-dialog-content][data-state="open"]')
  ) {
    cancelBoundaryScroll();
    return;
  }
  event.preventDefault();
  const edge = ["ArrowUp", "Home"].includes(event.key) ? "start" : "end";
  scrollToDocumentBoundary(edge);
}

/** Animate and render the new virtual range in the same frame. Independent
 * animation callbacks leave the lists following the scroll event a frame late. */
export function scrollToDocumentBoundary(edge: "start" | "end"): () => void {
  const from = window.scrollY;
  const edgePosition =
    edge === "start" ? 0 : document.documentElement.scrollHeight - window.innerHeight;
  const intent = {
    edge,
    started: performance.now(),
    from,
    duration: window.matchMedia("(prefers-reduced-motion: reduce)").matches
      ? 0
      : scrollMotionDuration(edgePosition - from, window.innerHeight),
    target: -1,
    progress: 0,
    stableFrames: 0,
  };
  boundary = intent;
  if (!intent.duration) window.scrollTo({ top: Math.max(0, edgePosition), behavior: "instant" });
  requestVirtualFrame(true);
  return () => {
    if (boundary === intent) cancelBoundaryScroll();
  };
}

export function virtualViewport(): VirtualViewport {
  return {
    scrollTop: window.scrollY,
    height: window.innerHeight,
    width: window.innerWidth,
    velocity: 0,
    direction: 0,
    interaction: true,
    dragSource,
  };
}

/** All nested lists read the viewport in one frame, before Svelte commits their
 * ranges. Interaction changes refresh pins; ordinary scrolls reuse them. */
export function requestVirtualFrame(interaction = false) {
  interactionPending ||= interaction;
  if (frame || (!subscribers.size && !reads.size && !boundary)) return;
  frame = requestAnimationFrame((now) => {
    frame = 0;
    settleBoundaryScroll(now);
    // Read all geometry before Svelte commits any of the new list ranges.
    // Separate rAF callbacks can run after that microtask and force layout.
    const pendingReads = [...reads];
    reads.clear();
    for (const read of pendingReads) read();
    const next = virtualViewport();
    const distance = next.scrollTop - previousTop;
    next.velocity = Math.abs(distance) / Math.max(16, now - previousTime);
    next.direction = Math.sign(distance);
    next.interaction = interactionPending;
    interactionPending = false;
    previousTop = next.scrollTop;
    previousTime = now;
    for (const subscriber of subscribers) subscriber(next);
    if (boundary) requestVirtualFrame();
  });
}

export function requestViewportRead(read: () => void) {
  reads.add(read);
  requestVirtualFrame();
}

const onScroll = () => requestVirtualFrame();
const onInteraction = (event: Event) => {
  const target = event.composedPath().find((node): node is Element => node instanceof Element);
  // A modal editor cannot change pins in the document lists behind it. Reading
  // their viewport on every keystroke needlessly flushes a large editor layout.
  // Local virtual lists inside dialogs still need their focus/menu pins.
  if (target?.closest("[data-dialog-content]") && !target.closest("[data-virtual-list]")) return;
  requestVirtualFrame(true);
};
const onDragStart = (event: DragEvent) => {
  dragSource = event.target instanceof HTMLElement ? event.target : undefined;
  requestVirtualFrame(true);
};
const onDragEnd = () => {
  dragSource = undefined;
  requestVirtualFrame(true);
};

export function subscribeVirtualViewport(subscriber: Subscriber): () => void {
  subscribers.add(subscriber);
  if (!listening) {
    listening = true;
    previousTop = window.scrollY;
    previousTime = performance.now();
    window.addEventListener("scroll", onScroll, { passive: true });
    window.addEventListener("resize", onScroll);
    document.addEventListener("focusin", onInteraction);
    document.addEventListener("focusout", onInteraction);
    document.addEventListener("click", onInteraction);
    document.addEventListener("keydown", onInteraction);
    window.addEventListener("keydown", onBoundaryKey);
    window.addEventListener("wheel", cancelBoundaryScroll, { passive: true });
    window.addEventListener("touchstart", cancelBoundaryScroll, { passive: true });
    window.addEventListener("pointerdown", cancelBoundaryScroll, { passive: true });
    document.addEventListener("dragstart", onDragStart, true);
    document.addEventListener("dragend", onDragEnd, true);
  }
  return () => {
    subscribers.delete(subscriber);
    if (subscribers.size || !listening) return;
    listening = false;
    cancelAnimationFrame(frame);
    frame = 0;
    interactionPending = false;
    dragSource = undefined;
    cancelBoundaryScroll();
    window.removeEventListener("scroll", onScroll);
    window.removeEventListener("resize", onScroll);
    document.removeEventListener("focusin", onInteraction);
    document.removeEventListener("focusout", onInteraction);
    document.removeEventListener("click", onInteraction);
    document.removeEventListener("keydown", onInteraction);
    window.removeEventListener("keydown", onBoundaryKey);
    window.removeEventListener("wheel", cancelBoundaryScroll);
    window.removeEventListener("touchstart", cancelBoundaryScroll);
    window.removeEventListener("pointerdown", cancelBoundaryScroll);
    document.removeEventListener("dragstart", onDragStart, true);
    document.removeEventListener("dragend", onDragEnd, true);
    if (reads.size) requestVirtualFrame();
  };
}
