import type { Group, Profile, Subscription } from "../../types";
import { webSettings } from "./web-settings";

export type Json = null | boolean | number | string | Json[] | { [key: string]: Json };
export type Settings = { [key: string]: Json };
export type RuleOverride = { rule: string; enable?: boolean; type?: string };
export type SubscriptionDefinition = Pick<
  Subscription,
  "id" | "name" | "url" | "interface" | "profile" | "enable" | "priority" | "interval"
> & { overrides?: RuleOverride[] };
export type ProfileDefinition = Omit<Profile, "usage">;
export type Section = "groups" | "subscriptions" | "profiles" | "settings";
export const sections: readonly Section[] = ["groups", "subscriptions", "profiles", "settings"];
export type Collections = {
  groups: Group[];
  subscriptions: SubscriptionDefinition[];
  profiles: ProfileDefinition[];
};
export type Snapshot = Collections & { settings: Settings };
export type PortableConfig = Partial<Snapshot> & {
  format?: "mt-c-config";
  schemaVersion?: 1;
  exportedAt?: string;
  source?: { application: string; version?: string; platform?: string };
};
export type PolicySettings = { enabled: boolean; policy_id: string; mode: "include" | "exclude" };

/** Copy only configuration fields. Do not validate, normalize, save or fetch here:
 * exporting an unfinished draft must preserve exactly what the user is editing. */
export function exportSnapshot(
  snapshot: Snapshot,
  source: NonNullable<PortableConfig["source"]>,
  exportedAt = new Date().toISOString(),
): PortableConfig {
  const settings = webSettings(snapshot.settings);
  return {
    format: "mt-c-config",
    schemaVersion: 1,
    exportedAt,
    source: { ...source },
    groups: snapshot.groups.map((g) => ({
      id: g.id,
      name: g.name,
      color: g.color,
      interface: g.interface,
      ...(g.profile !== undefined ? { profile: g.profile } : {}),
      enable: g.enable,
      priority: g.priority,
      rules: g.rules.map(({ id, name, rule, type, enable }) => ({ id, name, rule, type, enable })),
    })),
    subscriptions: snapshot.subscriptions.map((s) => ({
      id: s.id,
      name: s.name,
      url: s.url,
      interface: s.interface,
      ...(s.profile !== undefined ? { profile: s.profile } : {}),
      enable: s.enable,
      priority: s.priority,
      interval: s.interval,
      ...(s.overrides
        ? {
            overrides: s.overrides.map(({ rule, enable, type }) => ({
              rule,
              ...(enable !== undefined ? { enable } : {}),
              ...(type !== undefined ? { type } : {}),
            })),
          }
        : {}),
    })),
    profiles: snapshot.profiles.map(({ id, name, interfaces, on_unavailable }) => ({
      id,
      name,
      interfaces: [...interfaces],
      on_unavailable,
    })),
    ...(Object.keys(settings).length ? { settings: copy(settings) } : {}),
  };
}

export function copy<T>(value: T): T {
  // structuredClone cannot clone Svelte proxies. All inputs are JSON-shaped.
  return JSON.parse(JSON.stringify(value)) as T;
}

/** Object property order is irrelevant; array order (especially failover) is not. */
export function fingerprint(value: unknown): string {
  if (value === undefined) return "undefined";
  if (value === null || typeof value !== "object") return JSON.stringify(value);
  if (Array.isArray(value)) return `[${value.map(fingerprint).join(",")}]`;
  const record = value as Record<string, unknown>;
  return `{${Object.keys(record)
    .filter((key) => record[key] !== undefined)
    .sort()
    .map((key) => `${JSON.stringify(key)}:${fingerprint(record[key])}`)
    .join(",")}}`;
}

function assertSafeSettings(value: Json): void {
  if (value === null || typeof value !== "object") return;
  for (const [key, child] of Object.entries(value)) {
    if (["__proto__", "prototype", "constructor"].includes(key)) {
      throw new Error(`Invalid settings key: ${key}`);
    }
    assertSafeSettings(child);
  }
}

function overlaySettings(current: Settings, incoming: Settings): Settings {
  assertSafeSettings(incoming);
  const result = copy(current);
  for (const [key, value] of Object.entries(incoming)) {
    const old = result[key];
    result[key] =
      value &&
      typeof value === "object" &&
      !Array.isArray(value) &&
      old &&
      typeof old === "object" &&
      !Array.isArray(old)
        ? overlaySettings(old, value)
        : copy(value);
  }
  return result;
}

