import { tick } from "svelte";

import { t } from "../../data/locale.svelte";
import { ChangeTracker } from "../../utils/change-tracker.svelte";

import { type Group, type Rule } from "../../types";
import { isAcceptedRegex } from "../../utils/accepted-regex";
import { appliedGroups } from "../../utils/applied-save";
import { CooperativeBudget } from "../../utils/cooperative-data";
import { defaultGroup, defaultRule } from "../../utils/defaults";
import { overlay, toast } from "../../utils/events";
import { fetcher } from "../../utils/fetcher";
import { DEFAULT_GROUP_PRIORITY, isValidPriority, withPriorityDefault } from "../../utils/priority";
import { type SortDirection, type SortField } from "../../utils/rule-sorter";
import { VALIDATOP_MAP } from "../../utils/rule-validators";
import { buildGroupUpdate, snapshotGroupRules } from "./group-payload";
import {
  cloneGroupsWithNewIds as cloneGroupsWithNewIdsData,
  cloneGroupWithNewIds as cloneGroupWithNewIdsData,
  prependGroups as prependGroupsData,
  restoreGroupRulesOrder as restoreGroupRulesOrderData,
  sortGroupRules as sortGroupRulesData,
  toConfigPayload as toConfigPayloadData,
} from "./groups-data";
import { captureRuleMove } from "./rule-motion";

export const GROUPS_STORE_CONTEXT = Symbol("groups-store");

const SEARCH_DEBOUNCE_MS = 150 as const;
const IMPORT_GROUPS_CLONE_CHUNK_SIZE = 20 as const;
const IMPORT_GROUPS_INSERT_CHUNK_SIZE = 25 as const;
export const RULE_SEARCH_MATCH_NAME = 1 << 0;
export const RULE_SEARCH_MATCH_PATTERN = 1 << 1;

const clamp = (value: number, min: number, max: number) => Math.max(min, Math.min(max, value));

export type VisibleGroup = {
  group_index: number;
  ruleIndices: number[] | null;
};

export type GroupDragData = {
  group_id: string;
  group_index: number;
  name: string;
  color: string;
  count: number;
};

export type GroupDropSlotData = {
  group_index: number;
  insert: "before" | "after";
};

export type GroupDuplicateConflict = {
  ruleId: string;
  ruleName: string;
  rulePattern: string;
  ruleType: string;
  groupId: string;
  groupName: string;
  groupColor: string;
  inCurrentGroup: boolean;
  duplicateKey: string;
  totalRulesWithSameKey: number;
};

export type DuplicateRuleFocusRequest = {
  groupId: string;
  ruleId: string;
  nonce: number;
};

type DuplicateComputationResult = {
  duplicateRules: Set<string>;
  duplicateGroups: Set<string>;
  duplicateRuleKeys: Map<string, string>;
  conflictsByKey: Map<string, GroupDuplicateConflict[]>;
  keysByGroup: Map<string, Set<string>>;
};

type SearchIndexRule = {
  id: string;
  nameLower: string;
  patternLower: string;
};

type SearchIndexGroup = {
  id: string;
  nameLower: string;
  rules: SearchIndexRule[];
};

type SearchHighlightSegment = {
  text: string;
  matched: boolean;
};

type GroupsStoreOptions = {
  onRenderComplete?: () => void;
  isActive?: () => boolean;
};

export class GroupsStore {
  onRenderComplete?: () => void;
  private isActive: () => boolean;

  tracker = $state(new ChangeTracker<Group[]>([]));
  #savedRules = new Map<string, Rule[]>();
  #loadGeneration = 0;
  #focusGeneration = 0;
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
  valid_priorities = $derived(this.data.every((group) => isValidPriority(group.priority)));
  canSave = $derived(
    this.hasUnsavedChanges &&
      this.data.every((group) => Boolean(group.name.trim())) &&
      this.valid_rules &&
      this.valid_priorities &&
      !this.saving,
  );

  open_state = $state<Record<string, boolean>>({});
  panelState = $state<
    Record<
      string,
      {
        sortField: SortField | null;
        sortDirection: SortDirection;
        initialOrderIds: string[] | null;
      }
    >
  >({});
  ensureGroupVisible?: (groupId: string) => Promise<void>;
  ruleScrollers = new Map<string, (ruleId: string) => Promise<void>>();

  getPanelState(groupId: string) {
    this.panelState[groupId] ??= { sortField: null, sortDirection: "asc", initialOrderIds: null };
    return this.panelState[groupId];
  }

  searchValue = $state("");
  visibleGroups = $state<VisibleGroup[]>([]);
  searchPending = $state(false);
  searchMatchedGroupIds = $state<Set<string>>(new Set());
  searchRuleMatchMaskById = $state<Map<string, number>>(new Map());

  normalizedSearch = $derived(this.searchValue.trim().toLowerCase());
  searchActive = $derived(Boolean(this.normalizedSearch));

  searchIndex = $state<SearchIndexGroup[]>([]);
  searchIndexRevision = $state(-1);

  visibilityMap = $derived(new Map(this.visibleGroups.map((v) => [v.group_index, v.ruleIndices])));

  firstVisibleGroupIndex = $derived(
    this.searchActive ? (this.visibleGroups.length ? this.visibleGroups[0].group_index : -1) : 0,
  );

  noVisibleGroups = $derived(
    this.searchActive && !this.searchPending && this.visibleGroups.length === 0,
  );

  fetchError = $state(false);
  dataLoaded = $state(false);

  // Readiness follows loaded data, never the number of mounted virtual rows.
  isAllRendered = $derived(this.dataLoaded);

