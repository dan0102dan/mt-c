import type { Rule } from "../../types";
import {
  isValidDomain,
  isValidNamespace,
  isValidRegex,
  isValidSubnet,
  isValidSubnet6,
  isValidWildcard,
  VALIDATOP_MAP,
} from "../../utils/rule-validators";

export type RuleListType = Rule["type"] | "auto";
export type ParsedRuleLine = {
  text: string;
  type: Rule["type"] | "INVALID" | null;
  isValid: boolean;
};
export type RuleListPreview = {
  lines: ParsedRuleLine[];
  totalLines: number;
  stats: Record<string, number>;
};
export type ParsedRuleList = RuleListPreview & {
  entries: { rule: string; type: Rule["type"] }[];
};
export type RuleListJob =
  | { type: "parse"; version: number; text: string; ruleType: RuleListType }
  | { type: "apply"; version: number };
export type RuleListRequest = RuleListJob & { id: number };
export type RuleListResponse = { id: number; version: number } & (
  | { type: "preview"; preview: RuleListPreview }
  | { type: "rules"; rules: Rule[] }
  | { type: "error"; message: string }
);

export function detectRuleListType(pattern: string): Rule["type"] | null {
  if (isValidSubnet6(pattern)) return "subnet6";
  if (isValidSubnet(pattern)) return "subnet";
  if (isValidNamespace(pattern)) return "namespace";
  if (isValidDomain(pattern)) return "domain";
  if (isValidRegex(pattern)) return "regex";
  if (isValidWildcard(pattern)) return "wildcard";
  return null;
}

/** Preserve the existing separator/detection order; only the bounded preview
 * crosses back to the UI. Complete unique rules remain in its worker session. */
export function parseRuleList(
  text: string,
  ruleType: RuleListType,
  previewLimit = 500,
): ParsedRuleList {
  const lines: ParsedRuleLine[] = [];
  const entries: ParsedRuleList["entries"] = [];
  const stats: Record<string, number> = {};
  const seen = new Set<string>();
  let totalLines = 0;
  for (const line of text.split(/[\n,]+/)) {
    const trimmed = line.trim();
    let type: ParsedRuleLine["type"] = null;
    if (trimmed && !trimmed.startsWith("#")) {
      type =
        ruleType === "auto"
          ? detectRuleListType(trimmed)
          : VALIDATOP_MAP[ruleType]?.(trimmed)
            ? ruleType
            : null;
      stats[type || "INVALID"] = (stats[type || "INVALID"] || 0) + 1;
      if (type) {
        const key = `${type}|${trimmed}`;
        if (!seen.has(key)) {
          seen.add(key);
          entries.push({ rule: trimmed, type });
        }
      } else type = "INVALID";
    }
    if (lines.length < previewLimit)
      lines.push({ text: line, type, isValid: Boolean(type && type !== "INVALID") });
    totalLines++;
  }
  return { lines, totalLines, stats, entries };
}

export function ruleListPreview(parsed: ParsedRuleList): RuleListPreview {
  return { lines: parsed.lines, totalLines: parsed.totalLines, stats: parsed.stats };
}

export function buildImportedRules(parsed: ParsedRuleList, newId?: () => string): Rule[] {
  const reserved = new Set<string>();
  // Keep collision-safe random IDs without 100k separate crypto calls and
  // short-lived typed arrays when applying a large native rule list.
  const random = new Uint32Array(4096);
  let offset = random.length;
  const nextId =
    newId ??
    (() => {
      if (offset === random.length) {
        crypto.getRandomValues(random);
        offset = 0;
      }
      return random[offset++].toString(16).padStart(8, "0");
    });
  return parsed.entries.map(({ rule, type }) => {
    let id: string;
    do id = nextId();
    while (reserved.has(id));
    reserved.add(id);
    return { id, name: "", rule, type, enable: true };
  });
}

/** Height saturates after a few visible lines; never split a multi-megabyte
 * textarea merely to decide that it already needs the maximum height. */
export function boundedRuleListLines(text: string, limit: number): number {
  let lines = 0;
  const tokens = /[^\n,]+/g;
  let match: RegExpExecArray | null;
  while ((match = tokens.exec(text))) {
    if (match[0].trim() && ++lines >= limit) return lines;
  }
  return Math.max(1, lines);
}
