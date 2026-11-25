import type { Group, Rule } from "../../types";

export function isBlankGroupRule(rule: Rule) {
  return !rule.rule.trim() && !rule.name?.trim();
}

export function countBlankGroupRules(groups: readonly Group[]) {
  let count = 0;
  for (const group of groups) {
    for (const rule of group.rules) {
      if (isBlankGroupRule(rule)) count++;
    }
  }
  return count;
}

export function groupForSave(group: Group): Group {
  return { ...group, rules: group.rules.filter((rule) => !isBlankGroupRule(rule)) };
}

export function buildGroupUpdate(group: Group, baseline: readonly Rule[] | undefined) {
  const { rules, ...metadata } = groupForSave(group);
  if (
    !baseline ||
    baseline.length !== rules.length ||
    baseline.some((rule, index) => rule.id !== rules[index].id)
  ) {
    return { ...metadata, rules }; // New/imported/moved/removed rules: preserve full ordered replacement.
  }
  const ruleChanges = [];
  for (let i = 0; i < rules.length; i++) {
    const before = baseline[i],
      after = rules[i];
    if (
      before.name !== after.name ||
      before.type !== after.type ||
      before.rule !== after.rule ||
      before.enable !== after.enable
    ) {
      ruleChanges.push({
        ...after,
        previous: {
          name: before.name,
          type: before.type,
          rule: before.rule,
          enable: before.enable,
        },
      });
    }
  }
  return { ...metadata, ruleChanges };
}

export function snapshotGroupRules(groups: readonly Group[]) {
  return new Map(groups.map((group) => [group.id, group.rules.map((rule) => ({ ...rule }))]));
}