export type FieldChange = { field: string; before: unknown; after: unknown };
export type Change = {
  section: Section;
  id: string;
  name: string;
  kind: "add" | "update" | "remove" | "unchanged";
  fields: FieldChange[];
};
export type Conflict = { section: keyof Collections; id: string; name: string };
export type Choice = "keep" | "replace" | "copy";
export type ImportOptions = {
  mode: "merge" | "replace";
  selected: ReadonlySet<Section>;
  choices?: ReadonlyMap<string, Choice>;
  interfaceMap?: ReadonlyMap<string, string>;
  policyId?: string;
  /** IDs are allocated by the import session, not on every reactive preview. */
  newId: (section: keyof Collections, originalId: string) => string;
};
export type ImportPlan = {
  baseline: string;
  next: Snapshot;
  changes: Change[];
  conflicts: Conflict[];
  problems: string[];
  includedDependencies: string[];
};
export const conflictKey = (section: keyof Collections, id: string) => `${section}:${id}`;

function changedFields(before: object, after: object): FieldChange[] {
  const a = before as Record<string, unknown>;
  const b = after as Record<string, unknown>;
  return [...new Set([...Object.keys(a), ...Object.keys(b)])]
    .filter((field) => fingerprint(a[field]) !== fingerprint(b[field]))
    .map((field) => ({ field, before: a[field], after: b[field] }));
}

function changedSettings(before: Settings, after: Settings, prefix = ""): FieldChange[] {
  return changedFields(before, after).flatMap((change) => {
    const field = prefix + change.field;
    const a = change.before;
    const b = change.after;
    if (
      a &&
      b &&
      typeof a === "object" &&
      typeof b === "object" &&
      !Array.isArray(a) &&
      !Array.isArray(b)
    ) {
      return changedSettings(a as Settings, b as Settings, `${field}.`);
    }
    return [{ ...change, field }];
  });
}

export function diffSnapshots(before: Snapshot, after: Snapshot): Change[] {
  const changes: Change[] = [];
  for (const section of ["groups", "subscriptions", "profiles"] as const) {
    const previous = new Map(before[section].map((item) => [item.id, item]));
    for (const item of after[section]) {
      const old = previous.get(item.id);
      const fields = old ? changedFields(old, item) : [];
      changes.push({
        section,
        id: item.id,
        name: item.name,
        kind: old ? (fields.length ? "update" : "unchanged") : "add",
        fields,
      });
      previous.delete(item.id);
    }
    for (const item of previous.values()) {
      changes.push({ section, id: item.id, name: item.name, kind: "remove", fields: [] });
    }
  }
  for (const field of changedSettings(before.settings, after.settings)) {
    changes.push({
      section: "settings",
      id: field.field,
      name: field.field,
      kind: field.after === undefined ? "remove" : field.before === undefined ? "add" : "update",
      fields: [field],
    });
  }
  return changes;
}

/** A plan has no I/O and never modifies either input. Absence is not emptiness.
 * Inputs must already be decoded and validated by the adapter. The planner does
 * not establish platform support, available interfaces or an authoritative server revision. */
