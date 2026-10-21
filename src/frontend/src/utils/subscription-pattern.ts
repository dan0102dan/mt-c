const TYPED_SOURCE_PREFIXES = new Set([
  "DOMAIN",
  "DOMAIN-SUFFIX",
  "DOMAIN-KEYWORD",
  "DOMAIN-WILDCARD",
  "DOMAIN-REGEX",
  "IP-CIDR",
  "IP-CIDR6",
]);

/** Display/validation only: retain the full source in rule.rule and API payloads.
 * Its prefix distinguishes equal patterns of different types and survives
 * user type overrides, refresh, export/import and cache reload.
 */
export function subscriptionRulePattern(source: string): string {
  const comma = source.indexOf(",");
  return comma >= 0 && TYPED_SOURCE_PREFIXES.has(source.slice(0, comma))
    ? source.slice(comma + 1)
    : source;
}
