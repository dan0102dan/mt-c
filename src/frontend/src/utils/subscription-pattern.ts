const TYPED_SOURCE_PREFIXES = new Set([
  "DOMAIN",
  "DOMAIN-SUFFIX",
  "DOMAIN-KEYWORD",
  "DOMAIN-WILDCARD",
  "DOMAIN-REGEX",
  "IP-CIDR",
  "IP-CIDR6",
]);

/** Display/validation only: retain the original source in rule.rule and API payloads.
 * The source prefix preserves identity through refresh and cache recovery.
 */
export function subscriptionRulePattern(source: string, type?: string): string {
  const comma = source.indexOf(",");
  if (comma < 0) return source;
  const prefix = source.slice(0, comma);
  if (!TYPED_SOURCE_PREFIXES.has(prefix)) return source;
  const payload = source.slice(comma + 1);
  if (prefix === "DOMAIN-KEYWORD" && type === "wildcard") {
    // The wildcard matches an entire domain; source metacharacters stay literal.
    return `*${payload.replace(/[\\*?]/g, "\\$&")}*`;
  }
  return payload;
}
