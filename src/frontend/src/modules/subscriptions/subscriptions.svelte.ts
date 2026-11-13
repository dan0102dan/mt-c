import { t } from "../../data/locale.svelte";
import { ChangeTracker } from "../../utils/change-tracker.svelte";

import { type Subscription, type SubscriptionRule } from "../../types";
import { appliedSubscriptions } from "../../utils/applied-save";
import { randomId } from "../../utils/defaults";
import { overlay, toast } from "../../utils/events";
import { fetcher } from "../../utils/fetcher";
import {
  DEFAULT_SUBSCRIPTION_PRIORITY,
  isValidPriority,
  withPriorityDefault,
} from "../../utils/priority";
import { VALIDATOP_MAP } from "../../utils/rule-validators";
import { subscriptionRulePattern } from "../../utils/subscription-pattern";
import { buildSubscriptionUpdate, snapshotRules } from "./subscription-payload";

export const SUBSCRIPTIONS_STORE_CONTEXT = Symbol("subscriptions-store");

const SEARCH_DEBOUNCE_MS = 150 as const;

const clamp = (value: number, min: number, max: number) => Math.max(min, Math.min(max, value));

function editorSubscription(subscription: Subscription): Subscription {
  const normalized = withPriorityDefault(subscription, DEFAULT_SUBSCRIPTION_PRIORITY);
  // Keep empty exceptions explicit: absence in older snapshots can mean retain
  // saved overrides, whereas [] also represents the user's deliberate reset.
  normalized.overrides ??= [];
  return normalized;
}

export function normalizeSubscriptionUrl(value: string) {
  return value.trim();
}

export function validateSubscriptionUrl(value: string) {
  const normalized = normalizeSubscriptionUrl(value);
  if (!normalized) return false;

  try {
    new URL(normalized);
    return true;
  } catch {
    return false;
  }
}

export type VisibleSubscription = {
  group_index: number;
  ruleIndices: number[] | null;
};

export type SubscriptionDragData = {
  group_id: string;
  group_index: number;
  name: string;
  count: number;
};

export type SubscriptionDropSlotData = {
  group_index: number;
  insert: "before" | "after";
};

type AddSubscriptionPayload = {
  url: string;
  name: string;
  interface: string;
  profile?: string;
  interval: number;
};

type SubscriptionsStoreOptions = {
  onRenderComplete?: () => void;
  isActive?: () => boolean;
};

export class SubscriptionsStore {
  onRenderComplete?: () => void;
  private isActive: () => boolean;

  tracker = $state(new ChangeTracker<Subscription[]>([]));
  #loadGeneration = 0;
  importPending = $state(false);
  delegateSave?: () => Promise<void>;
  saving = $state(false);
  persistencePending = $state(false);
  hasUnsavedChanges = $derived(
    this.tracker.isDirty || this.persistencePending || this.importPending,
  );
  data = $derived.by(() => this.tracker.data);
  dataRevision = $state(0);
  valid_rules = $state(true);
  #validatedRevision = -1;
  valid_priorities = $derived(
    this.data.every((subscription) => isValidPriority(subscription.priority)),
  );
  subscriptionUrlErrors = $derived.by(() => {
    const errors = new Map<string, string>();
    const idsByUrl = new Map<string, string[]>();

    for (const subscription of this.data) {
      const normalizedUrl = normalizeSubscriptionUrl(subscription.url ?? "");

      if (!validateSubscriptionUrl(normalizedUrl)) {
        errors.set(subscription.id, "Invalid URL");
        continue;
      }

      const ids = idsByUrl.get(normalizedUrl) ?? [];
      ids.push(subscription.id);
      idsByUrl.set(normalizedUrl, ids);
    }

    for (const ids of idsByUrl.values()) {
      if (ids.length <= 1) continue;
      for (const id of ids) {
        errors.set(id, "Subscription already exists");
      }
    }

    return errors;
  });
  valid_subscription_urls = $derived(this.subscriptionUrlErrors.size === 0);
  canSave = $derived(
    this.hasUnsavedChanges &&
      this.data.every((subscription) => Boolean(subscription.name.trim())) &&
      this.valid_rules &&
      this.valid_priorities &&
      this.valid_subscription_urls &&
      !this.saving,
  );