export function planImport(
  current: Snapshot,
  incoming: PortableConfig,
  options: ImportOptions,
): ImportPlan {
  const next = copy(current);
  const problems: string[] = [];
  const conflicts: Conflict[] = [];
  const includedDependencies: string[] = [];
  const profileIds = new Map<string, string>();
  const definitionIds = new Set<string>();
  const remap = (name: string) => options.interfaceMap?.get(name) ?? name;
  const selected = <K extends Section>(section: K) =>
    options.selected.has(section) && incoming[section] !== undefined;
  const importedProfiles = copy(incoming.profiles ?? []);
  const referenced = new Set<string>();
  for (const section of ["groups", "subscriptions"] as const) {
    if (!selected(section)) continue;
    for (const item of incoming[section] ?? []) {
      if (item.profile) referenced.add(item.profile);
    }
  }

  function merge<T extends { id: string; name: string }>(
    section: keyof Collections,
    previous: T[],
    imported: T[],
    replaceSection: boolean,
  ): T[] {
    // Preserve order and identity on merge. Matching URL/name alone is not identity.
    const result = replaceSection ? [] : copy(previous);
    const seen = new Set<string>();
    for (const raw of imported) {
      const item = copy(raw);
      if (seen.has(item.id)) {
        problems.push(`Duplicate ${section} ID: ${item.id}`);
        continue;
      }
      seen.add(item.id);
      const index = result.findIndex((entry) => entry.id === item.id);
      if (index < 0) {
        result.push(item);
      } else if (fingerprint(result[index]) !== fingerprint(item)) {
        conflicts.push({ section, id: item.id, name: item.name });
        const choice = options.choices?.get(conflictKey(section, item.id)) ?? "keep";
        if (choice === "replace") result[index] = item;
        if (choice === "copy") {
          const oldId = item.id;
          item.id = options.newId(section, oldId);
          const collision = [...previous, ...result, ...imported].some((p) => p.id === item.id);
          if (collision) {
            problems.push(`Allocated ${section} ID already exists: ${item.id}`);
            continue;
          }
          if (section === "profiles") profileIds.set(oldId, item.id);
          result.push(item);
        }
      }
    }
    return result;
  }

  const chosenProfiles = importedProfiles.filter(
    (p) => selected("profiles") || referenced.has(p.id),
  );
  for (const p of chosenProfiles) {
    if (definitionIds.has(p.id)) problems.push(`Duplicate profiles ID: ${p.id}`);
    definitionIds.add(p.id);
    if (!selected("profiles")) includedDependencies.push(p.id);
    p.interfaces = p.interfaces.map(remap);
    if (new Set(p.interfaces).size !== p.interfaces.length) {
      problems.push(`Profile contains duplicate interfaces after mapping: ${p.name}`);
    }
  }
  next.profiles = merge(
    "profiles",
    current.profiles,
    chosenProfiles,
    selected("profiles") && options.mode === "replace",
  );
  const profilesById = new Map(next.profiles.map((p) => [p.id, p]));
  for (const section of ["groups", "subscriptions"] as const) {
    if (!selected(section)) continue;
    const imported = (incoming[section] ?? []).map((raw) => {
      const item = copy(raw);
      item.interface = remap(item.interface);
      if (item.profile) {
        item.profile = profileIds.get(item.profile) ?? item.profile;
        const profile = profilesById.get(item.profile);
        if (profile?.interfaces.length) item.interface = profile.interfaces[0];
      }
      return item;
    });
    // Keep the collections independently typed: rules belong only to user groups.
    if (section === "groups") {
      next.groups = merge(
        "groups",
        current.groups,
        imported as Group[],
        options.mode === "replace",
      );
    } else {
      next.subscriptions = merge(
        "subscriptions",
        current.subscriptions,
        imported as SubscriptionDefinition[],
        options.mode === "replace",
      );
    }
  }
  if (selected("settings")) {
    // Missing fields, including fields introduced after an older export, survive.
    next.settings = overlaySettings(current.settings, incoming.settings ?? {});
    if (options.policyId !== undefined) {
      const policy = next.settings.keeneticPolicy;
      if (policy && typeof policy === "object" && !Array.isArray(policy)) {
        policy.policyId = options.policyId;
      }
    }
  }

  // Do not normalize unrelated drafts. Only a changed definition updates retained consumers.
  const oldProfilesById = new Map(current.profiles.map((p) => [p.id, p]));
  for (const route of [...next.groups, ...next.subscriptions]) {
    if (!route.profile) continue;
    const profile = profilesById.get(route.profile);
    if (!profile?.interfaces.length) problems.push(`Missing routing profile: ${route.profile}`);
    else if (oldProfilesById.get(profile.id)?.interfaces[0] !== profile.interfaces[0]) {
      route.interface = profile.interfaces[0];
    }
  }
  const routeIds = new Set<string>();
  for (const route of [...next.groups, ...next.subscriptions]) {
    if (routeIds.has(route.id)) problems.push(`Group/subscription ID collision: ${route.id}`);
    routeIds.add(route.id);
  }
  return {
    baseline: fingerprint(current),
    next,
    changes: diffSnapshots(current, next),
    conflicts,
    problems: [...new Set(problems)],
    includedDependencies,
  };
}

export function assertPlanCurrent(plan: ImportPlan, current: Snapshot): void {
  if (plan.baseline !== fingerprint(current)) {
    throw new Error("Configuration changed; refresh the import preview");
  }
  if (plan.problems.length) throw new Error(plan.problems.join("\n"));
}

/** Overlay fresh source rules by pattern, never by device-local rule IDs.
 * Unmatched overrides stay separate and never recreate a removed source rule. */
export function applyRuleOverrides<T extends { rule: string; type: string; enable: boolean }>(
  rules: readonly T[],
  overrides: readonly RuleOverride[],
): { rules: T[]; unmatched: RuleOverride[] } {
  const byPattern = new Map<string, RuleOverride>();
  for (const override of overrides) {
    if (byPattern.has(override.rule)) throw new Error(`Duplicate override: ${override.rule}`);
    byPattern.set(override.rule, override);
  }
  const result = rules.map((rule) => {
    const override = byPattern.get(rule.rule);
    if (!override) return { ...rule };
    return {
      ...rule,
      ...(override.type !== undefined ? { type: override.type } : {}),
      ...(override.enable !== undefined ? { enable: override.enable } : {}),
    };
  });
  const present = new Set(rules.map((rule) => rule.rule));
  return {
    rules: result,
    unmatched: overrides.filter((r) => !present.has(r.rule)).map((r) => ({ ...r })),
  };
}
