export const PAGE_CONTROLS_MOTION = Symbol("page-controls-motion");

// Per application, non-reactive history: inactive toolbars must not replay or
// overwrite the last visible toolbar's Add state.
export type PageControlsMotion = { hasAdd?: boolean };