  open_state = $state<Record<string, boolean>>({});

  searchValue = $state("");
  visibleSubscriptions = $state<VisibleSubscription[]>([]);
  searchPending = $state(false);

  normalizedSearch = $derived(this.searchValue.trim().toLowerCase());
  searchActive = $derived(Boolean(this.normalizedSearch));

  visibilityMap = $derived(
    new Map(this.visibleSubscriptions.map((entry) => [entry.group_index, entry.ruleIndices])),
  );

  firstVisibleSubscriptionIndex = $derived(
    this.searchActive
      ? this.visibleSubscriptions.length
        ? this.visibleSubscriptions[0].group_index
        : -1
      : 0,
  );

  noVisibleSubscriptions = $derived(
    this.searchActive && !this.searchPending && this.visibleSubscriptions.length === 0,
  );

  fetchError = $state(false);
  dataLoaded = $state(false);

  // Readiness belongs to accepted data, not to the number of DOM components.
  isAllRendered = $derived(this.dataLoaded);

  isEmptyData = $derived(
    this.dataLoaded &&
      !this.fetchError &&
      !this.searchActive &&
      !this.searchPending &&
      this.data.length === 0,
  );

  #savedRules = new Map<string, SubscriptionRule[]>();
  #forcedSubscriptionIds = new Set<string>();
  #forcedRuleIdsBySubscription = new Map<string, Set<string>>();
  #forcedSearchKey = "";
  #debounceTimer: number | null = null;
  #dispose: (() => void) | null = null;

  constructor(options: SubscriptionsStoreOptions = {}) {
    this.onRenderComplete = options.onRenderComplete;
    this.isActive = options.isActive ?? (() => true);
    this.#setupEffects();
  }

  #setupEffects() {
    this.#dispose = $effect.root(() => {
      $effect(() => {
        if (typeof window === "undefined" || !this.hasUnsavedChanges) return;

        const handleBeforeUnload = (event: BeforeUnloadEvent) => {
          event.preventDefault();
        };

        window.addEventListener("beforeunload", handleBeforeUnload);
        return () => window.removeEventListener("beforeunload", handleBeforeUnload);
      });

      $effect(() => {
        this.dataRevision;
        if (typeof window === "undefined") return;
        setTimeout(() => {
          if (this.#validatedRevision !== this.dataRevision) this.checkRulesValidityState();
        }, 10);
      });

      $effect(() => {
        const query = this.normalizedSearch;
        this.dataRevision;
        this.data.length;

        if (this.#debounceTimer) {
          clearTimeout(this.#debounceTimer);
          this.#debounceTimer = null;
        }

        if (!query) {
          this.visibleSubscriptions = this.data.map((_, index) => ({
            group_index: index,
            ruleIndices: null,
          }));
          this.searchPending = false;
          return;
        }

        this.searchPending = true;

        if (typeof window === "undefined") {
          this.performSearch();
          return;
        }

        this.#debounceTimer = window.setTimeout(() => this.performSearch(), SEARCH_DEBOUNCE_MS);
      });

      $effect(() => {
        if (this.isAllRendered) {
          this.onRenderComplete?.();
        }
      });

      return () => {
        if (this.#debounceTimer) {
          clearTimeout(this.#debounceTimer);
          this.#debounceTimer = null;
        }
      };
    });
  }

  destroy() {
    this.#loadGeneration++;
    if (typeof window !== "undefined") {
      window.removeEventListener("keydown", this.handleSaveShortcut);
    }

    if (this.#dispose) {
      this.#dispose();
      this.#dispose = null;
    }
  }