  isEmptyData = $derived(
    this.dataLoaded &&
      !this.fetchError &&
      !this.searchActive &&
      !this.searchPending &&
      this.data.length === 0,
  );
  duplicateRuleIds = $state<Set<string>>(new Set());
  duplicateGroupIds = $state<Set<string>>(new Set());
  duplicateRuleKeys = $state<Map<string, string>>(new Map());
  // Detailed rows are requested only by an open menu. Sharing the key index
  // avoids materializing every group's copy of all matching rules at once.
  duplicateConflictsByGroup = new Map<string, GroupDuplicateConflict[]>();
  #conflictsByKey = new Map<string, GroupDuplicateConflict[]>();
  #duplicateKeysByGroup = new Map<string, Set<string>>();
  #duplicateIndexVersion = $state(0);
  #duplicateWorkGeneration = 0;
  #validationGeneration = 0;
  activeDuplicateKey = $state<string | null>(null);
  duplicateHighlightPinned = $state(false);
  highlightedRuleId = $state<string | null>(null);
  duplicateFocusRequest = $state<DuplicateRuleFocusRequest | null>(null);

  #forcedGroupIds = new Set<string>();
  #forcedRuleIdsByGroup = new Map<string, Set<string>>();
  #forcedSearchKey = "";
  #debounceTimer: number | null = null;
  #duplicateRuleIdsTimer: number | null = null;
  #highlightedRuleTimer: number | null = null;
  #searchIndexBuildToken = 0;
  #searchIndexBuilding = false;
  #dispose: (() => void) | null = null;

