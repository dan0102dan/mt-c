/** Reuse inactive row component instances while retaining the slot belonging to
 * any logical row still in the rendered range (including pinned editors). */
export class VirtualRowPool {
  private readonly slots = new Map<string, number>();
  private readonly free: number[] = [];
  private order: number[] = [];
  private next = 0;

  assign(keys: readonly string[], canReuse: (slot: number) => boolean): number[] {
    const wanted = new Set(keys);
    const retained = new Set<number>();
    for (const [key, slot] of this.slots) {
      if (wanted.has(key)) {
        retained.add(slot);
        continue;
      }
      this.slots.delete(key);
    }
    // Reuse mounted slots in their DOM order. A stack reverses every disjoint
    // range, making Svelte move all the reused rows during a large jump.
    const available = [...this.order.filter((slot) => !retained.has(slot)), ...this.free];
    this.free.length = 0;
    let cursor = 0;
    const assigned = keys.map((key) => {
      const existing = this.slots.get(key);
      if (existing !== undefined) return existing;
      let slot: number | undefined;
      while (cursor < available.length) {
        const candidate = available[cursor++];
        if (canReuse(candidate)) {
          slot = candidate;
          break;
        }
        this.free.push(candidate);
      }
      slot ??= this.next++;
      this.slots.set(key, slot);
      return slot;
    });
    this.free.push(...available.slice(cursor));
    this.order = assigned;
    return assigned;
  }
}