  mount = async (options: { preserveDrafts?: boolean } = {}) => {
    const generation = ++this.#loadGeneration;
    if (typeof window !== "undefined") {
      window.addEventListener("keydown", this.handleSaveShortcut);
    }
    this.fetchError = false;
    try {
      const fetched = (
        (await fetcher.get<{ subscriptions: Subscription[] }>("/subscriptions"))?.subscriptions ??
        []
      ).map(editorSubscription);
      if (generation !== this.#loadGeneration) return;
      if (options.preserveDrafts) this.tracker.rebase(fetched);
      else this.tracker = new ChangeTracker(fetched);
      this.#savedRules = new Map(fetched.map((sub) => [sub.id, snapshotRules(sub.rules)]));
      this.markDataRevision();
      if (typeof window !== "undefined") {
        setTimeout(() => {
          if (this.#validatedRevision !== this.dataRevision) this.checkRulesValidityState();
        }, 10);
      }
    } catch (error) {
      if (generation === this.#loadGeneration) {
        this.fetchError = true;
        console.error("Failed to load subscriptions:", error);
      }
    } finally {
      if (generation === this.#loadGeneration) this.dataLoaded = true;
    }
  };

  /** Download caches in the draft are editor data, not an accepted save baseline. */
  stageImport(baseline: readonly Subscription[], draft: readonly Subscription[]) {
    const saved = structuredClone([...baseline]).map(editorSubscription);
    const next = structuredClone([...draft]).map(editorSubscription);
    this.#loadGeneration++;
    this.#savedRules = new Map(saved.map((sub) => [sub.id, snapshotRules(sub.rules)]));
    this.tracker.reset(saved);
    this.tracker.replaceDraft(next);
    this.fetchError = false;
    this.dataLoaded = true;
    this.markDataRevision();
  }

  async prepareImport(baseline: readonly Subscription[], draft: readonly Subscription[]) {
    const generation = this.#loadGeneration;
    const revision = this.dataRevision;
    const saved = baseline.map(editorSubscription);
    const next = draft.map(editorSubscription);
    const tracker = await ChangeTracker.fromDraftAsync(saved, next);
    const savedRules = new Map<string, SubscriptionRule[]>();
    let deadline = performance.now() + 4;
    const pause = async () => {
      await new Promise<void>((resolve) => setTimeout(resolve, 0));
      deadline = performance.now() + 4;
    };
    for (const sub of saved) {
      const rules: SubscriptionRule[] = [];
      for (const rule of sub.rules) {
        rules.push({ ...rule });
        if (rules.length % 256 === 0 && performance.now() >= deadline) await pause();
      }
      savedRules.set(sub.id, rules);
    }
    let validRules = true;
    for (const sub of next) {
      for (let index = 0; index < sub.rules.length; index++) {
        const rule = sub.rules[index];
        const pattern = subscriptionRulePattern(rule.rule, rule.type);
        const typedWildcard =
          rule.type === "wildcard" &&
          (rule.rule.startsWith("DOMAIN-KEYWORD,") || rule.rule.startsWith("DOMAIN-WILDCARD,"));
        if (
          !pattern.length ||
          (!typedWildcard && VALIDATOP_MAP[rule.type] && !VALIDATOP_MAP[rule.type](pattern))
        )
          validRules = false;
        if (index % 256 === 0 && performance.now() >= deadline) await pause();
      }
    }
    const validate = () => {
      if (generation !== this.#loadGeneration || revision !== this.dataRevision)
        throw new Error("Configuration changed; refresh the import preview");
    };
    return {
      validate,
      commit: () => {
        validate();
        this.#loadGeneration++;
        this.#savedRules = savedRules;
        this.tracker = tracker;
        this.fetchError = false;
        this.dataLoaded = true;
        this.valid_rules = validRules;
        this.markDataRevision();
        this.#validatedRevision = this.dataRevision;
      },
    };
  }

  acceptImport(
    baseline: readonly Subscription[],
    retainedDraft: readonly Subscription[] = baseline,
    { persisted = true }: { persisted?: boolean } = {},
  ) {
    this.stageImport(baseline, retainedDraft);
    if (persisted) this.persistencePending = false;
  }

  handleSaveShortcut = (event: KeyboardEvent) => {
    if (
      !this.isActive() ||
      event.defaultPrevented ||
      event.isComposing ||
      event.altKey ||
      event.shiftKey
    )
      return;
    if (document.querySelector('[data-dialog-content][data-state="open"]')) return;
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "s") {
      if (this.importPending || this.canSave) {
        event.preventDefault();
        this.saveChanges();
      }
    }
  };

  #resetForcedVisibility(searchKey: string) {
    if (!this.#forcedSearchKey) return;
    if (this.#forcedSearchKey !== searchKey) {
      this.#forcedSubscriptionIds.clear();
      this.#forcedRuleIdsBySubscription.clear();
      this.#forcedSearchKey = "";
    }
  }

  forceVisibleGroup(groupId: string) {
    if (!this.normalizedSearch) return;
    this.#forcedSubscriptionIds.add(groupId);
    this.#forcedSearchKey = this.normalizedSearch;
  }

  forceVisibleRule(groupId: string, ruleId: string) {
    if (!this.normalizedSearch) return;
    let forced = this.#forcedRuleIdsBySubscription.get(groupId);
    if (!forced) {
      forced = new Set<string>();
      this.#forcedRuleIdsBySubscription.set(groupId, forced);
    }
    forced.add(ruleId);
    this.#forcedSearchKey = this.normalizedSearch;
  }

  removeForcedGroup(groupId: string) {
    this.#forcedSubscriptionIds.delete(groupId);
    this.#forcedRuleIdsBySubscription.delete(groupId);
  }

  removeForcedRule(groupId: string, ruleId: string) {
    const forced = this.#forcedRuleIdsBySubscription.get(groupId);
    if (!forced) return;
    forced.delete(ruleId);
    if (!forced.size) this.#forcedRuleIdsBySubscription.delete(groupId);
  }

  moveForcedRule(sourceGroupId: string, targetGroupId: string, ruleId: string) {
    if (sourceGroupId === targetGroupId) return;
    const forced = this.#forcedRuleIdsBySubscription.get(sourceGroupId);
    if (!forced?.has(ruleId)) return;
    forced.delete(ruleId);
    if (!forced.size) this.#forcedRuleIdsBySubscription.delete(sourceGroupId);
    let targetForced = this.#forcedRuleIdsBySubscription.get(targetGroupId);
    if (!targetForced) {
      targetForced = new Set<string>();
      this.#forcedRuleIdsBySubscription.set(targetGroupId, targetForced);
    }
    targetForced.add(ruleId);
  }

  performSearch() {
    const query = this.normalizedSearch;
    this.#resetForcedVisibility(query);

    if (!query) {
      this.visibleSubscriptions = this.data.map((_, index) => ({
        group_index: index,
        ruleIndices: null,
      }));
      this.searchPending = false;
      return;
    }

    const subscriptions = this.data;
    const nextVisible: VisibleSubscription[] = [];

    for (let i = 0; i < subscriptions.length; i++) {
      const subscription = subscriptions[i];
      const isForcedSubscription = this.#forcedSubscriptionIds.has(subscription.id);

      const nameMatches = (subscription.name || "").toLowerCase().includes(query);
      const urlMatches = (subscription.url || "").toLowerCase().includes(query);

      if (isForcedSubscription || nameMatches || urlMatches) {
        nextVisible.push({ group_index: i, ruleIndices: null });
        continue;
      }

      const matchedRuleIndices: number[] = [];
      const forcedRules = this.#forcedRuleIdsBySubscription.get(subscription.id);
      const rules = subscription.rules;

      for (let ruleIndex = 0; ruleIndex < rules.length; ruleIndex++) {
        const rule = rules[ruleIndex];
        const isForcedRule = forcedRules?.has(rule.id);
        const patternMatches = (rule.rule || "").toLowerCase().includes(query);
        if (isForcedRule || patternMatches) {
          matchedRuleIndices.push(ruleIndex);
        }
      }

      if (matchedRuleIndices.length > 0) {
        nextVisible.push({ group_index: i, ruleIndices: matchedRuleIndices });
      }
    }

    this.visibleSubscriptions = nextVisible;
    this.searchPending = false;
  }

  async saveChanges() {
    if (this.importPending) {
      await this.delegateSave?.();
      return;
    }
    if (!this.canSave) return;
    this.#loadGeneration++;
    this.saving = true;
    overlay.show(t("saving changes..."));
    const rawData = $state.snapshot(this.data).map((subscription) => ({
      ...subscription,
      url: normalizeSubscriptionUrl(subscription.url),
    }));
    try {
      const subscriptions = rawData.map((sub) =>
        buildSubscriptionUpdate(sub, this.#savedRules.get(sub.id)),
      );
      await fetcher.put("/subscriptions", { subscriptions });
      // The C backend persists user-authored exceptions by stable rule
      // pattern, not by short-lived downloaded rule IDs. Keep local editor
      // baselines in sync, including before a page reload.
      for (let i = 0; i < rawData.length; i++) {
        const byRule = new Map((rawData[i].overrides ?? []).map((o) => [o.rule, { ...o }]));
        for (const change of subscriptions[i].ruleChanges) {
          byRule.set(change.rule, {
            ...byRule.get(change.rule),
            rule: change.rule,
            ...(change.type !== change.previousType ? { type: change.type } : {}),
            ...(change.enable !== change.previousEnable ? { enable: change.enable } : {}),
          });
        }
        if (byRule.size) rawData[i].overrides = [...byRule.values()];
      }
      this.#savedRules = new Map(rawData.map((sub) => [sub.id, snapshotRules(sub.rules)]));
      this.tracker.reset(rawData);
      this.persistencePending = false;
      toast.success(t("Saved"));
    } catch (error) {
      const applied = appliedSubscriptions(error);
      if (applied) {
        this.#savedRules = new Map(applied.map((sub) => [sub.id, snapshotRules(sub.rules)]));
        this.tracker.reset(applied);
        this.persistencePending = true;
        this.markDataRevision();
      }
      // The fetcher reports HTTP errors. This also covers a missing/stale
      // local baseline without ever falling back to a huge rules upload.
      if (
        error instanceof Error &&
        error.message === "Subscription rules changed; reload before saving"
      ) {
        toast.error(t("Subscription rules changed; reload before saving"));
      }
    } finally {
      overlay.hide();
      this.saving = false;
    }
  }

  checkRulesValidityState = () => {
    this.#validatedRevision = this.dataRevision;
    this.valid_rules = this.data.every((subscription) =>
      subscription.rules.every((rule) => {
        const pattern = subscriptionRulePattern(rule.rule, rule.type);
        const typedWildcard =
          rule.type === "wildcard" &&
          (rule.rule.startsWith("DOMAIN-KEYWORD,") || rule.rule.startsWith("DOMAIN-WILDCARD,"));
        return (
          pattern.length > 0 &&
          (typedWildcard || !VALIDATOP_MAP[rule.type] || VALIDATOP_MAP[rule.type](pattern))
        );
      }),
    );
  };

  markDataRevision = () => {
    this.dataRevision += 1;
  };

  normalizeSubscriptionUrl(index: number) {
    const subscription = this.data[index];
    if (!subscription) return;

    const normalizedUrl = normalizeSubscriptionUrl(subscription.url);
    if (subscription.url === normalizedUrl) return;

    subscription.url = normalizedUrl;
    this.markDataRevision();
  }

  async syncSubscription(index: number) {
    const subscription = this.data[index];
    if (!subscription) return;
    if (this.importPending) {
      toast.info(t("Save the imported configuration before syncing subscriptions."));
      return;
    }

    this.normalizeSubscriptionUrl(index);
    const normalizedUrl = normalizeSubscriptionUrl(subscription.url);
    const urlError = this.subscriptionUrlErrors.get(subscription.id);
    if (urlError) {
      toast.error(t(urlError));
      return;
    }

    overlay.show(t("syncing..."));
    try {
      const updated = await fetcher.post<{
        rules: SubscriptionRule[];
        lastUpdate: number;
        url?: string;
      }>(`/subscriptions/${subscription.id}/sync`, { url: normalizedUrl });

      this.#savedRules.set(subscription.id, snapshotRules(updated.rules));
      this.tracker.acknowledgeUpdate(subscription, {
        url: updated.url ?? normalizedUrl,
        rules: updated.rules,
        lastUpdate: updated.lastUpdate,
      });

      this.markDataRevision();
      toast.success(t("Synced"));
    } catch (error) {
      console.error(error);
      toast.error(t("Failed to sync"));
    } finally {
      overlay.hide();
    }
  }

  async deleteSubscription(index: number, confirmed = false) {
    if (!confirmed && !confirm(t("Delete this subscription?"))) return;

    const removed = this.data[index];
    if (!removed) return;
    if (this.importPending) {
      this.data.splice(index, 1);
      delete this.open_state[removed.id];
      this.removeForcedGroup(removed.id);
      this.markDataRevision();
      return;
    }

    overlay.show(t("deleting subscription..."));
    try {
      await fetcher.delete(`/subscriptions/${removed.id}`);
      this.data.splice(index, 1);
      this.tracker.acknowledgeDelete(this.data, removed.id);
      this.#savedRules.delete(removed.id);
      delete this.open_state[removed.id];
      this.removeForcedGroup(removed.id);
      this.markDataRevision();
      toast.success(t("Deleted"));
    } catch (error) {
      console.error(error);
      toast.error(t("Failed to delete subscription"));
    } finally {
      overlay.hide();
    }
  }

  async addSubscription(payload: AddSubscriptionPayload) {
    if (this.importPending) {
      let id: string;
      do {
        id = randomId();
      } while (this.data.some((subscription) => subscription.id === id));
      this.data.unshift({
        ...payload,
        id,
        url: normalizeSubscriptionUrl(payload.url),
        enable: true,
        priority: DEFAULT_SUBSCRIPTION_PRIORITY,
        rules: [],
        lastUpdate: 0,
      });
      this.open_state[id] = true;
      if (this.searchActive) this.forceVisibleGroup(id);
      this.markDataRevision();
      return;
    }
    overlay.show(t("Adding..."));
    try {
      // Fetch/parse on the server; never echo the preview's thousands of
      // rules back into the request, and use canonical server-assigned IDs.
      const { subscription } = await fetcher.post<{ subscription: Subscription }>(
        "/subscriptions?fetch=true",
        {
          ...payload,
          url: normalizeSubscriptionUrl(payload.url),
          enable: true,
          priority: DEFAULT_SUBSCRIPTION_PRIORITY,
        },
      );
      const nextSubscription = withPriorityDefault(subscription, DEFAULT_SUBSCRIPTION_PRIORITY);
      this.#savedRules.set(nextSubscription.id, snapshotRules(nextSubscription.rules));
      this.data.unshift(nextSubscription);
      this.tracker.acknowledgeNewItem(this.data, nextSubscription, "start");
      this.open_state[nextSubscription.id] = true;
      if (this.searchActive) {
        this.forceVisibleGroup(nextSubscription.id);
      }
      this.markDataRevision();
      toast.success(t("Added"));
    } catch (error) {
      console.error(error);
      toast.error(t("Failed to add subscription"));
    } finally {
      overlay.hide();
    }
  }

  changeSubscriptionIndex(
    from_index: number,
    to_index: number,
    insert: "before" | "after" = "before",
  ) {
    if (from_index === to_index && insert !== "after") return;
    if (from_index < 0 || from_index >= this.data.length) return;

    const subscription = this.data[from_index];
    if (!subscription) return;

    this.data.splice(from_index, 1);

    let target = insert === "after" ? to_index + 1 : to_index;
    if (from_index < target) target -= 1;

    target = clamp(target, 0, this.data.length);
    this.data.splice(target, 0, subscription);

    this.markDataRevision();
  }

  handleSubscriptionSlotDrop = (source: SubscriptionDragData, target: SubscriptionDropSlotData) => {
    const { group_index: from_index } = source;
    const { group_index: to_index, insert } = target;
    this.changeSubscriptionIndex(from_index, to_index, insert);
  };
}
