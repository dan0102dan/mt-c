/** Zero velocity and acceleration at both ends avoids a sudden initial jump. */
export function scrollMotionProgress(progress: number): number {
  const t = Math.max(0, Math.min(1, progress));
  return t * t * t * (t * (t * 6 - 15) + 10);
}

/** Keep nearby moves quick, allowing a few more frames for long document jumps. */
export function scrollMotionDuration(distance: number, viewportHeight: number): number {
  const screens = Math.abs(distance) / Math.max(1, viewportHeight);
  return Math.min(480, 200 + Math.log2(1 + screens) * 36);
}
