import { authState, requireAuthentication } from "../../../data/auth.svelte";

import { fetcher } from "../../../utils/fetcher";
import { HttpError } from "../../../utils/http-error";
import {
  buildGeneralSettings,
  emptyDraft,
  readSettingsAcknowledgement,
  readSettingsError,
  readSettingsSnapshot,
  sameSettings,
  settingsToDraft,
  webTarget,
  type GeneralSettings,
  type SettingsError,
  type SettingsSnapshot,
  type WebTarget,
} from "./config";

type PendingApplication = {
  settings: GeneralSettings;
  draft: string;
  previousRevision: string;
  previousOperationId?: string;
  operationId?: string;
  targetRevision?: string;
};
type Outcome =
  "idle" | "applying" | "succeeded" | "failed" | "rollback_failed" | "unknown" | "awaiting_auth";

const POLL_TIMEOUT_MS = 20000;
const POLL_INTERVAL_MS = 750;
const unconfirmedMessage =
  "Could not confirm whether the settings were applied. Your draft is preserved.";

function pause(delay: number, signal: AbortSignal): Promise<void> {
  return new Promise((resolve) => {
    const done = () => {
      clearTimeout(timer);
      signal.removeEventListener("abort", done);
      resolve();
    };
    const timer = setTimeout(done, delay);
    signal.addEventListener("abort", done, { once: true });
    if (signal.aborted) done();
  });
}

/** The draft and pending request stay in memory, including through reauthentication. */
export class GeneralSettingsState {
  draft = $state(emptyDraft());
  snapshot = $state.raw<SettingsSnapshot>();
  loading = $state(false);
  saving = $state(false);
  checking = $state(false);
  pending = $state.raw<PendingApplication>();
  nextWeb = $state.raw<WebTarget>();
  outcome = $state<Outcome>("idle");
  error = $state.raw<SettingsError>();
  built = $derived(buildGeneralSettings(this.draft));
  dirty = $derived(
    Boolean(
      this.snapshot &&
      (this.built.settings
        ? !sameSettings(this.built.settings, this.snapshot.settings)
        : JSON.stringify(this.draft) !== JSON.stringify(settingsToDraft(this.snapshot.settings))),
    ),
  );
  busy = $derived(this.loading || this.saving || this.checking);
  canSave = $derived(
    Boolean(
      this.snapshot &&
      this.dirty &&
      this.built.settings &&
      !this.busy &&
      !this.pending &&
      this.snapshot.operation?.state !== "applying",
    ),
  );
  private generation = 0;
  private request?: AbortController;

  private begin() {
    this.request?.abort();
    const controller = new AbortController();
    this.request = controller;
    return { controller, generation: ++this.generation };
  }

  private unauthorized(error: unknown): boolean {
    if (!(error instanceof HttpError) || ![401, 403].includes(error.status)) return false;
    this.outcome = "awaiting_auth";
    this.error = { message: "Sign in to check the settings application." };
    requireAuthentication();
    return true;
  }

  async load(): Promise<void> {
    if (this.busy || this.dirty || this.pending) return;
    const { controller, generation } = this.begin();
    this.loading = true;
    this.error = undefined;
    try {
      const snapshot = readSettingsSnapshot(
        await fetcher.get<unknown>("/system/settings", {
          silent: true,
          authFailure: "preserve",
          timeoutMs: 6000,
          signal: controller.signal,
          redirect: "error",
        }),
      );
      if (generation !== this.generation) return;
      this.snapshot = snapshot;
      this.draft = settingsToDraft(snapshot.settings);
      this.pending = undefined;
      this.nextWeb = undefined;
      this.outcome = snapshot.operation?.state === "applying" ? "applying" : "idle";
    } catch (error) {
      if (generation === this.generation && !controller.signal.aborted && !this.unauthorized(error))
        this.error = readSettingsError(error);
    } finally {
      if (generation === this.generation) this.loading = false;
    }
  }