  constructor(options: GroupsStoreOptions = {}) {
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
          if (this.#validatedRevision !== this.dataRevision) void this.#checkRulesValidityAsync();
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
          this.#cancelSearchIndexBuild();
          this.visibleGroups = this.data.map((_, index) => ({
            group_index: index,
            ruleIndices: null,
          }));
          this.#clearSearchMatches();
          this.searchPending = false;
          return;
        }

        this.searchPending = true;

        if (this.searchIndexRevision !== this.dataRevision) {
          this.#startSearchIndexBuild();
        }

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

        if (this.#duplicateRuleIdsTimer) {
          clearTimeout(this.#duplicateRuleIdsTimer);
          this.#duplicateRuleIdsTimer = null;
        }

        if (this.#highlightedRuleTimer) {
          clearTimeout(this.#highlightedRuleTimer);
          this.#highlightedRuleTimer = null;
        }
      };
    });
  }

  destroy() {
    this.#loadGeneration++;
    this.#duplicateWorkGeneration++;
    this.#validationGeneration++;
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
        (await fetcher.get<{ groups: Group[] }>("/groups?with_rules=true"))?.groups ?? []
      ).map((group) => withPriorityDefault(group, DEFAULT_GROUP_PRIORITY));
      if (generation !== this.#loadGeneration) return;
      if (options.preserveDrafts) this.tracker.rebase(fetched);
      else this.tracker = new ChangeTracker(fetched);
      this.#savedRules = snapshotGroupRules(fetched);
      this.markDataRevision();
      if (typeof window !== "undefined") {
        setTimeout(() => {
          if (this.#validatedRevision !== this.dataRevision) void this.#checkRulesValidityAsync();
        }, 10);
      }
    } catch (error) {
      if (generation === this.#loadGeneration) {
        this.fetchError = true;
        console.error("Failed to load groups:", error);
      }
    } finally {
      if (generation === this.#loadGeneration) {
        this.refreshDuplicateRuleIds();
        this.dataLoaded = true;
      }
    }
  };

  /** Stage editor values against the actual server collection, without saving. */
  stageImport(baseline: readonly Group[], draft: readonly Group[]) {
    const saved = structuredClone([...baseline]).map((group) =>
      withPriorityDefault(group, DEFAULT_GROUP_PRIORITY),
    );
    const next = structuredClone([...draft]).map((group) =>
      withPriorityDefault(group, DEFAULT_GROUP_PRIORITY),
    );
    this.#loadGeneration++;
    this.#savedRules = snapshotGroupRules(saved);
    this.tracker.reset(saved);
    this.tracker.replaceDraft(next);
    this.fetchError = false;
    this.dataLoaded = true;
    this.markDataRevision();
    this.refreshDuplicateRuleIds();
  }

  /** Build the replacement offscreen; the page commits all prepared sections together. */
  async prepareImport(baseline: readonly Group[], draft: readonly Group[]) {
    const generation = this.#loadGeneration;
    const revision = this.dataRevision;
    const saved = baseline.map((group) => withPriorityDefault(group, DEFAULT_GROUP_PRIORITY));
    const next = draft.map((group) => withPriorityDefault(group, DEFAULT_GROUP_PRIORITY));
    const tracker = await ChangeTracker.fromDraftAsync(saved, next);
    const savedRules = new Map<string, Rule[]>();
    let deadline = performance.now() + 4;
    for (const group of saved) {
      const rules: Rule[] = [];
      for (const rule of group.rules) {
        rules.push({ ...rule });
        if (rules.length % 256 === 0 && performance.now() >= deadline) {
          await this.#yieldToMain();
          deadline = performance.now() + 4;
        }
      }
      savedRules.set(group.id, rules);
    }
    let validRules = true;
    for (const group of next) {
      for (let index = 0; index < group.rules.length; index++) {
        const rule = group.rules[index];
        if (
          !rule.rule ||
          (!isAcceptedRegex(rule, savedRules.get(group.id)) &&
            VALIDATOP_MAP[rule.type] &&
            !VALIDATOP_MAP[rule.type](rule.rule))
        )
          validRules = false;
        if (index % 256 === 0 && performance.now() >= deadline) {
          await this.#yieldToMain();
          deadline = performance.now() + 4;
        }
      }
    }
    const work = this.#duplicateComputation(next);
    let result = work.next();
    while (!result.done) {
      if (performance.now() >= deadline) {
        await this.#yieldToMain();
        deadline = performance.now() + 4;
      }
      result = work.next();
    }
    const duplicates = result.value;
    const validate = () => {
      if (generation !== this.#loadGeneration || revision !== this.dataRevision)
        throw new Error("Configuration changed; refresh the import preview");
    };
    return {
      validate,
      commit: () => {
        validate();
        this.#loadGeneration++;
        if (this.#duplicateRuleIdsTimer) {
          clearTimeout(this.#duplicateRuleIdsTimer);
          this.#duplicateRuleIdsTimer = null;
        }
        this.#savedRules = savedRules;
        this.tracker = tracker;
        this.fetchError = false;
        this.dataLoaded = true;
        this.valid_rules = validRules;
        this.markDataRevision();
        this.#validatedRevision = this.dataRevision;
        if (this.#duplicateRuleIdsTimer) {
          clearTimeout(this.#duplicateRuleIdsTimer);
          this.#duplicateRuleIdsTimer = null;
        }
        this.#applyDuplicateComputation(duplicates);
      },
    };
  }

  /** A confirmed import save becomes the baseline; unrelated drafts stay editable. */
  acceptImport(
    baseline: readonly Group[],
    retainedDraft: readonly Group[] = baseline,
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

  forceVisibleGroup(groupId: string) {
    if (!this.normalizedSearch) return;
    this.#forcedGroupIds.add(groupId);
    this.#forcedSearchKey = this.normalizedSearch;
  }

  forceVisibleRule(groupId: string, ruleId: string) {
    if (!this.normalizedSearch) return;
    let forced = this.#forcedRuleIdsByGroup.get(groupId);
    if (!forced) {
      forced = new Set<string>();
      this.#forcedRuleIdsByGroup.set(groupId, forced);
    }
    forced.add(ruleId);
    this.#forcedSearchKey = this.normalizedSearch;
  }

  removeForcedGroup(groupId: string) {
    this.#forcedGroupIds.delete(groupId);
    this.#forcedRuleIdsByGroup.delete(groupId);
  }

  removeForcedRule(groupId: string, ruleId: string) {
    const forced = this.#forcedRuleIdsByGroup.get(groupId);
    if (!forced) return;
    forced.delete(ruleId);
    if (!forced.size) this.#forcedRuleIdsByGroup.delete(groupId);
  }

  moveForcedRule(sourceGroupId: string, targetGroupId: string, ruleId: string) {
    if (sourceGroupId === targetGroupId) return;
    const forced = this.#forcedRuleIdsByGroup.get(sourceGroupId);
    if (!forced?.has(ruleId)) return;
    forced.delete(ruleId);
    if (!forced.size) this.#forcedRuleIdsByGroup.delete(sourceGroupId);
    let targetForced = this.#forcedRuleIdsByGroup.get(targetGroupId);
    if (!targetForced) {
      targetForced = new Set<string>();
      this.#forcedRuleIdsByGroup.set(targetGroupId, targetForced);
    }
    targetForced.add(ruleId);
  }

  syncRuleDeletion(groupIndex: number, ruleIndex: number) {
    if (!this.searchActive) return;
    const targetIndex = this.visibleGroups.findIndex((entry) => entry.group_index === groupIndex);
    if (targetIndex === -1) return;
    const target = this.visibleGroups[targetIndex];
    if (!target?.ruleIndices) return;

    const ruleIndices = target.ruleIndices;
    let write = 0;
    for (let i = 0; i < ruleIndices.length; i++) {
      const index = ruleIndices[i];
      if (index === ruleIndex) continue;
      ruleIndices[write] = index > ruleIndex ? index - 1 : index;
      write += 1;
    }

    if (write === 0) {
      this.visibleGroups.splice(targetIndex, 1);
      return;
    }

    ruleIndices.length = write;
    target.ruleIndices = ruleIndices;
    this.visibleGroups[targetIndex] = target;
  }

  #resetForcedVisibility(searchKey: string) {
    if (!this.#forcedSearchKey) return;
    if (this.#forcedSearchKey !== searchKey) {
      this.#forcedGroupIds.clear();
      this.#forcedRuleIdsByGroup.clear();
      this.#forcedSearchKey = "";
    }
  }

  #clearSearchMatches() {
    this.searchMatchedGroupIds = new Set();
    this.searchRuleMatchMaskById = new Map();
  }

  #splitSearchHighlightSegments(
    value: string,
    query: string,
  ): SearchHighlightSegment[] | undefined {
    if (!value || !query) return undefined;

    const source = `${value}`;
    const needle = query.trim().toLowerCase();
    if (!needle) return undefined;

    const haystack = source.toLowerCase();
    const needleLength = needle.length;

    let cursor = 0;
    let matchIndex = haystack.indexOf(needle, cursor);
    if (matchIndex === -1) return undefined;

    const segments: SearchHighlightSegment[] = [];

    while (matchIndex !== -1) {
      if (matchIndex > cursor) {
        segments.push({ text: source.slice(cursor, matchIndex), matched: false });
      }

      const matchEnd = matchIndex + needleLength;
      segments.push({ text: source.slice(matchIndex, matchEnd), matched: true });

      cursor = matchEnd;
      matchIndex = haystack.indexOf(needle, cursor);
    }

    if (cursor < source.length) {
      segments.push({ text: source.slice(cursor), matched: false });
    }

    return segments;
  }

  #cancelSearchIndexBuild() {
    this.#searchIndexBuildToken += 1;
  }

  #startSearchIndexBuild(incrementToken = true) {
    if (incrementToken) {
      this.#searchIndexBuildToken += 1;
    }

    if (this.#searchIndexBuilding) return;
    void this.#buildSearchIndex(this.#searchIndexBuildToken);
  }

  async #buildSearchIndex(token: number) {
    if (!this.searchActive) return;

    this.#searchIndexBuilding = true;
    const revision = this.dataRevision;
    const groups = this.data;
    const total = groups.length;
    const nextIndex = new Array<SearchIndexGroup>(total);

    const now = () => (typeof performance !== "undefined" ? performance.now() : Date.now());
    let lastYield = now();

    const maybeYield = async () => {
      if (now() - lastYield < 8) return;
      await this.#yieldToMain();
      lastYield = now();
    };

    const abortIfStale = () => token !== this.#searchIndexBuildToken || !this.searchActive;

    for (let i = 0; i < total; i++) {
      if (abortIfStale()) {
        this.#searchIndexBuilding = false;
        if (this.searchActive && token !== this.#searchIndexBuildToken) {
          this.#startSearchIndexBuild(false);
        }
        return;
      }

      const group = groups[i];
      const rules = group.rules;
      const rulesCount = rules.length;
      const indexedRules = new Array<SearchIndexRule>(rulesCount);

      for (let r = 0; r < rulesCount; r++) {
        const rule = rules[r];
        indexedRules[r] = {
          id: rule.id,
          nameLower: (rule.name || "").toLowerCase(),
          patternLower: (rule.rule || "").toLowerCase(),
        };
        if ((r & 31) === 0) {
          await maybeYield();
          if (abortIfStale()) {
            this.#searchIndexBuilding = false;
            if (this.searchActive && token !== this.#searchIndexBuildToken) {
              this.#startSearchIndexBuild(false);
            }
            return;
          }
        }
      }

      nextIndex[i] = {
        id: group.id,
        nameLower: (group.name || "").toLowerCase(),
        rules: indexedRules,
      };

      if ((i & 7) === 0) {
        await maybeYield();
      }
    }

    if (abortIfStale()) {
      this.#searchIndexBuilding = false;
      if (this.searchActive && token !== this.#searchIndexBuildToken) {
        this.#startSearchIndexBuild(false);
      }
      return;
    }

    if (this.dataRevision !== revision) {
      this.#searchIndexBuilding = false;
      if (this.searchActive) {
        this.#startSearchIndexBuild(false);
      }
      return;
    }

    this.searchIndex = nextIndex;
    this.searchIndexRevision = revision;
    this.#searchIndexBuilding = false;

    if (this.normalizedSearch) {
      this.performSearch();
    } else {
      this.searchPending = false;
    }
  }

  performSearch() {
    const query = this.normalizedSearch;
    this.#resetForcedVisibility(query);

    if (!query) {
      this.visibleGroups = this.data.map((_, index) => ({
        group_index: index,
        ruleIndices: null,
      }));
      this.#clearSearchMatches();
      this.searchPending = false;
      return;
    }

    if (this.searchIndexRevision !== this.dataRevision) {
      this.#clearSearchMatches();
      this.searchPending = true;
      return;
    }

    const nextVisible: VisibleGroup[] = [];
    const matchedGroupIds = new Set<string>();
    const ruleMatchMaskById = new Map<string, number>();
    const searchIndex = this.searchIndex;
    const len = searchIndex.length;

    for (let i = 0; i < len; i++) {
      const indexedGroup = searchIndex[i];
      const isForcedGroup = this.#forcedGroupIds.has(indexedGroup.id);
      const isGroupMatchedByName = indexedGroup.nameLower.includes(query);

      if (isForcedGroup || isGroupMatchedByName) {
        if (isGroupMatchedByName) {
          matchedGroupIds.add(indexedGroup.id);
        }
        nextVisible.push({ group_index: i, ruleIndices: null });
        continue;
      }

      const matchedRuleIndices: number[] = [];
      const forcedRules = this.#forcedRuleIdsByGroup.get(indexedGroup.id);
      const rules = indexedGroup.rules;
      const rulesLen = rules.length;

      for (let r = 0; r < rulesLen; r++) {
        const indexedRule = rules[r];
        let matchMask = 0;
        if (indexedRule.nameLower.includes(query)) {
          matchMask |= RULE_SEARCH_MATCH_NAME;
        }
        if (indexedRule.patternLower.includes(query)) {
          matchMask |= RULE_SEARCH_MATCH_PATTERN;
        }

        if (forcedRules?.has(indexedRule.id) || matchMask !== 0) {
          matchedRuleIndices.push(r);
          if (matchMask !== 0) {
            ruleMatchMaskById.set(indexedRule.id, matchMask);
          }
        }
      }

      if (matchedRuleIndices.length > 0) {
        nextVisible.push({ group_index: i, ruleIndices: matchedRuleIndices });
      }
    }

    this.visibleGroups = nextVisible;
    this.searchMatchedGroupIds = matchedGroupIds;
    this.searchRuleMatchMaskById = ruleMatchMaskById;
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
    try {
      const rawData = $state.snapshot(this.data);
      const groups = rawData.map((group) =>
        buildGroupUpdate(group, this.#savedRules.get(group.id)),
      );
      const saved = await fetcher.put<{ groups: Group[] }>("/groups?save=true", { groups });
      // The server assigns IDs to new/moved rules. Never keep client-only IDs
      // as the next edit's baseline; that used to regenerate IDs on every save.
      const savedGroups = saved.groups.map((group) =>
        withPriorityDefault(group, DEFAULT_GROUP_PRIORITY),
      );
      this.#savedRules = snapshotGroupRules(savedGroups);
      this.tracker.reset(savedGroups);
      this.persistencePending = false;
      this.markDataRevision();
      toast.success(t("Saved"));
    } catch (error) {
      const applied = appliedGroups(error);
      if (applied) {
        // Runtime apply succeeded (including newly assigned IDs), disk write did
        // not. Retry against that exact live baseline while still warning about
        // unsaved data, even when there are no additional editor changes.
        this.#savedRules = snapshotGroupRules(applied);
        this.tracker.reset(applied);
        this.persistencePending = true;
        this.markDataRevision();
      }
      // Other failures retain both the local edits and the old baseline.
    } finally {
      overlay.hide();
      this.saving = false;
    }
  }

  isSavedRegexRule = (groupId: string, rule: Rule) =>
    isAcceptedRegex(rule, this.#savedRules.get(groupId));

  checkRulesValidityState = () => {
    this.#validationGeneration++;
    this.#validatedRevision = this.dataRevision;
    this.valid_rules = this.data.every((group) =>
      group.rules.every(
        (rule) =>
          Boolean(rule.rule) &&
          (this.isSavedRegexRule(group.id, rule) ||
            !VALIDATOP_MAP[rule.type] ||
            VALIDATOP_MAP[rule.type](rule.rule)),
      ),
    );
  };

  async #checkRulesValidityAsync() {
    const generation = ++this.#validationGeneration;
    const revision = this.dataRevision;
    const budget = new CooperativeBudget();
    let valid = true;
    // A pending scan must not allow saving an as-yet unvalidated edit.
    this.valid_rules = false;
    try {
      for (const group of this.data) {
        for (const rule of group.rules) {
          if (
            !rule.rule ||
            (!this.isSavedRegexRule(group.id, rule) &&
              VALIDATOP_MAP[rule.type] &&
              !VALIDATOP_MAP[rule.type](rule.rule))
          ) {
            valid = false;
            break;
          }
          const pause = budget.checkpoint();
          if (pause) {
            await pause;
            if (generation !== this.#validationGeneration || revision !== this.dataRevision) return;
          }
        }
        if (!valid) break;
      }
      if (generation === this.#validationGeneration && revision === this.dataRevision) {
        this.valid_rules = valid;
        this.#validatedRevision = revision;
      }
    } finally {
      budget.close();
    }
  }

  *#sortDuplicateConflicts(conflicts: GroupDuplicateConflict[]): Generator<void, void, void> {
    const compare = (a: GroupDuplicateConflict, b: GroupDuplicateConflict) => {
      const byKey = a.duplicateKey.localeCompare(b.duplicateKey);
      if (byKey !== 0) return byKey;

      if (a.inCurrentGroup !== b.inCurrentGroup) {
        return a.inCurrentGroup ? 1 : -1;
      }
      const byGroup = (a.groupName || "").localeCompare(b.groupName || "");
      if (byGroup !== 0) return byGroup;
      const byName = (a.ruleName || "").localeCompare(b.ruleName || "");
      if (byName !== 0) return byName;
      const byPattern = (a.rulePattern || "").localeCompare(b.rulePattern || "");
      if (byPattern !== 0) return byPattern;
      return a.ruleId.localeCompare(b.ruleId);
    };
    if (conflicts.length < 256) {
      conflicts.sort(compare);
      return;
    }
    // A single shared pattern can itself contain 100k matches. Built-in sort
    // cannot yield; merge in bounded pieces for the background index builder.
    let source = conflicts;
    let target = new Array<GroupDuplicateConflict>(conflicts.length);
    let operations = 0;
    for (let width = 1; width < conflicts.length; width *= 2) {
      for (let start = 0; start < conflicts.length; start += width * 2) {
        const middle = Math.min(start + width, conflicts.length);
        const end = Math.min(start + width * 2, conflicts.length);
        let left = start;
        let right = middle;
        for (let index = start; index < end; index++) {
          target[index] =
            left < middle && (right >= end || compare(source[left], source[right]) <= 0)
              ? source[left++]
              : source[right++];
          if (++operations % 256 === 0) yield;
        }
      }
      [source, target] = [target, source];
    }
    if (source !== conflicts) {
      for (let index = 0; index < source.length; index++) {
        conflicts[index] = source[index];
        if (++operations % 256 === 0) yield;
      }
    }
  }

  #computeDuplicates(): DuplicateComputationResult {
    const work = this.#duplicateComputation(this.data);
    let result = work.next();
    while (!result.done) result = work.next();
    return result.value;
  }

  *#duplicateComputation(
    data: readonly Group[],
  ): Generator<void, DuplicateComputationResult, void> {
    const duplicateRules = new Set<string>();
    const duplicateGroups = new Set<string>();
    const duplicateRuleKeys = new Map<string, string>();
    type Entry = { rule: Rule; groupId: string; groupName: string; groupColor: string };
    const firstByKey = new Map<string, Entry>();
    const rulesByKey = new Map<string, Entry[]>();

    let visited = 0;
    for (const group of data) {
      const groupId = group.id;
      const groupName = group.name;
      const groupColor = group.color || "#ffffff";
      for (const rule of group.rules) {
        if (++visited % 256 === 0) yield;
        const pattern = rule.rule;
        if (!pattern.trim()) continue;
        const key = `${rule.type}:${pattern}`;
        const entry = { rule, groupId, groupName, groupColor };
        const first = firstByKey.get(key);
        if (!first) firstByKey.set(key, entry);
        else {
          let matches = rulesByKey.get(key);
          if (!matches) {
            matches = [first];
            rulesByKey.set(key, matches);
          }
          matches.push(entry);
        }
      }
    }

    const conflictsByKey = new Map<string, GroupDuplicateConflict[]>();
    const duplicateKeysByGroup = new Map<string, Set<string>>();

    for (const [key, rules] of rulesByKey) {
      yield;
      if (rules.length < 2) continue;

      for (const entry of rules) {
        duplicateRules.add(entry.rule.id);
        duplicateRuleKeys.set(entry.rule.id, key);
        duplicateGroups.add(entry.groupId);
        let keys = duplicateKeysByGroup.get(entry.groupId);
        if (!keys) {
          keys = new Set<string>();
          duplicateKeysByGroup.set(entry.groupId, keys);
        }
        keys.add(key);
        if (++visited % 256 === 0) yield;
      }

      const conflicts: GroupDuplicateConflict[] = [];
      for (const entry of rules) {
        conflicts.push({
          ruleId: entry.rule.id,
          ruleName: entry.rule.name,
          rulePattern: entry.rule.rule,
          ruleType: entry.rule.type,
          groupId: entry.groupId,
          groupName: entry.groupName,
          groupColor: entry.groupColor,
          inCurrentGroup: false,
          duplicateKey: key,
          totalRulesWithSameKey: rules.length,
        });
        if (++visited % 256 === 0) yield;
      }
      // Sort each shared key once. An open menu only needs to concatenate
      // these groups, with its own group's entries at the end of each key.
      yield* this.#sortDuplicateConflicts(conflicts);
      conflictsByKey.set(key, conflicts);
    }

    return {
      duplicateRules,
      duplicateGroups,
      duplicateRuleKeys,
      conflictsByKey,
      keysByGroup: duplicateKeysByGroup,
    };
  }

  #applyDuplicateComputation(result: DuplicateComputationResult) {
    this.duplicateRuleIds = result.duplicateRules;
    this.duplicateGroupIds = result.duplicateGroups;
    this.duplicateRuleKeys = result.duplicateRuleKeys;
    this.#conflictsByKey = result.conflictsByKey;
    this.#duplicateKeysByGroup = result.keysByGroup;
    this.duplicateConflictsByGroup.clear();
    this.#duplicateIndexVersion++;
    this.#syncActiveDuplicateKey();
  }

  #syncActiveDuplicateKey() {
    if (this.highlightedRuleId && !this.duplicateRuleIds.has(this.highlightedRuleId)) {
      this.highlightedRuleId = null;
    }

    if (!this.activeDuplicateKey) return;

    for (const ruleId of this.duplicateRuleIds) {
      if (this.duplicateRuleKeys.get(ruleId) === this.activeDuplicateKey) {
        return;
      }
    }

    this.activeDuplicateKey = null;
    this.duplicateHighlightPinned = false;
  }

  #resolveDuplicateKey(ruleId: string) {
    const key = this.duplicateRuleKeys.get(ruleId);
    if (!key || !this.duplicateRuleIds.has(ruleId)) {
      return null;
    }
    return key;
  }

  refreshDuplicateRuleIds = () => {
    this.#duplicateWorkGeneration++;
    if (this.#duplicateRuleIdsTimer) {
      clearTimeout(this.#duplicateRuleIdsTimer);
      this.#duplicateRuleIdsTimer = null;
    }
    this.#applyDuplicateComputation(this.#computeDuplicates());
  };

  scheduleDuplicateRuleIdsRefresh = (delayMs = 120) => {
    const generation = ++this.#duplicateWorkGeneration;
    if (typeof window === "undefined") {
      this.refreshDuplicateRuleIds();
      return;
    }

    if (this.#duplicateRuleIdsTimer) {
      clearTimeout(this.#duplicateRuleIdsTimer);
    }

    this.#duplicateRuleIdsTimer = window.setTimeout(() => {
      this.#duplicateRuleIdsTimer = null;
      void this.#refreshDuplicatesAsync(generation);
    }, delayMs);
  };

  async #refreshDuplicatesAsync(generation: number) {
    const revision = this.dataRevision;
    const work = this.#duplicateComputation(this.data);
    const budget = new CooperativeBudget();
    try {
      let result = work.next();
      while (!result.done) {
        const pause = budget.checkpoint(true);
        if (pause) {
          await pause;
          if (generation !== this.#duplicateWorkGeneration || revision !== this.dataRevision)
            return;
        }
        result = work.next();
      }
      if (generation === this.#duplicateWorkGeneration && revision === this.dataRevision)
        this.#applyDuplicateComputation(result.value);
    } finally {
      budget.close();
    }
  }

  isRuleDuplicate = (ruleId: string) => this.duplicateRuleIds.has(ruleId);
  getRuleSearchMatchMask = (ruleId: string) => this.searchRuleMatchMaskById.get(ruleId) ?? 0;
  getSearchHighlightParts = (value: string, query: string): SearchHighlightSegment[] | undefined =>
    this.#splitSearchHighlightSegments(value, query);

  pinDuplicateByRuleId = (ruleId: string) => {
    const key = this.#resolveDuplicateKey(ruleId);
    if (!key) {
      this.activeDuplicateKey = null;
      this.duplicateHighlightPinned = false;
      return false;
    }
    this.activeDuplicateKey = key;
    this.duplicateHighlightPinned = true;
    return true;
  };

  setActiveDuplicateByRuleId = (ruleId: string) => {
    if (this.duplicateHighlightPinned) return;
    const key = this.#resolveDuplicateKey(ruleId);
    if (!key) {
      this.activeDuplicateKey = null;
      return;
    }
    this.activeDuplicateKey = key;
  };

  clearActiveDuplicateByRuleId = (ruleId: string) => {
    if (this.duplicateHighlightPinned || !this.activeDuplicateKey) return;
    const key = this.#resolveDuplicateKey(ruleId);
    if (!key || key === this.activeDuplicateKey) {
      this.activeDuplicateKey = null;
    }
  };

  togglePinnedDuplicateByRuleId = (ruleId: string) => {
    const key = this.#resolveDuplicateKey(ruleId);
    if (!key) {
      this.activeDuplicateKey = null;
      this.duplicateHighlightPinned = false;
      return;
    }
    if (this.duplicateHighlightPinned && this.activeDuplicateKey === key) {
      this.activeDuplicateKey = null;
      this.duplicateHighlightPinned = false;
      return;
    }
    this.pinDuplicateByRuleId(ruleId);
  };

  clearPinnedDuplicateHighlight = () => {
    if (!this.duplicateHighlightPinned) return;
    this.activeDuplicateKey = null;
    this.duplicateHighlightPinned = false;
  };

  highlightRuleTemporarily = (ruleId: string, durationMs = 2200) => {
    if (!this.duplicateRuleIds.has(ruleId)) return false;

    if (this.#highlightedRuleTimer) {
      clearTimeout(this.#highlightedRuleTimer);
      this.#highlightedRuleTimer = null;
    }

    const applyHighlight = () => {
      this.highlightedRuleId = ruleId;

      if (typeof window === "undefined") return;

      this.#highlightedRuleTimer = window.setTimeout(() => {
        this.#highlightedRuleTimer = null;
        if (this.highlightedRuleId === ruleId) {
          this.highlightedRuleId = null;
        }
      }, durationMs);
    };

    if (this.highlightedRuleId === ruleId) {
      this.highlightedRuleId = null;
      if (typeof window !== "undefined") {
        requestAnimationFrame(() => applyHighlight());
      } else {
        applyHighlight();
      }
      return true;
    }

    applyHighlight();

    return true;
  };

  isRuleHighlighted = (ruleId: string) => this.highlightedRuleId === ruleId;

  isRuleHighlightPinned = (ruleId: string) => this.isRuleHighlighted(ruleId);

  requestDuplicateRuleFocus = (groupId: string, ruleId: string) => {
    const groupIndex = this.data.findIndex((group) => group.id === groupId);
    if (groupIndex < 0) return false;

    if (this.searchActive) {
      this.forceVisibleRule(groupId, ruleId);
      this.performSearch();
    }

    this.open_state[groupId] = true;

    const nonce = (this.duplicateFocusRequest?.nonce ?? 0) + 1;
    this.duplicateFocusRequest = { groupId, ruleId, nonce };
    void this.ensureGroupVisible?.(groupId);
    return true;
  };

  consumeDuplicateRuleFocus = (groupId: string, ruleId: string, nonce: number) => {
    const request = this.duplicateFocusRequest;
    if (!request) return;
    if (request.groupId !== groupId || request.ruleId !== ruleId || request.nonce !== nonce) return;
    this.duplicateFocusRequest = null;
  };

  getDuplicateConflictsForGroup = (groupId: string): GroupDuplicateConflict[] => {
    void this.#duplicateIndexVersion;
    const cached = this.duplicateConflictsByGroup.get(groupId);
    if (cached) return cached;
    const keys = this.#duplicateKeysByGroup.get(groupId);
    if (!keys) return [];
    const conflicts: GroupDuplicateConflict[] = [];
    for (const key of [...keys].sort((a, b) => a.localeCompare(b))) {
      const matching = this.#conflictsByKey.get(key) ?? [];
      for (const conflict of matching) if (conflict.groupId !== groupId) conflicts.push(conflict);
      for (const conflict of matching)
        if (conflict.groupId === groupId) conflicts.push({ ...conflict, inCurrentGroup: true });
    }
    // Visiting many groups must not accumulate another O(groups × rules) cache.
    if (this.duplicateConflictsByGroup.size >= 2)
      this.duplicateConflictsByGroup.delete(this.duplicateConflictsByGroup.keys().next().value!);
    this.duplicateConflictsByGroup.set(groupId, conflicts);
    return conflicts;
  };

  markDataRevision = () => {
    this.dataRevision += 1;
    this.scheduleDuplicateRuleIdsRefresh();
  };

  async addRuleToGroup(group_index: number, rule: Rule, focus = false) {
    const origin = typeof document === "undefined" ? null : document.activeElement;
    const originMenu = origin?.closest<HTMLElement>("[data-dropdown-menu-content]");
    const menuTrigger = originMenu
      ?.closest(".group-actions")
      ?.querySelector<HTMLElement>("[data-dropdown-menu-trigger]");
    const focusGeneration = focus ? ++this.#focusGeneration : this.#focusGeneration;
    const group = this.data[group_index];
    if (!group) return;
    group.rules.unshift(rule);
    this.markDataRevision();
    if (!rule.rule || !rule.name) {
      this.valid_rules = false;
    }
    if (this.searchActive) {
      this.forceVisibleRule(group.id, rule.id);
    }
    if (!focus) return;
    this.open_state[group.id] = true;
    await tick();
    await this.ensureGroupVisible?.(group.id);
    await this.ruleScrollers.get(group.id)?.(rule.id);
    await tick();
    const el = document.querySelector(
      `.rule[data-group-uuid="${group.id}"][data-uuid="${rule.id}"]`,
    );
    if (el) {
      // Mounting a distant virtual row can take several frames. An explicit
      // focus move or a newer Add action wins over this deferred autofocus.
      if (
        focusGeneration === this.#focusGeneration &&
        (document.activeElement === origin ||
          document.activeElement === menuTrigger ||
          document.activeElement === document.body ||
          !document.activeElement?.isConnected)
      ) {
        el.querySelector<HTMLInputElement>("div.name input")?.focus();
      }
      el.querySelector<HTMLInputElement>("div.pattern input")?.classList.add("invalid");
      this.checkRulesValidityState();
    }
  }

  deleteRuleFromGroup = (group_index: number, rule_index: number) => {
    const group = this.data[group_index];
    if (!group) return;
    const removed = group.rules[rule_index];
    group.rules.splice(rule_index, 1);
    if (removed) {
      this.removeForcedRule(group.id, removed.id);
    }
    this.syncRuleDeletion(group_index, rule_index);
    this.markDataRevision();
  };

  changeRuleIndex(
    from_group_index: number,
    from_rule_index: number,
    to_group_index: number,
    to_rule_index: number,
    to_rule_id?: string,
    insert: "before" | "after" = "before",
  ) {
    const sourceGroup = this.data[from_group_index];
    const targetGroup = this.data[to_group_index];

    if (!sourceGroup || !targetGroup) return;

    const isSameGroup = from_group_index === to_group_index;

    const sourceRules = sourceGroup.rules;
    const targetRules = targetGroup.rules;

    if (!sourceRules.length) return;

    const fromIndex = clamp(from_rule_index, 0, sourceRules.length - 1);
    const animateMove = captureRuleMove(sourceRules[fromIndex].id);
    const [movedRule] = sourceRules.splice(fromIndex, 1);
    if (!movedRule) return;

    if (!isSameGroup) {
      this.moveForcedRule(sourceGroup.id, targetGroup.id, movedRule.id);
    }

    let anchorIndex =
      to_rule_id && to_rule_id.length > 0 ? targetRules.findIndex((r) => r.id === to_rule_id) : -1;

    if (anchorIndex === -1 && targetRules.length > 0) {
      anchorIndex = clamp(to_rule_index, 0, targetRules.length - 1);
      if (isSameGroup && fromIndex < anchorIndex) {
        anchorIndex -= 1;
      }
    }

    let insertIndex: number;
    if (anchorIndex === -1) {
      insertIndex = insert === "after" ? targetRules.length : 0;
    } else {
      insertIndex = insert === "after" ? anchorIndex + 1 : anchorIndex;
    }

    insertIndex = clamp(insertIndex, 0, targetRules.length);
    targetRules.splice(insertIndex, 0, movedRule);

    this.markDataRevision();
    void animateMove();
  }

  changeGroupIndex(from_index: number, to_index: number, insert: "before" | "after" = "before") {
    if (from_index === to_index && insert !== "after") return;

    if (from_index < 0 || from_index >= this.data.length) return;

    const group = this.data[from_index];
    if (!group) return;

    this.data.splice(from_index, 1);

    let target = insert === "after" ? to_index + 1 : to_index;

    if (from_index < target) target -= 1;

    if (target < 0) target = 0;
    if (target > this.data.length) target = this.data.length;

    this.data.splice(target, 0, group);
    this.markDataRevision();
  }

  canDropGroup = (source: GroupDragData, target: GroupDropSlotData) => {
    const slot = target.group_index + (target.insert === "after" ? 1 : 0);
    return source.group_index !== slot && source.group_index + 1 !== slot;
  };

  handleGroupSlotDrop = (source: GroupDragData, target: GroupDropSlotData) => {
    if (!this.canDropGroup(source, target)) return;
    const { group_index: from_index } = source;
    const { group_index: to_index, insert } = target;
    if (from_index === to_index && insert !== "after") return;
    this.changeGroupIndex(from_index, to_index, insert);
  };

  async addGroup() {
    const origin = typeof document === "undefined" ? null : document.activeElement;
    const focusGeneration = ++this.#focusGeneration;
    const group = defaultGroup();
    this.data.unshift(group);
    this.open_state[group.id] = true;
    this.markDataRevision();
    if (this.searchActive) {
      this.forceVisibleGroup(group.id);
    }
    await this.addRuleToGroup(0, defaultRule(), false);
    await tick();
    await this.ensureGroupVisible?.(group.id);
    await tick();
    const el = document.querySelector(`.group-header[data-group-index="0"]`);
    if (
      focusGeneration === this.#focusGeneration &&
      (document.activeElement === origin ||
        document.activeElement === document.body ||
        !document.activeElement?.isConnected)
    ) {
      el?.querySelector<HTMLInputElement>("input.group-name")?.focus();
    }
  }

  deleteGroup = (index: number, confirmed = false) => {
    if (!confirmed && !confirm(t("Delete this group?"))) return;
    const removed = this.data[index];
    this.data.splice(index, 1);
    if (removed) {
      this.removeForcedGroup(removed.id);
      delete this.open_state[removed.id];
      delete this.panelState[removed.id];
    }
    this.markDataRevision();
  };

  cloneGroupWithNewIds(group: Group): Group {
    return cloneGroupWithNewIdsData(group);
  }

  sortGroupRules(groupIndex: number, field: SortField, direction: SortDirection) {
    const group = this.data[groupIndex];
    if (!group) return false;
    sortGroupRulesData(group, field, direction, (target, items) =>
      this.tracker.replaceArray(target, items),
    );
    this.markDataRevision();
    return true;
  }

  restoreGroupRulesOrder(groupIndex: number, ruleIds: string[]) {
    const group = this.data[groupIndex];
    if (!group) return false;

    const restored = restoreGroupRulesOrderData(group, ruleIds, (target, items) =>
      this.tracker.replaceArray(target, items),
    );
    if (!restored) return false;
    this.markDataRevision();
    return true;
  }

  toConfigPayload() {
    return toConfigPayloadData($state.snapshot(this.data));
  }

  async cloneGroupsWithNewIds(groups: Group[]) {
    return cloneGroupsWithNewIdsData(
      groups,
      () => this.#yieldToMain(),
      IMPORT_GROUPS_CLONE_CHUNK_SIZE,
    );
  }

  async addGroups(groups: Group[]) {
    if (!groups.length) return;
    await prependGroupsData(
      this.data,
      this.open_state,
      groups,
      () => this.#yieldToMain(),
      IMPORT_GROUPS_INSERT_CHUNK_SIZE,
    );
    this.markDataRevision();
  }

  async overwriteGroups(groups: Group[]) {
    this.data.splice(0, this.data.length);
    this.open_state = {};
    this.panelState = {};
    await this.addGroups(groups);
    if (!groups.length) {
      this.markDataRevision();
    }
  }

  async addRulesToGroup(groupIndex: number, rules: Rule[], options: { signal?: AbortSignal } = {}) {
    const group = this.data[groupIndex];
    if (!group || !rules.length) return;
    const tracker = this.tracker;
    const target = group.rules;
    const generation = this.#loadGeneration;
    const revision = this.dataRevision;
    const budget = new CooperativeBudget(4, options.signal);
    try {
      // Client IDs share the tracker's identity namespace across every group.
      // Native import creates new rules, so resolve collisions before staging.
      const reserved = new Set<string>();
      for (const existing of this.data) {
        reserved.add(existing.id);
        for (const rule of existing.rules) {
          reserved.add(rule.id);
          const pause = budget.checkpoint();
          if (pause) await pause;
        }
      }
      const incoming: Rule[] = [];
      for (const rule of rules) {
        let id = rule.id;
        while (!id || reserved.has(id))
          id = crypto.getRandomValues(new Uint32Array(1))[0].toString(16).padStart(8, "0");
        reserved.add(id);
        incoming.push(id === rule.id ? rule : { ...rule, id });
        const pause = budget.checkpoint();
        if (pause) await pause;
      }
      const prepared = await tracker.preparePrepend(target, incoming, options);
      if (options.signal?.aborted) throw new DOMException("Import cancelled", "AbortError");
      if (
        tracker !== this.tracker ||
        generation !== this.#loadGeneration ||
        revision !== this.dataRevision ||
        this.data[groupIndex]?.rules !== target
      )
        throw new Error("Configuration changed while importing rules");
      prepared.commit();
      // A request started before this atomic edit may no longer replace it.
      this.#loadGeneration++;
      this.markDataRevision();
    } finally {
      budget.close();
    }
  }

  async #yieldToMain() {
    if (typeof window === "undefined") return;
    await new Promise<void>((resolve) => {
      window.setTimeout(resolve, 0);
    });
  }
}
