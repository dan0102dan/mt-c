type RegexRule = { id: string; type: string; rule: string };

const indexes = new WeakMap<readonly RegexRule[], Map<string, string>>();

/** Server snapshots are immutable accepted baselines, not JavaScript regexes.
 * Trust only the same identity, type and source; edits still use UI validation. */
export function isAcceptedRegex(rule: RegexRule, baseline?: readonly RegexRule[]): boolean {
  if (rule.type !== "regex" || !rule.rule || !baseline) return false;
  let index = indexes.get(baseline);
  if (!index) {
    index = new Map(
      baseline.filter((item) => item.type === "regex").map((item) => [item.id, item.rule]),
    );
    indexes.set(baseline, index);
  }
  return index.get(rule.id) === rule.rule;
}
