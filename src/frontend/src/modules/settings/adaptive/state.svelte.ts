import { fetcher } from "../../../utils/fetcher";
import { HttpError } from "../../../utils/http-error";
import {
  adaptiveDefaults,
  adaptiveDraft,
  buildAdaptiveSettings,
  readAdaptiveStatus,
  type AdaptiveStatus,
} from "./data";

const unconfirmed = "Could not confirm whether the settings were applied. Your draft is preserved.";

/** One mounted settings editor owns its draft; status reads never discard edits. */
export class AdaptiveSettingsState {
  draft = $state(adaptiveDraft(adaptiveDefaults));
  snapshot = $state.raw<AdaptiveStatus>();
  baseline = $state("");
  loading = $state(false);
  saving = $state(false);
  clearing = $state(false);
  failed = $state(false);
  saveError = $state<string>();
  persistencePending = $state(false);
  private generation = 0;
  private request?: AbortController;

  get built() {
    return buildAdaptiveSettings(this.draft);
  }
  get dirty() {
    return Boolean(
      this.snapshot && (this.persistencePending || JSON.stringify(this.draft) !== this.baseline),
    );
  }
  get busy() {
    return this.loading || this.saving || this.clearing;
  }
  get canSave() {
    return Boolean(
      this.snapshot && this.dirty && this.built.settings && !this.busy && !this.failed,
    );
  }
  get canRetryActivation() {
    return Boolean(
      this.snapshot?.settings.enabled &&
      !this.snapshot.active &&
      !this.dirty &&
      !this.busy &&
      !this.failed,
    );
  }

  private accept(status: AdaptiveStatus, resetDraft: boolean) {
    this.snapshot = status;
    this.failed = false;
    if (resetDraft) {
      this.draft = adaptiveDraft(status.settings);
      this.baseline = JSON.stringify(this.draft);
    }
  }

  async load(refresh = false): Promise<void> {
    if (this.busy || (!refresh && (this.snapshot || this.failed))) return;
    const version = ++this.generation;
    this.request = new AbortController();
    this.loading = true;
    try {
      const value = readAdaptiveStatus(
        await fetcher.get<unknown>("/adaptive", {
          silent: true,
          timeoutMs: 6000,
          signal: this.request.signal,
        }),
      );
      if (version === this.generation) this.accept(value, !this.dirty);
    } catch {
      if (version === this.generation) this.failed = true;
    } finally {
      if (version === this.generation) this.loading = false;
    }
  }

  async save(): Promise<void> {
    if (!this.canSave) return;
    await this.apply();
  }

  async retryActivation(): Promise<void> {
    if (!this.canRetryActivation) return;
    await this.apply();
  }

  private async apply(): Promise<void> {
    const settings = this.built.settings;
    if (!settings) return;
    const submitted = JSON.stringify(this.draft);
    ++this.generation;
    this.saving = true;
    this.saveError = undefined;
    try {
      const status = readAdaptiveStatus(await fetcher.put<unknown>("/adaptive", { settings }));
      if (JSON.stringify(status.settings) !== JSON.stringify(settings))
        throw new Error("The applied settings do not match this request. Your draft is preserved.");
      this.accept(status, JSON.stringify(this.draft) === submitted);
      if (JSON.stringify(this.draft) !== submitted)
        this.baseline = JSON.stringify(adaptiveDraft(status.settings));
      this.persistencePending = false;
    } catch (error) {
      let applied = false;
      const value = error instanceof HttpError ? error.data : undefined;
      let status: AdaptiveStatus | undefined;
      if (value) {
        try {
          status = readAdaptiveStatus(value);
        } catch {
          /* An incomplete error cannot confirm the current runtime state. */
        }
      }
      if (error instanceof HttpError && error.status === 500 && status) {
        if (
          value &&
          typeof value === "object" &&
          "code" in value &&
          value.code === "PERSISTENCE_FAILED" &&
          "applied" in value &&
          value.applied === true &&
          JSON.stringify(status.settings) === JSON.stringify(settings)
        ) {
          this.accept(status, JSON.stringify(this.draft) === submitted);
          this.baseline = JSON.stringify(adaptiveDraft(status.settings));
          this.persistencePending = true;
          applied = true;
        }
      }
      if (!applied) {
        // Failed activation may also fail rollback. Use the confirmed runtime
        // without treating a rejected request as a saved draft.
        if (status) this.accept(status, false);
        else this.failed = true;
        this.saveError =
          error instanceof HttpError
            ? "The settings could not be applied. Your draft is preserved."
            : unconfirmed;
      }
      throw error;
    } finally {
      this.saving = false;
    }
  }

  async clear(): Promise<void> {
    if (this.busy || !this.snapshot) return;
    ++this.generation;
    this.clearing = true;
    try {
      this.accept(readAdaptiveStatus(await fetcher.delete<unknown>("/adaptive/cache")), false);
    } catch {
      this.failed = true;
    } finally {
      this.clearing = false;
    }
  }

  destroy() {
    ++this.generation;
    this.request?.abort();
  }
}
