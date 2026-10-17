import { parse } from "valibot";

import { GroupSchema, ProfileSchema, type Group, type Profile, type Subscription } from "../../types";
import { profileError } from "../settings/profiles-data";
import {
  applyRuleOverrides,
  copy,
  type Json,
  type PortableConfig,
  type ProfileDefinition,
  type RuleOverride,
  type Settings,
  type Snapshot,
  type SubscriptionDefinition,
} from "./model";

export type SavedSubscription = {
  id: string;
  name: string;
  url: string;
  interface: string;
  profile?: string;
  enable: boolean;
  priority: number;
  interval: number;
  last_update: number;
  rules: { id: string; rule: string; type: string; enable: boolean }[];
  overrides?: RuleOverride[];
};
export type ServerConfig = {
  configVersion: string;
  app: Settings;
  groups: Group[];
  profiles?: ProfileDefinition[];
  subscriptions: SavedSubscription[];
};
export type ServerSnapshot = { revision: string; config: ServerConfig };
export type TransferPolicy = { enabled: boolean; policy_id: string; mode: "include" | "exclude" };

function object(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function checkJson(value: unknown, depth = 0): asserts value is Json {
  if (depth > 24) throw new Error("Configuration nesting is too deep");
  if (value === null || typeof value === "string" || typeof value === "boolean") return;
  if (typeof value === "number" && Number.isFinite(value)) return;
  if (Array.isArray(value)) {
    for (const item of value) checkJson(item, depth + 1);
    return;
  }
  if (!object(value)) throw new Error("Unsupported configuration value");
  for (const [key, child] of Object.entries(value)) {
    if (["__proto__", "prototype", "constructor"].includes(key))
      throw new Error("Invalid configuration setting");
    checkJson(child, depth + 1);
  }
}

function subscription(value: unknown): SubscriptionDefinition {
  if (!object(value)) throw new Error("Invalid subscription");
  const id = value.id;
  if (
    typeof id !== "string" ||
    !/^[a-f\d]{8}$/i.test(id) ||
    typeof value.name !== "string" ||
    typeof value.url !== "string" ||
    typeof value.interface !== "string" ||
    (value.profile !== undefined && typeof value.profile !== "string") ||
    typeof value.enable !== "boolean" ||
    !Number.isInteger(value.priority) ||
    (value.priority as number) < 1 ||
    (value.priority as number) > 999 ||
    !Number.isInteger(value.interval) ||
    (value.interval as number) < 0 ||
    (value.interval as number) > 4294967295
  ) throw new Error("Invalid subscription metadata");
  if (value.overrides !== undefined && !Array.isArray(value.overrides)) throw new Error("Invalid overrides");
  const overrides: RuleOverride[] = [];
  const seen = new Set<string>();
  for (const entry of (value.overrides ?? []) as unknown[]) {
    if (!object(entry) || typeof entry.rule !== "string" || !entry.rule ||
      entry.rule.length > 4096 ||
      (entry.type !== undefined && typeof entry.type !== "string") ||
      (entry.enable !== undefined && typeof entry.enable !== "boolean") ||
      (entry.type === undefined && entry.enable === undefined) ||
      seen.has(entry.rule)) throw new Error("Invalid or duplicate rule override");
    seen.add(entry.rule);
    overrides.push({
      rule: entry.rule,
      ...(entry.type !== undefined ? { type: entry.type as string } : {}),
      ...(entry.enable !== undefined ? { enable: entry.enable as boolean } : {}),
    });
  }
  return {
    id: id as string,
    name: value.name as string,
    url: value.url as string,
    interface: value.interface as string,
    ...(value.profile !== undefined ? { profile: value.profile as string } : {}),
    enable: value.enable as boolean,
    priority: value.priority as number,
    interval: value.interval as number,
    ...(value.overrides ? { overrides } : {}),
  };
}

export function decodePortable(text: string): PortableConfig {
  if (text.length > 20 * 1024 * 1024) throw new Error("Configuration file exceeds 20 MB");
  const value: unknown = JSON.parse(text);
  if (!object(value)) throw new Error("Not a configuration object");
  if (value.format !== undefined && value.format !== "mt-c-config")
    throw new Error("Unsupported configuration format");
  if (value.schemaVersion !== undefined && value.schemaVersion !== 1)
    throw new Error("Unsupported configuration version");
  for (const key of Object.keys(value)) {
    if (!["format", "schemaVersion", "exportedAt", "source", "groups", "subscriptions", "profiles", "settings"].includes(key))
      throw new Error("Unsupported configuration field: " + key);
  }
  if (!["groups", "subscriptions", "profiles", "settings"].some(key => key in value))
    throw new Error("Configuration has no sections");
  const result: PortableConfig = {};
  if (value.groups !== undefined) {
    if (!Array.isArray(value.groups)) throw new Error("Invalid groups list");
    result.groups = value.groups.map((g) => parse(GroupSchema, g));
  }
  if (value.subscriptions !== undefined) {
    if (!Array.isArray(value.subscriptions)) throw new Error("Invalid subscriptions list");
    result.subscriptions = value.subscriptions.map(subscription);
  }
  if (value.profiles !== undefined) {
    if (!Array.isArray(value.profiles)) throw new Error("Invalid profiles list");
    result.profiles = value.profiles.map((p) => parse(ProfileSchema, p));
    const error = profileError(result.profiles);
    if (error) throw new Error(error);
  }
  if (value.settings !== undefined) {
    if (!object(value.settings)) throw new Error("Invalid settings");
    checkJson(value.settings);
    result.settings = copy(value.settings as Settings);
  }
  return result;
}

export function fromSaved(s: SavedSubscription): SubscriptionDefinition {
  return {
    id: s.id, name: s.name, url: s.url, interface: s.interface,
    ...(s.profile ? { profile: s.profile } : {}),
    enable: s.enable, priority: s.priority, interval: s.interval,
    ...(s.overrides?.length ? { overrides: copy(s.overrides) } : {}),
  };
}

export function fromEditor(s: Subscription): SubscriptionDefinition {
  const data = s as Subscription & { overrides?: RuleOverride[] };
  return {
    id: s.id, name: s.name, url: s.url, interface: s.interface,
    ...(s.profile ? { profile: s.profile } : {}),
    enable: s.enable, priority: s.priority ?? 100, interval: s.interval ?? 86400,
    ...(data.overrides?.length ? { overrides: copy(data.overrides) } : {}),
  };
}

export function snapshotFromServer(
  server: ServerConfig,
  drafts: {
    groups?: readonly Group[];
    subscriptions?: readonly Subscription[];
    profiles?: readonly Profile[];
    policy?: TransferPolicy;
  } = {},
): Snapshot {
  const app = copy(server.app);
  if (drafts.policy && object(app.keeneticPolicy)) {
    app.keeneticPolicy = {
      enabled: drafts.policy.enabled,
      policyId: drafts.policy.policy_id,
      mode: drafts.policy.mode,
    };
  }
  return {
    groups: copy(drafts.groups ?? server.groups),
    subscriptions: drafts.subscriptions
      ? drafts.subscriptions.map(fromEditor)
      : server.subscriptions.map(fromSaved),
    profiles: copy(drafts.profiles ?? server.profiles ?? []).map(({ id, name, interfaces, on_unavailable }) => ({
      id, name, interfaces: [...interfaces], on_unavailable,
    })),
    settings: app,
  };
}

/** Restore download caches ONLY for the same source (same ID and URL).
 * Newly added subscriptions have no rules until their first synchronization. */
export function configForApply(server: ServerConfig, target: Snapshot): ServerConfig {
  const previous = new Map(server.subscriptions.map((s) => [s.id, s]));
  return {
    configVersion: server.configVersion,
    app: copy(target.settings),
    groups: copy(target.groups),
    profiles: copy(target.profiles),
    subscriptions: target.subscriptions.map((s) => {
      const old = previous.get(s.id);
      const retained = old?.url === s.url ? old : undefined;
      const overrides = s.overrides ?? [];
      const rules = applyRuleOverrides(retained?.rules ?? [], overrides).rules;
      return {
        id: s.id, name: s.name, url: s.url, interface: s.interface,
        ...(s.profile ? { profile: s.profile } : {}),
        enable: s.enable, priority: s.priority, interval: s.interval,
        last_update: retained?.last_update ?? 0,
        rules: copy(rules),
        ...(overrides.length ? { overrides: copy(overrides) } : {}),
      };
    }),
  };
}
