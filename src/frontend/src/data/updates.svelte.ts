import {
  candidate,
  isActive,
  parseBuildInfo,
  parseStatus,
  ReleaseClient,
  type BuildInfo,
  type Release,
  type UpdateStatus,
} from "../modules/updates/releases";
import { fetcher } from "../utils/fetcher";

export const updates = $state({
  info: null as BuildInfo | null,
  release: null as Release | null,
  status: { stage: "idle" } as UpdateStatus,
  preview: false,
  checking: false,
  submitting: false,
  reconnecting: false,
  checkedAt: 0,
  error: "",
  dialogOpen: false,
  installedThisSession: false,
});

let owner: AbortController | null = null;
let timer: ReturnType<typeof setTimeout> | undefined;
let generation = 0;
let mounts = 0;
let client: ReleaseClient | null = null;
let reconnectAttempts = 0;
let awaitingTag: string | null = null;
let observedJob: string | null = null;

function storage(): Storage | null {
  try {
    return window.localStorage;
  } catch {
    return null;
  }
}

export async function checkUpdates(force = false) {
  if (
    !owner ||
    updates.checking ||
    updates.submitting ||
    updates.reconnecting ||
    isActive(updates.status)
  )
    return;
  const signal = owner.signal;
  const revision = ++generation;
  updates.checking = true;
  updates.error = "";
  try {
    const info = parseBuildInfo(
      await fetcher.get<unknown>("/system/update", { silent: true, signal, timeoutMs: 7000 }),
    );
    if (signal.aborted || revision !== generation) return;
    updates.info = info;
    const result = await client!.latest(updates.preview, force, signal);
    if (signal.aborted || revision !== generation) return;
    updates.release = result.release;
    updates.checkedAt = result.checkedAt;
  } catch (error) {
    if (!signal.aborted && revision === generation) {
      updates.error = error instanceof Error ? error.message : "Cannot check GitHub releases";
    }
  } finally {
    if (!signal.aborted && revision === generation) updates.checking = false;
  }
}

export function setUpdateChannel(preview: boolean) {
  if (updates.submitting || updates.checking || updates.reconnecting || isActive(updates.status))
    return;
  updates.preview = preview;
  const cached = client?.cached(preview);
  updates.release = cached?.release ?? null;
  updates.checkedAt = cached?.checkedAt ?? 0;
  try {
    storage()?.setItem("mt-c.update-preview", String(preview));
  } catch {
    /* optional preference */
  }
}

async function pollStatus() {
  if (!owner || owner.signal.aborted) return;
  const signal = owner.signal;
  try {
    const status = parseStatus(
      await fetcher.get<unknown>("/system/update/status", {
        silent: true,
        signal,
        timeoutMs: 7000,
      }),
    );
    if (signal.aborted) return;
    if (awaitingTag && status.tag !== awaitingTag) {
      updates.error = "Update start was not confirmed; no automatic retry was made";
      updates.reconnecting = false;
      awaitingTag = null;
      return;
    }
    if (awaitingTag) updates.error = "";
    awaitingTag = null;
    if (isActive(status)) observedJob = status.job_id ?? null;
    updates.status = status;
    updates.reconnecting = false;
    reconnectAttempts = 0;
    if (status.stage === "succeeded" && observedJob === status.job_id) {
      const info = parseBuildInfo(
        await fetcher.get<unknown>("/system/update", { silent: true, signal, timeoutMs: 7000 }),
      );
      if (signal.aborted) return;
      updates.info = info;
      if (
        info.installed_version !== status.target_version ||
        info.installed_revision !== status.target_revision
      ) {
        updates.error = "Installed version could not be confirmed";
      } else {
        // Keep the reload hint in memory; historical jobs must not restore it after reload.
        updates.installedThisSession = true;
      }
    }
  } catch {
    if (
      !signal.aborted &&
      (isActive(updates.status) || updates.submitting || updates.reconnecting)
    ) {
      updates.reconnecting = true;
      reconnectAttempts++;
    }
  } finally {
    if (
      !signal.aborted &&
      (isActive(updates.status) || updates.reconnecting || updates.submitting)
    ) {
      timer = setTimeout(() => void pollStatus(), Math.min(10000, 2000 + reconnectAttempts * 1000));
    }
  }
}

export async function installUpdate() {
  const selected = candidate(updates.release, updates.info);
  if (
    !owner ||
    !updates.release ||
    !selected.asset ||
    selected.reason ||
    updates.submitting ||
    isActive(updates.status)
  )
    return;
  const signal = owner.signal;
  const release = updates.release;
  updates.submitting = true;
  updates.installedThisSession = false;
  awaitingTag = release.tag_name;
  updates.error = "";
  try {
    const result = parseStatus(
      await fetcher<unknown>("/system/update/install", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          tag: release.tag_name,
          release_id: release.id,
          preview: updates.preview,
        }),
        silent: true,
        signal,
        timeoutMs: 10000,
      }),
    );
    if (signal.aborted) return;
    updates.status = result;
    observedJob = result.job_id ?? null;
  } catch {
    if (!signal.aborted) {
      // The response can be lost after acceptance. Never blindly retry the POST.
      updates.error = "Could not confirm update start; checking router status";
      updates.reconnecting = true;
    }
  } finally {
    if (!signal.aborted) {
      updates.submitting = false;
      if (timer) clearTimeout(timer);
      void pollStatus();
    }
  }
}

/** One owner in the persistent header. Closing the dialog never cancels the
 * router's job; unmounting only cleans up browser requests and timers. */
export function mountUpdates() {
  if (++mounts === 1) {
    owner = new AbortController();
    client = new ReleaseClient(storage());
    updates.checking = false;
    updates.submitting = false;
    updates.reconnecting = false;
    updates.installedThisSession = false;
    awaitingTag = null;
    observedJob = null;
    try {
      updates.preview = storage()?.getItem("mt-c.update-preview") === "true";
    } catch {
      /* default stable */
    }
    const cached = client.cached(updates.preview);
    updates.release = cached?.release ?? null;
    updates.checkedAt = cached?.checkedAt ?? 0;
    void pollStatus().then(() => checkUpdates());
  }
  return () => {
    if (--mounts === 0) {
      owner?.abort();
      owner = null;
      generation++;
      if (timer) clearTimeout(timer);
      updates.dialogOpen = false;
    }
  };
}