  async save(): Promise<void> {
    const settings = this.built.settings;
    if (!this.canSave || !settings || !this.snapshot)
      throw new Error("Settings cannot be saved yet.");
    const { controller, generation } = this.begin();
    const pending: PendingApplication = {
      settings,
      draft: JSON.stringify(this.draft),
      previousRevision: this.snapshot.revision,
      previousOperationId: this.snapshot.operation?.id,
    };
    const previousWeb = this.snapshot.settings.httpWeb;
    const nextWeb = settings.httpWeb;
    this.nextWeb =
      previousWeb.enabled !== nextWeb.enabled ||
      previousWeb.host.address !== nextWeb.host.address ||
      previousWeb.host.port !== nextWeb.host.port
        ? webTarget(settings)
        : undefined;
    this.pending = pending;
    this.saving = true;
    this.error = undefined;
    this.outcome = "applying";
    try {
      try {
        const acknowledgement = readSettingsAcknowledgement(
          await fetcher<unknown>("/system/settings", {
            method: "PUT",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ settings, revision: pending.previousRevision }),
            silent: true,
            authFailure: "preserve",
            timeoutMs: 6000,
            signal: controller.signal,
            redirect: "error",
          }),
        );
        if (generation !== this.generation) return;
        if (JSON.stringify(acknowledgement.next_web) !== JSON.stringify(webTarget(settings)))
          throw new Error("The server did not confirm the settings request.");
        pending.operationId = acknowledgement.operation.id;
        pending.targetRevision = acknowledgement.target_revision;
        this.pending = { ...pending };
        if (this.nextWeb) this.nextWeb = acknowledgement.next_web;
      } catch (error) {
        if (generation !== this.generation || controller.signal.aborted) throw error;
        if (this.unauthorized(error)) {
          this.pending = undefined;
          throw error;
        }
        const failure = readSettingsError(error);
        if (
          error instanceof HttpError &&
          ([400, 409].includes(error.status) ||
            (error.status === 503 && failure.code === "SETTINGS_UNAVAILABLE"))
        ) {
          this.pending = undefined;
          this.nextWeb = undefined;
          this.error = failure;
          this.outcome = "failed";
          throw error;
        }
        // A missing/malformed ACK does not prove rejection. Reconcile using GET;
        // never repeat the PUT automatically after a transport failure.
        this.error = failure;
      }
      if (!(await this.poll(pending, generation, controller.signal)))
        throw new Error(this.error?.message ?? unconfirmedMessage);
    } finally {
      if (generation === this.generation) this.saving = false;
    }
  }

  async checkApplication(): Promise<void> {
    if (this.busy) return;
    if (!this.pending) {
      await this.load();
      return;
    }
    const { controller, generation } = this.begin();
    this.checking = true;
    this.error = undefined;
    this.outcome = "applying";
    try {
      await this.poll(this.pending, generation, controller.signal);
    } finally {
      if (generation === this.generation) this.checking = false;
    }
  }

  private async poll(
    pending: PendingApplication,
    generation: number,
    signal: AbortSignal,
  ): Promise<boolean> {
    const deadline = Date.now() + POLL_TIMEOUT_MS;
    for (let attempt = 0; attempt < 24 && Date.now() < deadline && !signal.aborted; attempt++) {
      try {
        const snapshot = readSettingsSnapshot(
          await fetcher.get<unknown>("/system/settings", {
            silent: true,
            authFailure: "preserve",
            timeoutMs: Math.min(3000, Math.max(1, deadline - Date.now())),
            signal,
            redirect: "error",
          }),
        );
        if (generation !== this.generation || signal.aborted) return false;
        const operation = snapshot.operation;
        // For a lost ACK, adopt only a newly observed completed operation whose
        // target revision AND full editable projection match our submission.
        if (
          !pending.operationId &&
          operation?.state === "succeeded" &&
          operation.id !== pending.previousOperationId &&
          operation.target_revision &&
          operation.target_revision === snapshot.revision &&
          sameSettings(snapshot.settings, pending.settings)
        ) {
          pending.operationId = operation.id;
          pending.targetRevision = operation.target_revision;
          this.pending = { ...pending };
        }
        if (pending.operationId && operation?.id === pending.operationId) {
          if (operation.state === "failed" || operation.state === "rollback_failed") {
            this.snapshot = snapshot;
            this.pending = undefined;
            this.nextWeb = undefined;
            this.outcome = operation.state;
            this.error = {
              message:
                operation.error ?? "The settings could not be applied. Your draft is preserved.",
            };
            return false;
          }
          if (operation.state === "succeeded") {
            if (
              snapshot.revision !== pending.targetRevision ||
              (operation.target_revision && operation.target_revision !== pending.targetRevision) ||
              !sameSettings(snapshot.settings, pending.settings)
            ) {
              this.error = {
                message: "The applied settings do not match this request. Your draft is preserved.",
              };
              this.outcome = "unknown";
              return false;
            }
            this.snapshot = snapshot;
            if (JSON.stringify(this.draft) === pending.draft)
              this.draft = settingsToDraft(snapshot.settings);
            this.pending = undefined;
            this.outcome = "succeeded";
            this.error = undefined;
            authState.enabled = snapshot.settings.httpWeb.auth.enabled;
            return true;
          }
        } else if (pending.operationId && operation?.id !== pending.operationId) {
          this.outcome = "unknown";
          this.error = {
            message: "Another settings change was observed. Your draft is preserved.",
          };
          return false;
        }
      } catch (error) {
        if (generation !== this.generation || signal.aborted) return false;
        if (this.unauthorized(error)) return false;
        this.error = readSettingsError(error);
      }
      await pause(Math.min(POLL_INTERVAL_MS, Math.max(0, deadline - Date.now())), signal);
    }
    if (generation === this.generation && !signal.aborted) {
      this.outcome = "unknown";
      this.error = { message: unconfirmedMessage };
    }
    return false;
  }

  destroy() {
    this.generation++;
    this.request?.abort();
  }
}
