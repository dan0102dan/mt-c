import { newProfileId } from "../settings/profiles-data";
import {
  conflictKey,
  planImport,
  sections,
  type Choice,
  type Collections,
  type ImportPlan,
  type PortableConfig,
  type Section,
  type Snapshot,
} from "./model";

export type ImportSelection = {
  version: number;
  mode: "merge" | "replace";
  excluded: string[];
  choices: [string, Choice][];
  policyId?: string;
};
export type ImportPreview = Pick<ImportPlan, "problems" | "conflicts"> & {
  next: {
    groups: { id: string; name: string; profile?: string }[];
    subscriptions: { id: string; name: string; profile?: string }[];
    profiles: { id: string; name: string }[];
  };
  changes: Pick<ImportPlan["changes"][number], "section" | "id" | "kind">[];
};
export type PlannerRequest =
  | { type: "init"; current: Snapshot; imported: PortableConfig }
  | ({ type: "plan" } & ImportSelection)
  | { type: "apply"; version: number };
export type PlannerResponse =
  | { type: "preview"; version: number; preview: ImportPreview }
  | { type: "apply"; version: number; plan: ImportPlan }
  | { type: "error"; version: number; message: string };

/** One immutable import session. Large snapshots stay in the worker while the
 * UI sends only selection changes and receives small preview metadata. */
export class ImportPlannerSession {
  private readonly reserved = new Set<string>();
  private readonly allocated = new Map<string, string>();
  private prepared?: { version: number; plan: ImportPlan };

  constructor(
    private readonly current: Snapshot,
    private readonly imported: PortableConfig,
  ) {
    for (const source of [current, imported]) {
      for (const section of ["groups", "subscriptions", "profiles"] as const)
        for (const item of source[section] ?? []) this.reserved.add(item.id);
      for (const group of source.groups ?? [])
        for (const rule of group.rules) this.reserved.add(rule.id);
    }
  }

  private newId = (section: keyof Collections, old: string): string => {
    const key = conflictKey(section, old);
    let id = this.allocated.get(key);
    if (id) return id;
    do {
      if (section === "profiles") id = newProfileId();
      else {
        const bytes = crypto.getRandomValues(new Uint8Array(4));
        id = [...bytes].map((value) => value.toString(16).padStart(2, "0")).join("");
      }
    } while (this.reserved.has(id));
    this.allocated.set(key, id);
    this.reserved.add(id);
    return id;
  };

  preview(selection: ImportSelection): ImportPreview {
    const excluded = new Set(selection.excluded);
    const selected = new Set<Section>();
    for (const section of sections) {
      const items = this.imported[section];
      if (items === undefined) continue;
      if (section === "settings") {
        if (
          Object.keys(this.imported.settings ?? {}).some((key) => !excluded.has(`settings:${key}`))
        )
          selected.add(section);
      } else {
        const list = this.imported[section] ?? [];
        if (
          !list.length
            ? !excluded.has(`${section}:empty`)
            : list.some((_, index) => !excluded.has(`${section}:${index}`))
        )
          selected.add(section);
      }
    }
    const groups = this.imported.groups?.filter(
      (_, index) => selected.has("groups") && !excluded.has(`groups:${index}`),
    );
    const subscriptions = this.imported.subscriptions?.filter(
      (_, index) => selected.has("subscriptions") && !excluded.has(`subscriptions:${index}`),
    );
    const referenced = new Set(
      [...(groups ?? []), ...(subscriptions ?? [])].map((item) => item.profile),
    );
    const input: PortableConfig = {
      ...this.imported,
      groups,
      subscriptions,
      profiles: this.imported.profiles?.filter(
        (item, index) =>
          (selected.has("profiles") && !excluded.has(`profiles:${index}`)) ||
          referenced.has(item.id),
      ),
      settings:
        this.imported.settings === undefined
          ? undefined
          : Object.fromEntries(
              Object.entries(this.imported.settings).filter(
                ([key]) => !excluded.has(`settings:${key}`),
              ),
            ),
    };
    const plan = planImport(this.current, input, {
      mode: selection.mode,
      selected,
      choices: new Map(selection.choices),
      policyId: selection.policyId,
      newId: this.newId,
    });
    this.prepared = { version: selection.version, plan };
    return {
      problems: plan.problems,
      conflicts: plan.conflicts,
      next: {
        groups: plan.next.groups.map(({ id, name, profile }) => ({ id, name, profile })),
        subscriptions: plan.next.subscriptions.map(({ id, name, profile }) => ({
          id,
          name,
          profile,
        })),
        profiles: plan.next.profiles.map(({ id, name }) => ({ id, name })),
      },
      changes: plan.changes.map(({ section, id, kind }) => ({ section, id, kind })),
    };
  }

  apply(version: number): ImportPlan {
    if (!this.prepared || this.prepared.version !== version)
      throw new Error("Configuration changed; refresh the import preview");
    return this.prepared.plan;
  }
}
