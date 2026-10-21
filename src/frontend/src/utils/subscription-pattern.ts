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
export function subscriptionRulePattern(source: string, type?: string): string {
  const comma = source.indexOf(",");
  if (comma < 0) return source;
  const prefix = source.slice(0, comma);
  if (!TYPED_SOURCE_PREFIXES.has(prefix)) return source;
  const payload = source.slice(comma + 1);
  if (prefix === "DOMAIN-KEYWORD" && type === "wildcard") {
    // Preserve literal '*' and '?' in a keyword's displayed wildcard pattern.
    return `*${payload.replace(/[\\\\*?]/g, "\\\\  return prefix === "DOMAIN-KEYWORD" && type === "wildcard" ? `*${payload}*` : payload;")}*`;
  }
  return payload;
}
