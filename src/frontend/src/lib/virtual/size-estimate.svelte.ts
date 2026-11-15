export type VirtualMeasurement = { index: number; size: number };

/** Share the usual row size across panels without making their content fixed
 * height. Individual measurements still belong to each VirtualList. */
export class SizeEstimate {
  #desktop = $state<number>();
  #mobile = $state<number>();

  constructor(private readonly fallback: { desktop: number; mobile: number }) {}

  forWidth(width: number): number {
    return width <= 700
      ? (this.#mobile ?? this.fallback.mobile)
      : (this.#desktop ?? this.fallback.desktop);
  }

  learn(width: number, measurements: readonly VirtualMeasurement[]) {
    const sizes = measurements
      .map(({ size }) => size)
      .filter((size) => Number.isFinite(size) && size > 0)
      .sort((a, b) => a - b);
    if (sizes.length < 2) return;

    // A single wrapped row must not change the estimate for the whole list.
    // ResizeObserver reports the visible batch again after width/font changes,
    // so a new common size can replace the old one without resetting to a guess.
    let start = 0;
    let bestStart = 0;
    let bestLength = 0;
    for (let end = 0; end < sizes.length; end++) {
      while (sizes[end] - sizes[start] > 0.1) start++;
      const length = end - start + 1;
      if (length > bestLength) {
        bestStart = start;
        bestLength = length;
      }
    }
    if (bestLength < 2 || bestLength < sizes.length * 0.75) return;
    const size = sizes[bestStart + Math.floor(bestLength / 2)];
    if (Math.abs(size - this.forWidth(width)) < 0.1) return;
    if (width <= 700) this.#mobile = size;
    else this.#desktop = size;
  }
}
