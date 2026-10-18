import { parse } from "valibot";

import { cleanProfiles, profileError } from "../modules/settings/profiles-data";
import { ProfileSchema, type Profile } from "../types";
import { fetcher } from "../utils/fetcher";
import { HttpError } from "../utils/http-error";

// Drafts survive tab changes. Refreshing server definitions/usage must not
// discard edits or the pending disk-write acknowledgement.
export const profiles = $state({
  list: [] as Profile[],
  draft: [] as Profile[],
  baseline: "[]",
  loaded: false,
  loading: false,
  failed: false,
  saving: false,
  persistencePending: false,
});
export function profilesDirty() {
  return (
    JSON.stringify(cleanProfiles(profiles.draft)) !== profiles.baseline ||
    profiles.persistencePending
  );
}
function baselineProfiles(): Profile[] {
  return JSON.parse(profiles.baseline) as Profile[];
}
function changedProfileIds(draft: readonly Profile[], baseline: readonly Profile[]): Set<string> {
  const previous = new Map(
    cleanProfiles(baseline).map((profile) => [profile.id, JSON.stringify(profile)]),
  );
  const dirty = new Set<string>();
  for (const profile of cleanProfiles(draft)) {
    if (previous.get(profile.id) !== JSON.stringify(profile)) dirty.add(profile.id);
    previous.delete(profile.id);
  }
  for (const id of previous.keys()) dirty.add(id);
  return dirty;
}
/** Collection order alone does not make an individual profile an import conflict. */
export function profileDraftIds(): Set<string> {
  return changedProfileIds(profiles.draft, baselineProfiles());
}
function copyProfile(profile: Profile): Profile {
  return {
    ...profile,
    interfaces: [...profile.interfaces],
    ...(profile.usage ? { usage: { ...profile.usage } } : {}),
  };
}
function rebasedDraft(list: readonly Profile[]): Profile[] {
  const baseline = baselineProfiles();
  const dirty = changedProfileIds(profiles.draft, baseline);
  const local = new Map(profiles.draft.map((profile) => [profile.id, profile]));
  const next = new Map<string, Profile>();
  for (const profile of list) {
    if (!dirty.has(profile.id)) next.set(profile.id, profile);
    else {
      // A missing local entry is a deliberate deletion from the old baseline.
      const draft = local.get(profile.id);
      if (draft) next.set(profile.id, draft);
    }
  }
  for (const profile of profiles.draft) {
    if (dirty.has(profile.id)) next.set(profile.id, profile);
  }
  const localOrderChanged =
    JSON.stringify(baseline.map((profile) => profile.id)) !==
    JSON.stringify(profiles.draft.map((profile) => profile.id));
  // Keep local moves/insertions, then append newly imported definitions. Without
  // local structural edits, retain the order returned by the server.
  const order = localOrderChanged ? [...profiles.draft, ...list] : [...list, ...profiles.draft];
  const result: Profile[] = [];
  for (const { id } of order) {
    const profile = next.get(id);
    if (!profile) continue;
    result.push(copyProfile(profile));
    next.delete(id);
  }
  return result;
}
function decodeProfiles(data: { profiles: unknown[] }): Profile[] {
  if (!Array.isArray(data?.profiles)) throw new Error("Invalid profiles response");
  const result = data.profiles.map((profile) => parse(ProfileSchema, profile));
  const error = profileError(result);
  if (error) throw new Error(error);
  return result;
}
function accept(list: Profile[], resetDraft: boolean) {
  profiles.list = list;
  profiles.loaded = true;
  profiles.failed = false;
  if (resetDraft) {
    profiles.draft = list.map(copyProfile);
    profiles.baseline = JSON.stringify(cleanProfiles(list));
  }
}
let pendingFetch: Promise<void> | undefined;
let generation = 0;
export function fetchProfiles(
  options: { fresh?: boolean; rebaseDrafts?: boolean } = {},
): Promise<void> {
  if (pendingFetch) {
    // A request started before a consumer was saved may contain obsolete
    // usage. An explicit refresh must follow it, not merely share its result.
    if (!options.fresh && !options.rebaseDrafts) return pendingFetch;
    // A pre-import response must not replace the baseline used to retain drafts.
    if (options.rebaseDrafts) generation++;
    return pendingFetch.then(() => fetchProfiles({ rebaseDrafts: options.rebaseDrafts }));
  }
  if (profiles.saving) return Promise.resolve();
  const requestGeneration = generation;
  profiles.loading = true;
  pendingFetch = (async () => {
    try {
      const response = await fetcher.get<{ profiles: unknown[] }>("/profiles");
      if (requestGeneration === generation) {
        const list = decodeProfiles(response);
        if (options.rebaseDrafts) {
          const draft = rebasedDraft(list);
          accept(list, false);
          profiles.baseline = JSON.stringify(cleanProfiles(list));
          profiles.draft = draft;
        } else accept(list, !profilesDirty());
      }
    } catch (error) {
      if (requestGeneration === generation) profiles.failed = true;
      console.error("Failed to fetch profiles", error);
    } finally {
      profiles.loading = false;
      pendingFetch = undefined;
    }
  })();
  return pendingFetch;
}
export function resetProfileDraft() {
  // Discard edits, not a failed disk write: the latter still needs a retry.
  accept(profiles.list, true);
}
export async function saveProfiles(values: readonly Profile[] = profiles.draft): Promise<void> {
  if (profiles.saving) throw new Error("Profile save already in progress");
  const clean = cleanProfiles(values);
  const error = profileError(clean);
  if (error) throw new Error(error);
  profiles.saving = true;
  generation++;
  try {
    const response = await fetcher.put<{ profiles: unknown[] }>("/profiles", { profiles: clean });
    accept(decodeProfiles(response), true);
    profiles.persistencePending = false;
  } catch (error) {
    if (error instanceof HttpError && error.status === 500) {
      const body = error.data as
        | { code?: string; applied?: boolean; profiles?: unknown[] }
        | undefined;
      if (
        body?.code === "PERSISTENCE_FAILED" &&
        body.applied === true &&
        Array.isArray(body.profiles)
      ) {
        try {
          accept(decodeProfiles({ profiles: body.profiles }), true);
          profiles.persistencePending = true;
        } catch {
          /* A malformed acknowledgement is not a new baseline. */
        }
      }
    }
    throw error;
  } finally {
    profiles.saving = false;
  }
}
