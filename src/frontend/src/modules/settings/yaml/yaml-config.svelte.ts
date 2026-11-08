import { fetcher } from "../../../utils/fetcher";
import { readYamlConfig, readYamlError, type YamlConfig, type YamlConfigError } from "./config";

/** Raw configuration is kept only in memory. It may contain credentials. */
export class YamlConfigState {
  draft = $state("");
  snapshot = $state.raw<YamlConfig>();
  loading = $state(false);
  saving = $state(false);
  error = $state.raw<YamlConfigError>();
  dirty = $derived(this.snapshot !== undefined && this.draft !== this.snapshot.yaml);
  private generation = 0;

  async load(discardDraft = false): Promise<void> {
    if (this.loading || this.saving || (this.dirty && !discardDraft)) return;
    const generation = ++this.generation;
    this.loading = true;
    this.error = undefined;
    try {
      const snapshot = readYamlConfig(
        await fetcher.get<unknown>("/system/config/yaml", { silent: true }),
      );
      if (generation !== this.generation) return;
      this.snapshot = snapshot;
      this.draft = snapshot.yaml;
    } catch (error) {
      if (generation === this.generation) this.error = readYamlError(error);
    } finally {
      if (generation === this.generation) this.loading = false;
    }
  }

  async save(allowOutOfSync = false): Promise<void> {
    if (!this.snapshot || !this.dirty || this.loading || this.saving) return;
    const generation = ++this.generation;
    const yaml = this.draft;
    const revision = this.snapshot.revision;
    this.saving = true;
    this.error = undefined;
    try {
      const snapshot = readYamlConfig(
        await fetcher<unknown>("/system/config/yaml", {
          method: "PUT",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            yaml,
            revision,
            ...(allowOutOfSync ? { allow_out_of_sync: true } : {}),
          }),
          silent: true,
        }),
      );
      if (generation !== this.generation) return;
      if (snapshot.yaml !== yaml || snapshot.out_of_sync)
        throw new Error("The server did not confirm the saved YAML. Your draft is preserved.");
      this.snapshot = snapshot;
      // Preserve edits made programmatically while a request was in flight.
      if (this.draft === yaml) this.draft = snapshot.yaml;
    } catch (error) {
      if (generation === this.generation) this.error = readYamlError(error);
      throw error;
    } finally {
      if (generation === this.generation) this.saving = false;
    }
  }
}
