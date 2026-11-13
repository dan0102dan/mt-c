import {
  copy,
  diffSnapshots,
  fingerprint,
  type Collections,
  type ImportPlan,
  type Section,
  type Snapshot,
} from "./model";

type Collection = keyof Collections;
type ObjectChange = { id: string; fields: string[] | null };
const collections: readonly Collection[] = ["groups", "subscriptions", "profiles"];

export type PendingImportDraft = {
  baseline: Snapshot;
  sections: Section[];
  implicit: Record<Collection, ObjectChange[]>;
};

/** Remember import intent without turning unrelated editor drafts into saved data. */
export function stageImportDraft(
  serverBaseline: Snapshot,
  beforeEditor: Snapshot,
  plan: ImportPlan,
  pending?: PendingImportDraft,
  changes = diffSnapshots(beforeEditor, plan.next),
): PendingImportDraft {
  const next: PendingImportDraft = pending
    ? copy(pending)
    : {
        baseline: copy(serverBaseline),
        sections: [],
        implicit: { groups: [], subscriptions: [], profiles: [] },
      };
  next.sections = [...new Set([...next.sections, ...plan.selected])];
  function remember(section: Collection, id: string, fields: string[] | null) {
    const change = next.implicit[section].find((entry) => entry.id === id);
    if (!change) next.implicit[section].push({ id, fields });
    else if (change.fields && fields) change.fields = [...new Set([...change.fields, ...fields])];
    else change.fields = null;
  }
  // A caller that already validated the plan baseline can reuse its worker diff.
  for (const change of changes) {
    if (change.section === "settings" || change.kind === "unchanged") continue;
    if (next.sections.includes(change.section)) continue;
    const fields = change.fields
      .map((field) => field.field)
      .filter((field) => field !== "position");
    if (change.section === "profiles" || change.kind === "add" || change.kind === "remove") {
      remember(change.section, change.id, null);
    } else if (fields.length) {
      remember(change.section, change.id, fields);
    }
  }
  // A dependency may already exist only in an editor, so its definition need
  // not appear in the before/after diff. Save the chosen definition with users.
  for (const id of plan.includedDependencies) remember("profiles", id, null);
  return next;
}

function writeField(target: object, key: string, value: unknown) {
  const record = target as Record<string, unknown>;
  if (value === undefined) delete record[key];
  else record[key] = copy(value);
}

function projectCollection<T extends { id: string }>(
  baseline: readonly T[],
  current: readonly T[],
  changes: readonly ObjectChange[],
): T[] {
  const result = new Map(baseline.map((item) => [item.id, copy(item)]));
  const drafts = new Map(current.map((item) => [item.id, item]));
  for (const change of changes) {
    const draft = drafts.get(change.id);
    const saved = result.get(change.id);
    if (!draft) {
      result.delete(change.id);
    } else if (change.fields === null) {
      result.set(change.id, copy(draft));
    } else if (saved) {
      for (const field of change.fields) {
        writeField(saved, field, (draft as Record<string, unknown>)[field]);
      }
    }
  }
  return [...result.values()];
}

/** Build the atomic Save payload from the latest editable values. Explicitly
 * imported sections and the page being saved are complete collections; only
 * imported dependencies/consumer fields from other pages enter the payload. */
export function projectImportDraft(
  pending: PendingImportDraft,
  currentEditor: Snapshot,
  saveSections: Iterable<Section>,
): Snapshot {
  const selected = new Set([...pending.sections, ...saveSections]);
  const result = copy(pending.baseline);
  result.groups = selected.has("groups")
    ? copy(currentEditor.groups)
    : projectCollection(result.groups, currentEditor.groups, pending.implicit.groups);
  result.subscriptions = selected.has("subscriptions")
    ? copy(currentEditor.subscriptions)
    : projectCollection(
        result.subscriptions,
        currentEditor.subscriptions,
        pending.implicit.subscriptions,
      );
  result.profiles = selected.has("profiles")
    ? copy(currentEditor.profiles)
    : projectCollection(result.profiles, currentEditor.profiles, pending.implicit.profiles);
  if (selected.has("settings")) result.settings = copy(currentEditor.settings);

  // A dependency can be edited after import. Consumers must follow the final
  // configured primary, including saved consumers outside the current page.
  const profiles = new Map(result.profiles.map((profile) => [profile.id, profile]));
  const previousProfiles = new Map(
    pending.baseline.profiles.map((profile) => [profile.id, profile]),
  );
  for (const route of [...result.groups, ...result.subscriptions]) {
    if (!route.profile) continue;
    const profile = profiles.get(route.profile);
    if (
      profile?.interfaces.length &&
      previousProfiles.get(profile.id)?.interfaces[0] !== profile.interfaces[0]
    ) {
      route.interface = profile.interfaces[0];
    }
  }
  return result;
}

function rebaseCollection<T extends { id: string }>(
  submitted: readonly T[],
  current: readonly T[],
  confirmed: readonly T[],
): T[] {
  const before = new Map(submitted.map((item) => [item.id, item]));
  const after = new Map(current.map((item) => [item.id, item]));
  const result = new Map(
    confirmed
      .filter((item) => !before.has(item.id) || after.has(item.id))
      .map((item) => [item.id, copy(item)]),
  );
  for (const item of current) {
    const original = before.get(item.id);
    const saved = result.get(item.id);
    if (!original) {
      result.set(item.id, copy(item));
      continue;
    }
    if (!saved) continue;
    const a = original as Record<string, unknown>;
    const b = item as Record<string, unknown>;
    for (const field of new Set([...Object.keys(a), ...Object.keys(b)])) {
      if (fingerprint(a[field]) !== fingerprint(b[field])) writeField(saved, field, b[field]);
    }
  }
  if (
    fingerprint(submitted.map((item) => item.id)) !== fingerprint(current.map((item) => item.id))
  ) {
    const ids = new Set([...current.map((item) => item.id), ...result.keys()]);
    return [...ids].flatMap((id) => {
      const item = result.get(id);
      return item ? [item] : [];
    });
  }
  return [...result.values()];
}

function record(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function rebaseFields(submitted: object, current: object, confirmed: object): void {
  const before = submitted as Record<string, unknown>;
  const after = current as Record<string, unknown>;
  const saved = confirmed as Record<string, unknown>;
  for (const field of new Set([...Object.keys(before), ...Object.keys(after)])) {
    if (fingerprint(before[field]) === fingerprint(after[field])) continue;
    if (record(before[field]) && record(after[field]) && record(saved[field])) {
      rebaseFields(before[field], after[field], saved[field]);
    } else {
      writeField(saved, field, after[field]);
    }
  }
}

/** Accept server canonicalization while retaining both unrelated drafts and
 * edits made after Save was pressed. No editor/baseline arrays are shared. */
export function rebaseImportDraft(
  submitted: Snapshot,
  currentEditor: Snapshot,
  confirmed: Snapshot,
  atSaveEditor: Snapshot = submitted,
): Snapshot {
  const editor = copy(currentEditor);
  const submittedProfiles = new Map(submitted.profiles.map((profile) => [profile.id, profile]));
  for (const section of ["groups", "subscriptions"] as const) {
    const sent = new Map(submitted[section].map((route) => [route.id, route]));
    const atSave = new Map(atSaveEditor[section].map((route) => [route.id, route]));
    for (const route of editor[section]) {
      const target = sent.get(route.id);
      const original = atSave.get(route.id);
      if (
        target?.profile &&
        target.profile === original?.profile &&
        route.profile === original.profile &&
        route.interface === original.interface &&
        target.interface === submittedProfiles.get(target.profile)?.interfaces[0]
      ) {
        // projectImportDraft may have advanced this derived field when its
        // profile was edited. An unchanged editor value is not a new user edit.
        route.interface = target.interface;
      }
    }
  }
  const result = copy(confirmed);
  for (const section of collections) {
    // The sections share identity/field semantics; keep each result typed.
    if (section === "groups")
      result.groups = rebaseCollection(submitted.groups, editor.groups, confirmed.groups);
    else if (section === "subscriptions")
      result.subscriptions = rebaseCollection(
        submitted.subscriptions,
        editor.subscriptions,
        confirmed.subscriptions,
      );
    else
      result.profiles = rebaseCollection(submitted.profiles, editor.profiles, confirmed.profiles);
  }
  rebaseFields(submitted.settings, currentEditor.settings, result.settings);
  return result;
}
